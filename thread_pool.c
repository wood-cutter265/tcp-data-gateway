#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include "thread_pool.h"
#include "log.h"

// ==========================================
#define FILTER_ALPHA 0.25f      // 工业低通滤波系数
#define AUDIT_THRESHOLD 15.0f   // 异常审计阈值（原始值与滤波值偏差超过15即判定为异常事件）
#define LOG_FILE_PATH "edge_audit.csv" // 本地持久化日志路径（CSV格式方便Excel直接打开分析）

// 算法状态与文件写入全局守护锁
static pthread_mutex_t filter_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER; // 保护文件并发写入不乱序

static float last_filtered_value = 0.0f;
static int is_first_data = 1;
// ==========================================

// 一阶低通滤波内核
float edge_low_pass_filter(int raw_value) {
    pthread_mutex_lock(&filter_mutex);
    if (is_first_data) {
        last_filtered_value = (float)raw_value;
        is_first_data = 0;
        pthread_mutex_unlock(&filter_mutex);
        return last_filtered_value;
    }
    float current_filtered = (FILTER_ALPHA * (float)raw_value) + ((1.0f - FILTER_ALPHA) * last_filtered_value);
    last_filtered_value = current_filtered;
    pthread_mutex_unlock(&filter_mutex);
    return current_filtered;
}

// 核心本地持久化审计组件
void audit_log_persistence(int data_id, int raw, float filtered, float deviation) {
    pthread_mutex_lock(&log_mutex);

    // 1. 获取 Linux 系统当前精确时间
    time_t raw_time;
    struct tm *time_info;
    char time_str[32];

    time(&raw_time);
    time_info = localtime(&raw_time);
    // 格式化时间为：2026-06-22 18:05:30
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", time_info);

    // 2. 以追加模式（"a"）打开本地 CSV 文件
    FILE *fp = fopen(LOG_FILE_PATH, "a");
    if (fp != NULL) {
        // 如果文件是新创建的，先写入表头
        fseek(fp, 0, SEEK_END);
        if (ftell(fp) == 0) {
            fprintf(fp, "时间戳,线程ID,数据ID,原始传感值,滤波平滑值,偏差值\n");
        }

        // 3. 规范化写入一行审计数据
        fprintf(fp, "%s,0x%lx,%d,%d,%.2f,%.2f\n",
                time_str, (unsigned long)pthread_self(), data_id, raw, filtered, deviation);

        // 4. 强行刷新缓冲区（fflush），确保数据瞬间掉电不丢失，直接写入磁盘
        fflush(fp);
        fclose(fp);
    } else {
        log_warn("本地审计日志打开失败");
    }

    pthread_mutex_unlock(&log_mutex);
}

// 工作线程的核心业务：跑低通滤波 + 异常审计
static void process_payload(DataNode *node) {
    float filtered_res = edge_low_pass_filter(node->payload);
    float deviation = fabsf((float)node->payload - filtered_res);

    log_info("工作线程-0x%lx 算力触发 -> 原始: %d | 滤波: %.2f | 偏差: %.2f",
              (unsigned long)pthread_self(), node->payload, filtered_res, deviation);

    if (deviation >= AUDIT_THRESHOLD) {
        log_warn("系统警报-安全审计：检测到硬件突变毛刺/异常！正在强制持久化存盘 ID=%d", node->data_id);
        audit_log_persistence(node->data_id, node->payload, filtered_res, deviation);
    }
}

// 工作线程入口：循环从环形缓冲区消费数据，直到收到停机信号
void* worker_routine(void *arg) {
    ThreadPool *pool = (ThreadPool*)arg;

    for (;;) {
        DataNode node;
        // pop 返回 0 表示缓冲区已停机且取空，本线程该下班了
        if (ring_buffer_pop(pool->rb, &node) == 0) {
            break;
        }
        process_payload(&node);
    }

    return NULL;
}

// 初始化线程池：创建 N 个常驻工作线程，各自阻塞在 ring_buffer_pop 上
ThreadPool* thread_pool_init(int thread_count, RingBuffer *rb) {
    ThreadPool *pool = (ThreadPool*)malloc(sizeof(ThreadPool));
    if (!pool) return NULL;

    pool->rb = rb;
    pool->thread_count = thread_count;
    pool->threads = (pthread_t*)malloc(thread_count * sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    for (int i = 0; i < thread_count; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_routine, pool) != 0) {
            log_warn("工作线程 %d 创建失败", i);
            pool->thread_count = i; // 已经成功创建的数量
            break;
        }
    }

    log_info("线程池就绪，工作线程数=%d", pool->thread_count);
    return pool;
}

// 销毁线程池：join 所有工作线程并释放内存
void thread_pool_destroy(ThreadPool *pool) {
    if (!pool) return;

    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    free(pool->threads);
    free(pool);
    log_info("=== 自实现线程池资源安全回收完毕 ===");
}
