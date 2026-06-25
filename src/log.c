/*
 * log backend: stdout + optional file
 */
#include "log.h"
#include <string.h>
#include <arpa/inet.h>
#include <pthread.h>

static log_level_t current_level = LOG_LEVEL_INFO;
static FILE *log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_strings[] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};

static const char *level_colors[] = {
    "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m"
};

const char *ip_to_str(uint32_t ip_be, char *buf, size_t buflen) {
    uint32_t ip = ntohl(ip_be);
    snprintf(buf, buflen, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

void log_set_level(log_level_t level) {
    current_level = level;
}

void log_open_file(const char *path) {
    pthread_mutex_lock(&log_mutex);
    
    if (log_file) {
        fclose(log_file);
    }
    
    log_file = fopen(path, "a");
    if (log_file) {
        setvbuf(log_file, NULL, _IOLBF, 0);
        fprintf(stdout, "Log file opened: %s\n", path);
        fflush(stdout);
    } else {
        fprintf(stderr, "Failed to open log file: %s\n", path);
    }
    
    pthread_mutex_unlock(&log_mutex);
}

void log_close_file(void) {
    pthread_mutex_lock(&log_mutex);
    
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    
    pthread_mutex_unlock(&log_mutex);
}

void log_msg(log_level_t level, const char *msg) {
    if (level < current_level) return;

    pthread_mutex_lock(&log_mutex);
    
    time_t t = time(NULL);
    struct tm lt = *localtime(&t);

    fprintf(stderr, "%s[%04d-%02d-%02d %02d:%02d:%02d] [%s] %s\x1b[0m\n", 
            level_colors[level], lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
            lt.tm_hour, lt.tm_min, lt.tm_sec, 
            level_strings[level], msg);

    if (log_file) {
        fprintf(log_file, "[%04d-%02d-%02d %02d:%02d:%02d] [%s] %s\n", 
                lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                lt.tm_hour, lt.tm_min, lt.tm_sec, 
                level_strings[level], msg);
    }
    
    pthread_mutex_unlock(&log_mutex);
}