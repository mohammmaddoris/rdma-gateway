/*
 * stats.c - 计数器实现，原子累加，周期性 dump
 */
#include "stats.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *counter_names[STAT_COUNTER_COUNT] = {
    "packets_lan_to_wan",
    "packets_wan_to_lan",
    "packets_dropped",
    "packets_passthrough",
    "ack_spoofed_sent",
    "nack_sent",
    "retransmit_sent",
    "cnp_sent",
    "write_packets_processed",
    "active_qps",
    "buffer_overflow_count",
    "arq_ack_received",
    "arq_nack_received",
    "arq_ack_sent"
};

void stat_init(gateway_stats_t *stats) {
    memset(stats, 0, sizeof(gateway_stats_t));
    LOG_INFO("stats: ready");
}

void stat_inc(gateway_stats_t *stats, stat_counter_t counter) {
    if (counter < STAT_COUNTER_COUNT) {
        __sync_fetch_and_add(&stats->counters[counter], 1);
    }
}

void stat_add(gateway_stats_t *stats, stat_counter_t counter, uint64_t value) {
    if (counter < STAT_COUNTER_COUNT) {
        __sync_fetch_and_add(&stats->counters[counter], value);
    }
}

uint64_t stat_get(gateway_stats_t *stats, stat_counter_t counter) {
    if (counter < STAT_COUNTER_COUNT) {
        return __sync_fetch_and_add(&stats->counters[counter], 0);
    }
    return 0;
}

void stat_dump(gateway_stats_t *stats) {
    LOG_INFO("========== RDMA Gateway Statistics ==========");
    for (int i = 0; i < STAT_COUNTER_COUNT; i++) {
        LOG_INFOF("  %-24s: %lu", counter_names[i], stats->counters[i]);
    }
    LOG_INFO("==============================================");
}

void stat_reset(gateway_stats_t *stats) {
    memset(stats, 0, sizeof(gateway_stats_t));
    LOG_INFO("Statistics reset");
}
