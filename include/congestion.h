/*
 * buffer-watermark congestion + DCQCN-style per-flow CNP pacing
 */
#ifndef _CONGESTION_H_
#define _CONGESTION_H_

#include <stdint.h>
#include <rte_mbuf.h>

#define BUFFER_HIGH_WATER 80
#define BUFFER_LOW_WATER 60
#define CNP_RATE_LIMIT_PPS 100000
#define CNP_TIMER_US 50   // DCQCN: min gap between CNPs of one flow

typedef struct {
    uint64_t total_cnp_sent;
    uint8_t congestion_detected;  // bool really, but this works
} cnp_context_t;

typedef struct {
    uint32_t buffer_usage_percent;
    uint32_t high_water_mark;
    uint32_t low_water_mark;
    uint64_t last_rate_reduce_time;
    uint32_t current_tx_rate;

    uint32_t cnp_src_ip;
    uint64_t cnp_timer_cycles;   // CNP_TIMER_US in tsc cycles

    cnp_context_t cnp_ctx;

    uint64_t queue_overflow_count;
} congestion_control_t;

void cc_init(congestion_control_t *cc, uint32_t high_watermark, uint32_t low_watermark);
int cc_check_buffer(congestion_control_t *cc, uint32_t used_buffers, uint32_t total_buffers);
int cc_cnp_due(congestion_control_t *cc, uint64_t last_cnp_tsc, uint64_t now);
struct rte_mbuf* cc_build_cnp(congestion_control_t *cc, uint32_t qpn, uint32_t dst_ip, struct rte_mempool *pool);

#endif
