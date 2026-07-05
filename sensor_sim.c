/*
 * sensor_sim.c — 下位机模拟器
 *
 * 功能：用 TCP 连接边缘网关，模拟传感器持续上报数据
 *       包格式为 8 字节定长包：data_id(4B,小端) + payload(4B,小端)
 *       前 5 包 payload 温和变化(200~240)，第 6 包跳变到 999 触发异常审计
 *
 * 编译：gcc sensor_sim.c -o sensor_sim
 * 运行：./sensor_sim [服务器IP] [端口]
 *       默认 IP=127.0.0.1 端口=8888
 *       如果网关在另一台机器：./sensor_sim 192.168.1.100 8888
 *
 * 配合项目 Makefile 编译：make sim
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// 与网关约定的 8 字节定长包格式（小端）
typedef struct {
    int data_id;   // 流水号
    int payload;   // 传感器值
} __attribute__((packed)) SensorPacket;

int main(int argc, char *argv[]) {
    const char *server_ip = "127.0.0.1";
    int port = 8888;

    if (argc >= 2) server_ip = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    printf("=== 下位机模拟器 ===\n");
    printf("目标服务器: %s:%d\n\n", server_ip, port);

    // 1. 创建 TCP 连接
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        perror("inet_pton"); return 1;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        printf("\n⚠️  连接失败，请确认 gageway 已启动: ./edge_gateway\n");
        return 1;
    }
    printf("✅ 已连接到边缘网关\n\n");

    // 2. 发送 6 个包，模拟传感器持续上报
    int values[] = {200, 210, 225, 230, 240, 999}; // 最后一个是异常跳变
    int count = sizeof(values) / sizeof(values[0]);

    for (int i = 0; i < count; i++) {
        SensorPacket pkt;
        pkt.data_id  = i + 1;
        pkt.payload  = values[i];

        ssize_t sent = send(sock, &pkt, sizeof(pkt), 0);
        if (sent != sizeof(pkt)) {
            perror("send"); break;
        }

        printf(">>> [第%02d包] data_id=%d  payload=%d\n",
               i + 1, pkt.data_id, pkt.payload);

        // 前 5 包间隔 1 秒，最后一包连发
        if (i < count - 1) sleep(1);
    }

    // 3. 等一会让网关处理完，再断开
    sleep(1);
    close(sock);
    printf("\n=== 连接已关闭，模拟结束 ===\n");
    return 0;
}
