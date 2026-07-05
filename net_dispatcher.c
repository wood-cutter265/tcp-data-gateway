#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "net_dispatcher.h"
#include "log.h"

#define TCP_BACKLOG      16     // TCP 全连接队列长度
#define MAX_EVENTS       32     // epoll_wait 单次最大返回事件
#define MAX_CLIENTS      64     // 最大并发连接数
#define RECV_BUF_SIZE    4096
#define IPC_REPLY_SIZE   256

// 用 fd 的最高位区分两类 socket，方便在 epoll 回调里分流处理
// epoll_data_t 是 u64，我们存 fd，并约定：
//   - fd 本身          -> TCP 客户端（数据通道）
//   - fd | IPC_FD_FLAG -> IPC 客户端（控制通道）
// 这样不用维护额外的 fd->类型 映射表，面试时可讲这个 trick
#define IPC_FD_FLAG  0x40000   // 取个高位，普通 fd 远小于此值

// =====================================================================
// 工具函数：把 socket 设为非阻塞（epoll + 多 client 的标配）
// =====================================================================
static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// =====================================================================
// 工具函数：把 fd 加入 epoll 监听
// =====================================================================
static int ep_add(int epfd, int fd, uint32_t events, uint64_t data) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.u64 = data;   // 把 fd（带类型标记）塞进 data
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

// =====================================================================
// 创建 TCP 监听 socket
// =====================================================================
static int create_tcp_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { log_err("TCP socket 创建失败"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_err("TCP bind 失败");
        close(fd);
        return -1;
    }
    if (listen(fd, TCP_BACKLOG) < 0) {
        log_err("TCP listen 失败");
        close(fd);
        return -1;
    }
    return fd;
}

// =====================================================================
// 创建 Unix Domain Socket 监听 socket（IPC 控制通道）
// 这是 P1 的核心新增：外部进程可通过它查询运行时状态、远程关机
// =====================================================================
static int create_ipc_listener(const char *path) {
    // 先清理可能残留的 socket 文件（否则 bind 会报 ADDRINUSE）
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { log_err("IPC socket 创建失败"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_err("IPC bind 失败");
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        log_err("IPC listen 失败");
        close(fd);
        return -1;
    }
    return fd;
}

// =====================================================================
// 处理 TCP 数据：半包/粘包容错，按 8 字节定长包解包后投递进环形缓冲区
// =====================================================================
static void handle_tcp_data(NetDispatcher *disp, int client_fd) {
    char buf[RECV_BUF_SIZE];
    while (1) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            // 按定长 8 字节包切分（半包丢弃、粘包拆分）
            int pkts = n / 8;
            for (int i = 0; i < pkts; i++) {
                int data_id, payload;
                memcpy(&data_id, buf + i * 8, 4);
                memcpy(&payload, buf + i * 8 + 4, 4);

                log_info("[收] 解包 ID=%d payload=%d", data_id, payload);

                DataNode node = { .data_id = data_id, .payload = payload };
                // push 满载会阻塞 → 天然背压，保护下游工作线程
                ring_buffer_push(disp->rb, node);
            }
        } else if (n == 0) {
            // 对端关闭
            log_info("TCP 客户端 fd=%d 主动断开", client_fd);
            epoll_ctl(disp->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
            close(client_fd);
            break;
        } else {
            // EAGAIN/EWOULDBLOCK：非阻塞模式下数据读完了，正常退出循环
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            // 真错误
            log_warn("TCP recv 错误 fd=%d: %s", client_fd, strerror(errno));
            epoll_ctl(disp->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
            close(client_fd);
            break;
        }
    }
}

// =====================================================================
// 处理 IPC 控制命令：STATUS（查积压量）/ SHUTDOWN（远程优雅关机）
// 外部用 `socat - UNIX-CONNECT:/tmp/edge.sock` 或自写小客户端连进来
// =====================================================================
static void handle_ipc_command(NetDispatcher *disp, int client_fd) {
    char cmd[64] = {0};
    ssize_t n = recv(client_fd, cmd, sizeof(cmd) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }

    // 去掉换行
    while (n > 0 && (cmd[n-1] == '\n' || cmd[n-1] == '\r')) cmd[--n] = '\0';

    char reply[IPC_REPLY_SIZE];

	    if (strcmp(cmd, "STATUS") == 0) {
	        int sz = ring_buffer_size(disp->rb);
	        snprintf(reply, sizeof(reply),
	                 "OK buffer_size=%d thread_count=4 status=running\n", sz);
	        (void)write(client_fd, reply, strlen(reply));
	        log_notice("IPC STATUS 查询，积压=%d", sz);
	    } else if (strcmp(cmd, "SHUTDOWN") == 0 || strcmp(cmd, "exit") == 0) {
	        strncpy(reply, "OK shutting down\n", sizeof(reply));
	        (void)write(client_fd, reply, strlen(reply));
	        log_notice("IPC 收到远程关机命令");
	        // 1. 让 epoll 子线程的事件循环退出
	        *(disp->keep_running) = false;
        // 2. 给自己进程投递 SIGTERM（进程级，主线程信号处理函数会置 keep_running=0）
        //    这比 raise(SIGTERM)（线程定向）更可靠，因为信号处理函数能打断
        //    epoll_wait（无 SA_RESTART），让事件循环立即退出
        kill(getpid(), SIGTERM);
	    } else {
	        snprintf(reply, sizeof(reply), "ERR unknown command: %s\n", cmd);
	        (void)write(client_fd, reply, strlen(reply));
	        log_warn("IPC 未知命令: %s", cmd);
	    }
    close(client_fd);  // IPC 采用一问一答短连接，回完即关
}

// =====================================================================
// epoll 主事件循环（Reactor 核心入口）
// =====================================================================
void net_dispatcher_run(NetDispatcher *disp) {
    struct epoll_event events[MAX_EVENTS];
    log_info("epoll 事件循环启动，TCP端口+IPC通道统一调度");

    while (*(disp->keep_running)) {
        // -1 阻塞等待；收到信号 EINTR 就重试
        int n = epoll_wait(disp->epoll_fd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_warn("epoll_wait 错误: %s", strerror(errno));
            break;
        }

	        for (int i = 0; i < n; i++) {
	            // 从 u64 取出原始数据：高 32 位存着 IPC 标记，低 32 位是 fd
	            // 绝不能直接用 events[i].data.fd（int 类型会把高位置0/1搞混）
	            uint64_t raw = events[i].data.u64;
	            int fd = (int)(raw & ~(uint64_t)IPC_FD_FLAG);  // 掩掉标志位得到真实 fd
	            int is_ipc = (raw & IPC_FD_FLAG) ? 1 : 0;

            if (fd == disp->tcp_listen_fd) {
                // —— TCP 新连接 ——
                struct sockaddr_in caddr;
                socklen_t clen = sizeof(caddr);
                int cfd = accept(disp->tcp_listen_fd, (struct sockaddr*)&caddr, &clen);
                if (cfd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        log_warn("TCP accept 失败: %s", strerror(errno));
                    continue;
                }
                set_nonblock(cfd);
                ep_add(disp->epoll_fd, cfd, EPOLLIN, (uint64_t)cfd);  // 不带 IPC 标记
                log_info("[连] 新 TCP 连接 fd=%d 来自 %s:%d",
                         cfd, inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));

            } else if (fd == disp->ipc_listen_fd) {
                // —— IPC 新连接 ——
                int cfd = accept(disp->ipc_listen_fd, NULL, NULL);
                if (cfd < 0) continue;
                ep_add(disp->epoll_fd, cfd, EPOLLIN, (uint64_t)cfd | IPC_FD_FLAG);  // 带 IPC 标记
                log_info("IPC 控制连接 fd=%d 接入", cfd);

            } else if (is_ipc) {
                // —— IPC 数据（控制命令）——
                handle_ipc_command(disp, fd);

            } else {
                // —— TCP 数据 ——
                handle_tcp_data(disp, fd);
            }
        }
    }
    log_info("epoll 事件循环退出");
}

