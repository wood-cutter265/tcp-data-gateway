#include "log.h"

// 初始化 syslog
// LOG_PID:   每条日志带上进程 PID
// LOG_NDELAY: 立即打开 syslog 连接（不延迟到第一条日志）
void log_init(const char *ident) {
    openlog(ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);
}
