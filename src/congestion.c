/*
 * congestion control
 */
#include "congestion.h"
#include "wan_tunnel.h"
#include "log.h"
#include "roce_defs.h"
#include "crc32c.h"
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_malloc.h>
#include <string.h>
#include <rte_cycles.h>

extern struct rte_mempool *g_wan_pool;

void cc_init(congestion_control_t *cc, uint32_t high_watermark, uint32_t low_watermark) {
    memset(cc, 0, sizeof(congestion_control_t));
    cc->high_water_mark = high_watermark > 0 ? high_watermark : BUFFER_HIGH_WATER;
    cc->low_water_mark = low_watermark > 0 ? low_watermark : BUFFER_LOW_WATER;
    cc->current_tx_rate = 100;
    cc->last_rate_reduce_time = 0;
    cc->cnp_src_ip = wan_tunnel_src_ip;
    cc->cnp_timer_cycles = rte_get_timer_hz() * CNP_TIMER_US / 1000000;
}

int cc_check_buffer(congestion_control_t *cc, uint32_t used_buffers, uint32_t total_buffers) {
    if (total_buffers == 0) return 0;

    cc->buffer_usage_percent = (used_buffers * 100) / total_buffers;

    if (cc->buffer_usage_percent >= cc->high_water_mark) {
        cc->congestion_detected = 1;
        return 1;
    }

    if (cc->buffer_usage_percent <= cc->low_water_mark) {
        cc->congestion_detected = 0;
    }

    return cc->congestion_detected;
}

int cc_cnp_due(congestion_control_t *cc, uint64_t last_cnp_tsc, uint64_t now) {
    return (now - last_cnp_tsc) >= cc->cnp_timer_cycles;
}

struct rte_mbuf* cc_build_cnp(congestion_control_t *cc, uint32_t qpn, uint32_t dst_ip, struct rte_mempool *pool) {
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    if (!m)
        return NULL;

    /* RoCEv2 CNP: BTH (opcode 0x81) + 16B reserved + ICRC, over UDP 4791 */
    size_t cnp_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                      sizeof(struct rte_udp_hdr) + sizeof(struct roce_bth) + 16 + 4;

    char *data = rte_pktmbuf_append(m, cnp_size);
    if (!data) {
        rte_pktmbuf_free(m);
        return NULL;
    }

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
    struct roce_bth *bth = (struct roce_bth *)(udp + 1);
    uint32_t *icrc = (uint32_t *)((uint8_t *)(bth + 1) + 16);

    memset(eth, 0, cnp_size);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    build_ipv4_udp(ip, udp, cc->cnp_src_ip, dst_ip,
                   rte_cpu_to_be_16(4791), rte_cpu_to_be_16(4791),
                   sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct roce_bth) + 16 + 4,
                   sizeof(struct rte_udp_hdr) + sizeof(struct roce_bth) + 16 + 4);

    bth->opcode = ROCE_OPCODE_CNP;
    bth->pkey = rte_cpu_to_be_16(0xFFFF);
    bth->dqpn = rte_cpu_to_be_32((qpn & 0x00FFFFFF) << 8);   // same dqpn layout as the WRITE path
    /* tver_pad / f_b_se_m / pad_res / psn and the 16B payload stay zero (memset) */

    *icrc = rte_cpu_to_be_32(crc32c_calculate((uint8_t *)bth, sizeof(struct roce_bth) + 16));

    cc->cnp_ctx.total_cnp_sent++;

    m->data_len = cnp_size;
    m->pkt_len = cnp_size;

    return m;
}
