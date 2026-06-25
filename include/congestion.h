/*
 * buffer-watermark congestion control + CNP generation
 */
#ifndef _CONGESTION_H_
#define _CONGESTION_H_

#include <stdint.h>
#include <rte_mbuf.h>

#define CNP_ETHERTYPE 0x8915
#define CNP_ROCE_ETHERTYPE 0x8915

#define BUFFER_HIGH_WATER 80
#define BUFFER_LOW_WATER 60
#define CNP_RATE_LIMIT_PPS 100000
#define CNP_MIN_INTERVAL_US 50

#pragma pack(push, 1)
struct roce_cnp_header {
    uint8_t  mgmt_class;
    uint8_t  ver_min_ver;
    uint16_t reserved1;
    uint8_t  mgmt_class_specific;
    uint8_t  reserved2[3];
    uint32_t qpn;
    uint8_t  reserved3[6];
    uint8_t  cnp_event;
    uint8_t  reserved4[2];
    uint8_t  cnp_event_specific[12];
};
#pragma pack(pop)

typedef struct {
    uint64_t total_cnp_sent;
    uint64_t cnp_by_qp[256];
    uint64_t last_cnp_time;
    uint8_t congestion_detected;  // bool really, but this works
} cnp_context_t;

typedef struct {
    uint32_t buffer_usage_percent;
    uint32_t high_water_mark;
    uint32_t low_water_mark;
    uint64_t last_rate_reduce_time;
    uint32_t current_tx_rate;

    uint32_t cnp_src_ip;
    uint32_t cnp_dst_ip;

    cnp_context_t cnp_ctx;

    uint64_t queue_overflow_count;
} congestion_control_t;

void cc_init(congestion_control_t *cc, uint32_t high_watermark, uint32_t low_watermark);
int cc_check_buffer(congestion_control_t *cc, uint32_t used_buffers, uint32_t total_buffers);
int cc_should_send_cnp(congestion_control_t *cc);
struct rte_mbuf* cc_build_cnp(congestion_control_t *cc, uint32_t qpn, struct rte_mempool *pool);

#endif
