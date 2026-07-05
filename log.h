#ifndef LOG_H
#define LOG_H

#include <syslog.h>

// =====================================================================
// 统一分级日志宏（基于 syslog）
//
// ⚠️ 命名避坑：syslog.h 已经把 LOG_INFO/LOG_NOTICE/LOG_DEBUG/LOG_WARNING
//   等定义为 priority 常量宏（如 #define LOG_INFO 6）。如果我们的日志宏也
//   叫这些名字，会发生宏重定义 + 自引用，导致 "undeclared" 编译错误。
//   所以这里统一用【小写】名字，彻底避开 syslog.h 的名字空间。
//
// 用法：log_info("已连接 %s", ip);  log_warn("缓冲区接近满载 %d/%d", sz, cap);
//   ##__VA_ARGS__ 是 GNU 扩展，允许零参数调用：log_info("启动完成");
// =====================================================================
#define log_emerg(fmt, ...)  syslog(LOG_EMERG,   fmt, ##__VA_ARGS__)
#define log_alert(fmt, ...)  syslog(LOG_ALERT,   fmt, ##__VA_ARGS__)
#define log_crit(fmt, ...)   syslog(LOG_CRIT,    fmt, ##__VA_ARGS__)
#define log_err(fmt, ...)    syslog(LOG_ERR,     fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)   syslog(LOG_WARNING, fmt, ##__VA_ARGS__)
#define log_notice(fmt, ...) syslog(LOG_NOTICE,  fmt, ##__VA_ARGS__)
#define log_info(fmt, ...)   syslog(LOG_INFO,    fmt, ##__VA_ARGS__)
#define log_debug(fmt, ...)  syslog(LOG_DEBUG,   fmt, ##__VA_ARGS__)

// 初始化日志（程序启动时调一次）。ident 是日志里显示的程序名
void log_init(const char *ident);

#endif // LOG_H
