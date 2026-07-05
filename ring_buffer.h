#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <pthread.h>

// 1. 定义多通道传感数据包：流水号 + 核心载荷
typedef struct {
    int data_id;           // 数据流水号（用于防丢包审计）
    int payload;           // 核心载荷（如模拟采集到的温度、压力值）
} DataNode;

// 2. 线程安全循环队列结构体
typedef struct {
    DataNode *buffer;       // 指向存放数据节点的连续内存阵列
    int capacity;           // 缓冲区最大容量上限
    int head;               // 队头指针（消费者从这里捞数据）
    int tail;               // 队尾指针（生产者往这里塞数据）
    int size;               // 当前缓冲区中积压的数据包总数
    int stop;               // 停机标志：置1后唤醒所有阻塞线程并让 pop 立即返回

    // POSIX 多线程同步核心组件
    pthread_mutex_t mutex;   // 互斥锁：确保同一时刻只有一个线程能改动队列指针
    pthread_cond_t not_full; // 条件变量：队列未满（用于唤醒因“满”而挂起的生产者）
    pthread_cond_t not_empty;// 条件变量：队列不空（用于唤醒因“空”而挂起的工作线程）
} RingBuffer;

// 3. 对外开放的硬核 API 接口声明
RingBuffer* ring_buffer_init(int capacity);
void ring_buffer_free(RingBuffer *rb);
void ring_buffer_push(RingBuffer *rb, DataNode node);          // 生产者：塞数据（满则阻塞）
int  ring_buffer_pop(RingBuffer *rb, DataNode *out);           // 消费者：取数据，返回1成功/0已停机且空
void ring_buffer_stop(RingBuffer *rb);                         // 停机：广播唤醒所有等待中的线程
int  ring_buffer_size(RingBuffer *rb);                         // P1新增：查询当前积压数据量（供IPC状态查询用）

#endif // RING_BUFFER_H
