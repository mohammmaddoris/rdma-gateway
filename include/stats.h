/*
 * stats.h - 网关运行计数器，__sync 原子累加，多 lcore 安全
 */
#ifndef _STATS_H_
#define _STATS_H_

#include <stdint.h>
#include <rte_atomic.h>

typedef enum {
    STAT_PKTS_LAN_TO_WAN = 0,
    STAT_PKTS_WAN_TO_LAN,
    STAT_PKTS_DROPPED,
    STAT_PKTS_PASSTHROUGH,
    STAT_ACK_SPOOFED_SENT,
    STAT_NACK_SENT,
    STAT_RETRANSMIT_SENT,
    STAT_CNP_SENT,
    STAT_WRITE_PKTS_PROCESSED,
    STAT_ACTIVE_QPS,
    STAT_BUFFER_OVERFLOW,
    STAT_ARQ_ACK_RECEIVED,
    STAT_ARQ_NACK_RECEIVED,
    STAT_ARQ_ACK_SENT,
    STAT_COUNTER_COUNT
} stat_counter_t;

typedef struct {
    uint64_t counters[STAT_COUNTER_COUNT];
} gateway_stats_t;

void stat_init(gateway_stats_t *stats);
void stat_inc(gateway_stats_t *stats, stat_counter_t counter);
void stat_add(gateway_stats_t *stats, stat_counter_t counter, uint64_t value);
uint64_t stat_get(gateway_stats_t *stats, stat_counter_t counter);
void stat_dump(gateway_stats_t *stats);
void stat_reset(gateway_stats_t *stats);

#endif
