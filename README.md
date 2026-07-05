基于 Linux 多线程的 TCP 数据采集网关

一款使用纯 C 语言编写的 Linux 网络服务程序，用于实时接收、处理与审计物联网传感器上报数据。

## 项目演示

>【基于 Linux 多线程的 TCP 数据采集网关-哔哩哔哩】 https://b23.tv/JpeWtW7

## 技术栈

- **语言**：C（C99 / GNU C）
- **框架/库**：POSIX（pthread / epoll / socket / syslog）
- **运行环境**：Linux（Ubuntu 22.04 测试通过）

## 核心功能

1. **多设备并发接入**：使用 epoll I/O 多路复用统一管理 TCP 数据通道与 Unix Domain Socket 控制通道，支持多台下位机同时上报
2. **线程安全数据队列**：基于 pthread_mutex + pthread_cond 自实现环形缓冲区（生产者-消费者模型），满时阻塞背压、空时条件变量挂起
3. **多线程数据处理**：4 个工作线程从环形缓冲区消费数据，运行一阶低通滤波算法与偏差异常检测，异常值触发 CSV 持久化审计
4. **IPC 远程控制**：通过 Unix Domain Socket 支持外部进程查询运行时状态（STATUS）或远程关机（SHUTDOWN）
5. **信号驱动优雅退出**：使用 sigaction 捕获 SIGINT/SIGTERM，打断 epoll_wait 实现资源安全回收，无内存泄漏
6. **系统级日志**：syslog 分级日志替代 printf，符合生产代码规范

## 环境依赖

### 软件

| 依赖 | 版本要求 |
|------|---------|
| gcc | 支持 C99 标准 |
| make | 任意版本 |
| Linux 内核 | 支持 epoll（2.6+） |
| socat（可选） | 用于 IPC 测试 |

### 硬件（可选）

运行上位机仅需一台 Linux 虚拟机。下位机硬件清单：

| 硬件 | 数量 | 用途 |
|------|------|------|
| STM32F103C8T6 | 1 | 数据采集主控 |
| NodeMCU V3 (ESP8266) | 1 | WiFi TCP 透传 |
| ST-Link V2 | 1 | 程序下载 |
| CH340 USB-TTL | 1 | 调试日志输出 |

## 快速部署

```bash
# 克隆项目
git clone <仓库地址>
cd edge_gateway

# 编译
make clean && make

# 启动网关
rm -f /tmp/edge_gateway.sock
./edge_gateway

# 查看日志（另一终端）
journalctl -t edge_gateway -f

# 模拟下位机发送数据（再开一终端）
./sensor_sim
```

### IPC 控制

```bash
# 查询状态
echo STATUS | socat - UNIX-CONNECT:/tmp/edge_gateway.sock

# 远程关机
echo SHUTDOWN | socat - UNIX-CONNECT:/tmp/edge_gateway.sock
```

### Stopping

```bash
# Ctrl+C 优雅退出（运行网关的终端按）
```

## 项目结构

```
edge_gateway/
├── main.c                # 主函数：初始化 → epoll → 优雅退出
├── net_dispatcher.c/h    # epoll 网络分发器（TCP + IPC 统一管理）
├── ring_buffer.c/h       # 线程安全环形缓冲区
├── thread_pool.c/h       # 工作线程池
├── log.c/h               # syslog 日志封装
├── sensor_sim.c          # 下位机数据模拟器
├── Makefile
├── edge_audit.csv        # 异常审计文件（运行时生成）
├── README.md
└── stm32_fw/             # STM32 下位机源码（CubeMX + Keil 工程）
```