// =====================================================================
// 初始化
// =====================================================================
NetDispatcher* net_dispatcher_init(int tcp_port,
                                   const char *ipc_path,
                                   RingBuffer *rb,
                                   volatile sig_atomic_t *keep_running) {
    NetDispatcher *disp = (NetDispatcher*)calloc(1, sizeof(NetDispatcher));
    if (!disp) return NULL;

    disp->rb = rb;
    disp->keep_running = keep_running;
    disp->max_events = MAX_EVENTS;
    disp->max_clients = MAX_CLIENTS;

    // 1. 创建 epoll 实例
    disp->epoll_fd = epoll_create1(0);
    if (disp->epoll_fd < 0) {
        log_err("epoll_create1 失败");
        free(disp);
        return NULL;
    }

    // 2. 创建 TCP 监听 socket 并加入 epoll
    disp->tcp_listen_fd = create_tcp_listener(tcp_port);
    if (disp->tcp_listen_fd < 0) { free(disp); return NULL; }
    set_nonblock(disp->tcp_listen_fd);
    ep_add(disp->epoll_fd, disp->tcp_listen_fd, EPOLLIN, disp->tcp_listen_fd);

    // 3. 创建 IPC 监听 socket 并加入 epoll
    //    注意：监听 socket 不带 IPC_FD_FLAG，否则事件循环里
    //    `fd == disp->ipc_listen_fd` 永远不成立，IPC 通道会失效。
    //    只有 IPC 客户端 fd 才打标记（见 net_dispatcher_run 的 accept 分支）。
    disp->ipc_listen_fd = create_ipc_listener(ipc_path);
    if (disp->ipc_listen_fd < 0) { free(disp); return NULL; }
    set_nonblock(disp->ipc_listen_fd);
    ep_add(disp->epoll_fd, disp->ipc_listen_fd, EPOLLIN, disp->ipc_listen_fd);

    log_info("网络分发器初始化完成 TCP端口=%d IPC路径=%s", tcp_port, ipc_path);
    return disp;
}

// =====================================================================
// 关闭并销毁
// =====================================================================
void net_dispatcher_shutdown(NetDispatcher *disp) {
    if (!disp) return;
    if (disp->epoll_fd >= 0)     close(disp->epoll_fd);
    if (disp->tcp_listen_fd >= 0) close(disp->tcp_listen_fd);
    if (disp->ipc_listen_fd >= 0) {
        close(disp->ipc_listen_fd);
        // IPC socket 文件留到这步删，避免删了又重建导致正在连的客户端报错
        unlink(EDGE_IPC_PATH_DEFAULT);
    }
    free(disp);
    log_info("网络分发器资源已回收");
}
