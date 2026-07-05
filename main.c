#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include "ring_buffer.h"
#include "thread_pool.h"
#include "net_dispatcher.h"
#include "log.h"

// ========================================================================
// 全局运行标志：
// sig_atomic_t 是 C 标准保证的在信号处理函数内可安全读写的最小类型。
// SA_RESTART 不设置 → 阻塞中的 epoll_wait 收到信号时返回 EINTR，
// 事件循环立即回到 while(keep_running) 检查，发现为 0 就退出。
// ========================================================================
static volatile sig_atomic_t keep_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    // 信号处理函数里只能做这件事——给 volatile sig_atomic_t 赋值
    keep_running = 0;
}

static void* dispatcher_thread(void *arg) {
    net_dispatcher_run((NetDispatcher*)arg);
    return NULL;
}

int main(void) {
    log_init("edge_gateway");
    log_notice("=== 边缘计算数据流网关启动（epoll + 线程池 + IPC）===");

    // -------- 1. 初始化三大核心组件 --------
    RingBuffer *rb = ring_buffer_init(5);
    if (!rb) { log_err("环形缓冲区初始化失败"); return 1; }

    ThreadPool *pool = thread_pool_init(4, rb);
    if (!pool) { log_err("线程池初始化失败"); ring_buffer_free(rb); return 1; }

    NetDispatcher *disp = net_dispatcher_init(
        8888,
        EDGE_IPC_PATH_DEFAULT,
        rb,
        &keep_running);
    if (!disp) {
        log_err("网络分发器初始化失败");
        ring_buffer_stop(rb);
        thread_pool_destroy(pool);
        ring_buffer_free(rb);
        return 1;
    }

    // -------- 2. 安装信号处理函数（Ctrl+C / kill）--------
    // 关键：不设 SA_RESTART，让 epoll_wait 等阻塞调用被信号打断并返回 EINTR
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;            // 默认无 SA_RESTART → 信号会打断 syscall
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // -------- 3. epoll 事件循环在子线程跑；主线程只管等它结束 --------
    pthread_t disp_tid;
    pthread_create(&disp_tid, NULL, dispatcher_thread, disp);

    log_info("系统就绪。控制方式：");
    log_info("  - 本地：Ctrl+C 优雅退出");
    log_info("  - 远程状态：echo STATUS | socat - UNIX-CONNECT:%s", EDGE_IPC_PATH_DEFAULT);
    log_info("  - 远程关机：echo SHUTDOWN | socat - UNIX-CONNECT:%s", EDGE_IPC_PATH_DEFAULT);

    // -------- 4. 主线程阻塞在 join；epoll 退出后从这里继续 --------
    pthread_join(disp_tid, NULL);

    printf("\n>>> 开始优雅退出\n");
    log_notice("开始优雅退出");

    // -------- 5. 优雅退出 --------
    ring_buffer_stop(rb);
    thread_pool_destroy(pool);
    net_dispatcher_shutdown(disp);
    ring_buffer_free(rb);

    printf(">>> 边缘网关安全退出，资源已全部回收\n");
    log_notice("=== 边缘网关安全退出，资源已全部回收 ===");
    closelog();
    return 0;
}
