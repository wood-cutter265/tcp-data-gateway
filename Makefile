# ==========================================
# 工业级边缘网关项目 Makefile 管理脚本
# ==========================================

# 1. 定义编译器
CC = gcc

# 2. 定义编译选项 (C Flags)
# -Wall: 开启所有语法警告
# -O2: 开启二级优化，提升边缘端运行效率
# -g:  生成调试信息，方便 gdb 调试与 valgrind 内存检测
CFLAGS = -Wall -O2 -g -pthread

# 3. 定义链接选项 (Linker Flags)
# 链接数学库（fabsf）和 POSIX 线程库
LIBS = -lpthread -lm

# 4. 定义最终生成的可执行文件名
TARGET = edge_gateway

# 5. 源文件清单（P1：加入 net_dispatcher 网络分发器 + log 日志模块）
SRCS = main.c ring_buffer.c thread_pool.c net_dispatcher.c log.c
OBJS = $(SRCS:.c=.o)

# ==========================================
# 编译规则 (Rules)
# ==========================================

# 终极目标 (All)：告诉 Makefile 我们的最终目的是生成 TARGET
all: $(TARGET) sensor_sim

# 规则 A：如何把所有的 .o 文件链接成最终的可执行程序
# $@ 代表目标文件 (即 TARGET)
# $^ 代表所有依赖文件 (即 OBJS)
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LIBS)
	@echo "====== 编译成功！完全体二进制文件 [$(TARGET)] 已就绪 ======"

# 模拟器（下位机）编译目标
sensor_sim: sensor_sim.c
	$(CC) -Wall -O2 -g sensor_sim.c -o sensor_sim
	@echo "====== 下位机模拟器 [sensor_sim] 已就绪 ======"

# 规则 B：如何把单个 .c 源文件编译成 .o 目标文件 (模式规则)
# $< 代表第一个依赖文件 (即对应的 .c 文件)
# -c 参数告诉 GCC 只编译不链接，这是实现【增量编译】的核心
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 规则 C：清理伪目标 (Clean)
# 允许用户执行 `make clean` 来清除所有编译出来的中间垃圾文件和可执行程序
.PHONY: clean
clean:
	rm -f $(OBJS) $(TARGET) sensor_sim
	@echo "====== 清理完毕！项目已恢复纯净状态 ======"
