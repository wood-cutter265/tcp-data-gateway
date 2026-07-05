#ifndef NET_DISPATCHER_H
#define NET_DISPATCHER_H

#include <signal.h>   // sig_atomic_t
#include "ring_buffer.h"

#define EDGE_IPC_PATH_DEFAULT  "/tmp/edge_gateway.sock"  // IPC socket 文件默认路径

// epoll 网络分发器：一个 epoll 实例统一管理 TCP 数据通道 + IPC 控制通道
typedef struct {
    int epoll_fd;                   // epoll 实例
    int tcp_listen_fd;              // TCP 数据通道监听 socket
    int ipc_listen_fd;              // Unix Domain Socket 控制通道监听 socket
    RingBuffer *rb;                 // 解包后投递到的环形缓冲区
    volatile sig_atomic_t *keep_running;  // 指向主控的运行标志（信号安全类型）
    int max_events;                 // epoll_wait 单次返回的最大事件数
    int max_clients;                // 支持的最大并发连接数
} NetDispatcher;

// 初始化分发器：创建 epoll、绑定 TCP 端口、创建 IPC socket 文件
NetDispatcher* net_dispatcher_init(int tcp_port,
                                   const char *ipc_path,
                                   RingBuffer *rb,
                                   volatile sig_atomic_t *keep_running);

// 进入 epoll 事件循环（生产者主循环，会一直阻塞直到 keep_running 被置 0）
void net_dispatcher_run(NetDispatcher *disp);

// 停止并销毁分发器
void net_dispatcher_shutdown(NetDispatcher *disp);

#endif // NET_DISPATCHER_H
