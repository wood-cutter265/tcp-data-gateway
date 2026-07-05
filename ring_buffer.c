#include <stdio.h>
#include <stdlib.h>
#include "ring_buffer.h"

// 初始化缓冲区：开辟动态堆内存，激活系统锁资源
RingBuffer* ring_buffer_init(int capacity) {
    RingBuffer *rb = (RingBuffer*)malloc(sizeof(RingBuffer));
    if (!rb) return NULL;

    rb->buffer = (DataNode*)malloc(capacity * sizeof(DataNode));
    if (!rb->buffer) { free(rb); return NULL; }

    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->size = 0;
    rb->stop = 0;

    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->not_full, NULL);
    pthread_cond_init(&rb->not_empty, NULL);

    return rb;
}

// 往缓冲区推入数据（生产者核心控制）
void ring_buffer_push(RingBuffer *rb, DataNode node) {
    pthread_mutex_lock(&rb->mutex); // 进临界区上锁

    while (rb->size == rb->capacity && !rb->stop) {
        // 满了就交出锁、睡觉，挂载在 not_full 队列上
        pthread_cond_wait(&rb->not_full, &rb->mutex);
    }

    // 停机期间不再接收新数据
    if (rb->stop) {
        pthread_mutex_unlock(&rb->mutex);
        return;
    }

    rb->buffer[rb->tail] = node;
    rb->tail = (rb->tail + 1) % rb->capacity; // 环形取模走向机制
    rb->size++;

    pthread_cond_signal(&rb->not_empty); // 唤醒可能在因“空”睡觉的工作线程

    pthread_mutex_unlock(&rb->mutex); // 出临界区解锁
}

// 从缓冲区取出数据（消费者核心控制）
// 返回值：1 = 成功取到一个节点；0 = 缓冲区已停机且取空，调用方应退出
int ring_buffer_pop(RingBuffer *rb, DataNode *out) {
    pthread_mutex_lock(&rb->mutex);

    while (rb->size == 0 && !rb->stop) {
        // 空了就交出锁、睡觉，挂载在 not_empty 队列上
        pthread_cond_wait(&rb->not_empty, &rb->mutex);
    }

    // 已经停机且队列空了：唤醒其它还在睡的兄弟，然后退出
    if (rb->size == 0 && rb->stop) {
        pthread_cond_broadcast(&rb->not_empty);
        pthread_mutex_unlock(&rb->mutex);
        return 0;
    }

    *out = rb->buffer[rb->head];
    rb->head = (rb->head + 1) % rb->capacity; // 队头环形递增
    rb->size--;

    pthread_cond_signal(&rb->not_full); // 取走了一个空位，赶紧唤醒可能在憋着的生产者

    pthread_mutex_unlock(&rb->mutex);

    return 1;
}

// 停机：把 stop 标志立起来，然后广播唤醒所有阻塞在 push/pop 上的线程
void ring_buffer_stop(RingBuffer *rb) {
    pthread_mutex_lock(&rb->mutex);
    rb->stop = 1;
    pthread_cond_broadcast(&rb->not_full);
    pthread_cond_broadcast(&rb->not_empty);
    pthread_mutex_unlock(&rb->mutex);
}

// 查询当前缓冲区积压数据量（供 IPC 状态查询用，线程安全快照）
int ring_buffer_size(RingBuffer *rb) {
    pthread_mutex_lock(&rb->mutex);
    int sz = rb->size;
    pthread_mutex_unlock(&rb->mutex);
    return sz;
}

// 销毁缓冲区：严防内存泄漏
void ring_buffer_free(RingBuffer *rb) {
    if (rb) {
        free(rb->buffer);
        pthread_mutex_destroy(&rb->mutex);
        pthread_cond_destroy(&rb->not_full);
        pthread_cond_destroy(&rb->not_empty);
        free(rb);
    }
}
