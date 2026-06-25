/*
 * congestion control
 */
#include "congestion.h"
#include "wan_tunnel.h"
#include "log.h"
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
    cc->cnp_dst_ip = 0;
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

int cc_should_send_cnp(congestion_control_t *cc) {
    if (!cc->congestion_detected) {
        return 0;
    }

    uint64_t current_time = rte_rdtsc();
    uint64_t interval_cycles = rte_get_timer_hz() * CNP_MIN_INTERVAL_US / 1000000;

    if (current_time - cc->cnp_ctx.last_cnp_time < interval_cycles) {
        return 0;
    }

    return 1;
}

struct rte_mbuf* cc_build_cnp(congestion_control_t *cc, uint32_t qpn, struct rte_mempool *pool) {
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    if (!m)
        return NULL;

    size_t cnp_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                     sizeof(struct rte_udp_hdr) + sizeof(struct roce_cnp_header);

    char *data = rte_pktmbuf_append(m, cnp_size);
    if (!data) {
        rte_pktmbuf_free(m);
        return NULL;
    }

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
    struct roce_cnp_header *cnp = (struct roce_cnp_header *)(udp + 1);

    memset(eth, 0, cnp_size);
    eth->ether_type = rte_cpu_to_be_16(CNP_ETHERTYPE);

    build_ipv4_udp(ip, udp, cc->cnp_src_ip, cc->cnp_dst_ip,
                   rte_cpu_to_be_16(4791), rte_cpu_to_be_16(4791),
                   sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct roce_cnp_header),
                   sizeof(struct rte_udp_hdr) + sizeof(struct roce_cnp_header));

    cnp->mgmt_class = 0x06;
    cnp->ver_min_ver = 0x01;
    cnp->reserved1 = 0;
    cnp->mgmt_class_specific = 0;
    memset(cnp->reserved2, 0, 3);
    cnp->qpn = rte_cpu_to_be_32(qpn);
    memset(cnp->reserved3, 0, 6);
    cnp->cnp_event = 0x01;
    memset(cnp->reserved4, 0, 2);
    memset(cnp->cnp_event_specific, 0, 12);

    cc->cnp_ctx.total_cnp_sent++;
    cc->cnp_ctx.last_cnp_time = rte_rdtsc();

    m->data_len = cnp_size;
    m->pkt_len = cnp_size;

    return m;
}
