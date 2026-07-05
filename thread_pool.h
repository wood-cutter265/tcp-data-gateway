#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include "ring_buffer.h"

// 自实现线程池：N 个常驻工作线程，共享消费一个环形缓冲区
typedef struct {
    pthread_t *threads;     // 工作线程 ID 数组
    int thread_count;       // 工作线程数量
    RingBuffer *rb;         // 绑定的环形缓冲区（任务队列本体）
} ThreadPool;

// 对外接口
ThreadPool* thread_pool_init(int thread_count, RingBuffer *rb);
void thread_pool_destroy(ThreadPool *pool);

#endif // THREAD_POOL_H
