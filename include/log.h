/*
 * log.h - 轻量日志接口，支持 stdout + 文件双写
 *
 */
#ifndef _LOG_H_
#define _LOG_H_

#include <stdio.h>
#include <stdint.h>
#include <time.h>

#define LOG_BUF_SIZE 256

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} log_level_t;

void log_set_level(log_level_t level);
void log_open_file(const char *path);
void log_close_file(void);
void log_msg(log_level_t level, const char *msg);

/* Format network-byte-order IPv4 into dotted-decimal string */
const char *ip_to_str(uint32_t ip_be, char *buf, size_t buflen);

#define LOG_DEBUG(msg) log_msg(LOG_LEVEL_DEBUG, msg)
#define LOG_INFO(msg)  log_msg(LOG_LEVEL_INFO, msg)
#define LOG_WARN(msg)  log_msg(LOG_LEVEL_WARN, msg)
#define LOG_ERROR(msg) log_msg(LOG_LEVEL_ERROR, msg)

/* printf-style helpers to avoid repeated sprintf+LOG pattern */
#define LOG_DEBUGF(fmt, ...) do { \
    char _lb[LOG_BUF_SIZE]; \
    snprintf(_lb, sizeof(_lb), (fmt), ##__VA_ARGS__); \
    log_msg(LOG_LEVEL_DEBUG, _lb); \
} while (0)

#define LOG_INFOF(fmt, ...) do { \
    char _lb[LOG_BUF_SIZE]; \
    snprintf(_lb, sizeof(_lb), (fmt), ##__VA_ARGS__); \
    log_msg(LOG_LEVEL_INFO, _lb); \
} while (0)

#define LOG_WARNF(fmt, ...) do { \
    char _lb[LOG_BUF_SIZE]; \
    snprintf(_lb, sizeof(_lb), (fmt), ##__VA_ARGS__); \
    log_msg(LOG_LEVEL_WARN, _lb); \
} while (0)

#define LOG_ERRORF(fmt, ...) do { \
    char _lb[LOG_BUF_SIZE]; \
    snprintf(_lb, sizeof(_lb), (fmt), ##__VA_ARGS__); \
    log_msg(LOG_LEVEL_ERROR, _lb); \
} while (0)

#endif
