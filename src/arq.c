/*
 * ARQ retransmit layer
 */
#include "arq.h"
#include "log.h"
#include "wan_tunnel.h"
#include "common.h"
#include <rte_malloc.h>
#include <string.h>
#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

#define ARQ_MAGIC 0x41525130

void arq_mgr_init(arq_manager_t *mgr) {
    memset(mgr, 0, sizeof(arq_manager_t));
    rte_spinlock_init(&mgr->global_lock);
}

static arq_context_t *arq_find(arq_manager_t *mgr, uint32_t qpn) {
    for (uint32_t i = 0; i < MAX_QP_CTX; i++) {
        if (mgr->contexts[i].qpn == qpn) {
            return &mgr->contexts[i];
        }
    }
    return NULL;
}

arq_context_t* arq_get_or_create(arq_manager_t *mgr, uint32_t qpn) {
    rte_spinlock_lock(&mgr->global_lock);

    arq_context_t *ctx = arq_find(mgr, qpn);
    if (ctx) {
        rte_spinlock_unlock(&mgr->global_lock);
        return ctx;
    }

    for (uint32_t i = 0; i < MAX_QP_CTX; i++) {
        if (mgr->contexts[i].qpn == 0) {
            memset(&mgr->contexts[i], 0, sizeof(arq_context_t));
            mgr->contexts[i].qpn = qpn;
            rte_spinlock_init(&mgr->contexts[i].lock);
            rte_spinlock_unlock(&mgr->global_lock);
            LOG_INFOF("Created ARQ context for QPN: 0x%x", qpn);
            return &mgr->contexts[i];
        }
    }

    rte_spinlock_unlock(&mgr->global_lock);
    LOG_ERROR("Failed to create ARQ context: max capacity reached");
    return NULL;
}

arq_context_t* arq_lookup(arq_manager_t *mgr, uint32_t qpn) {
    rte_spinlock_lock(&mgr->global_lock);
    arq_context_t *ctx = arq_find(mgr, qpn);
    rte_spinlock_unlock(&mgr->global_lock);
    return ctx;
}

int arq_send_pkt(arq_context_t *arq, struct rte_mbuf *m, uint32_t psn, uint8_t segment_type) {
    rte_spinlock_lock(&arq->lock);

    uint32_t idx = arq->send_next % WINDOW_SIZE;
    if (arq->send_window[idx].in_flight) {
        rte_spinlock_unlock(&arq->lock);
        return -1;
    }

    arq->send_window[idx].psn = psn;
    arq->send_window[idx].mbuf = m;
    arq->send_window[idx].send_time = rte_rdtsc();
    arq->send_window[idx].retry_count = 0;
    arq->send_window[idx].segment_type = segment_type;
    arq->send_window[idx].in_flight = 1;

    arq->send_next++;
    if (arq->send_next - arq->send_base > arq->send_max) {
        arq->send_max = arq->send_next - arq->send_base;
    }

    arq->total_sent++;
    rte_spinlock_unlock(&arq->lock);
    return 0;
}

int arq_handle_ack(arq_context_t *arq, uint32_t ack_psn) {
    rte_spinlock_lock(&arq->lock);

    while (arq->send_base < arq->send_next) {
        uint32_t idx = arq->send_base % WINDOW_SIZE;
        uint32_t diff = psn_forward_dist(arq->send_window[idx].psn, ack_psn);

        if (diff >= 0x800000) {
            break;
        }

        if (arq->send_window[idx].mbuf) {
            rte_pktmbuf_free(arq->send_window[idx].mbuf);
            arq->send_window[idx].mbuf = NULL;
        }
        arq->send_window[idx].in_flight = 0;
        arq->send_base++;
        arq->total_ack_received++;
    }

    rte_spinlock_unlock(&arq->lock);
    return 0;
}

struct rte_mbuf* arq_build_ctrl_msg(uint32_t qpn, uint8_t type, uint32_t psn,
                                 uint32_t dst_ip, uint32_t *psn_list, uint32_t count,
                                 struct rte_mempool *pool) {
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    if (!m)
        return NULL;

    size_t msg_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                      sizeof(struct rte_udp_hdr) + sizeof(arq_control_msg_t);

    char *data = rte_pktmbuf_append(m, msg_size);
    if (!data) {
        rte_pktmbuf_free(m);
        return NULL;
    }

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
    arq_control_msg_t *msg = (arq_control_msg_t *)(udp + 1);

    struct rte_ether_addr dst_mac;
    if (arp_cache_lookup(dst_ip, &dst_mac) != 0) {
        memset(&dst_mac, 0xFF, sizeof(dst_mac));
    }
    rte_ether_addr_copy(&dst_mac, &eth->d_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    build_ipv4_udp(ip, udp, wan_tunnel_src_ip, dst_ip,
                   rte_cpu_to_be_16(ARQ_CTRL_PORT), rte_cpu_to_be_16(ARQ_CTRL_PORT),
                   sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(arq_control_msg_t),
                   sizeof(struct rte_udp_hdr) + sizeof(arq_control_msg_t));

    msg->magic = rte_cpu_to_be_32(ARQ_MAGIC);
    msg->type = type;
    msg->qpn = rte_cpu_to_be_32(qpn);
    msg->psn = rte_cpu_to_be_32(psn);
    msg->count = rte_cpu_to_be_32(count);
    if (psn_list && count > 0) {
        for (uint32_t i = 0; i < count && i < MAX_RETX_BURST; i++) {
            msg->psn_list[i] = rte_cpu_to_be_32(psn_list[i]);
        }
    }

    m->data_len = msg_size;
    m->pkt_len = msg_size;

    LOG_DEBUGF("ARQ control msg built: type=%u, QPN=0x%x, PSN=%u", type, qpn, psn);
    return m;
}

int arq_parse_ctrl_msg(struct rte_mbuf *m, arq_control_msg_t *msg) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
    arq_control_msg_t *msg_hdr = (arq_control_msg_t *)(udp + 1);

    if (rte_be_to_cpu_32(msg_hdr->magic) != ARQ_MAGIC) {
        LOG_ERRORF("Invalid ARQ control msg magic: 0x%x", rte_be_to_cpu_32(msg_hdr->magic));
        return -1;
    }

    msg->magic = rte_be_to_cpu_32(msg_hdr->magic);
    msg->type = msg_hdr->type;
    msg->qpn = rte_be_to_cpu_32(msg_hdr->qpn);
    msg->psn = rte_be_to_cpu_32(msg_hdr->psn);
    msg->count = rte_be_to_cpu_32(msg_hdr->count);
    for (uint32_t i = 0; i < msg->count && i < MAX_RETX_BURST; i++) {
        msg->psn_list[i] = rte_be_to_cpu_32(msg_hdr->psn_list[i]);
    }

    return 0;
}

int arq_handle_nack(arq_context_t *arq, uint32_t nack_psn, uint16_t wan_port) {
    rte_spinlock_lock(&arq->lock);
    uint32_t retrans_count = 0;
    uint32_t retrans_idx[MAX_RETX_BURST];
    uint32_t retrans_psns[MAX_RETX_BURST];

    for (uint32_t i = arq->send_base; i < arq->send_next && retrans_count < MAX_RETX_BURST; i++) {
        uint32_t idx = i % WINDOW_SIZE;
        uint32_t entry_psn = arq->send_window[idx].psn;
        uint32_t diff = psn_forward_dist(entry_psn, nack_psn);

        if (diff < 0x800000) {
            retrans_idx[retrans_count]  = idx;
            retrans_psns[retrans_count] = entry_psn;
            retrans_count++;
            arq->send_window[idx].send_time = rte_rdtsc();
            arq->send_window[idx].retry_count++;
            arq->total_retransmit++;
        }
    }

    rte_spinlock_unlock(&arq->lock);

    if (retrans_count > 0) {
        LOG_INFOF("ARQ NACK for QPN 0x%x: retransmitting %u packets starting PSN %u",
                  arq->qpn, retrans_count, nack_psn);

        for (uint32_t i = 0; i < retrans_count; i++) {
            uint32_t idx = retrans_idx[i];
            struct rte_mbuf *m = arq->send_window[idx].mbuf;
            if (rte_eth_tx_burst(wan_port, 0, &m, 1) == 0) {
                LOG_WARNF("Failed to retransmit packet QPN 0x%x PSN %u", arq->qpn, retrans_psns[i]);
            }
        }
    }

    return retrans_count;
}

void arq_check_timeouts(arq_context_t *arq, uint16_t wan_port) {
    rte_spinlock_lock(&arq->lock);
    uint64_t current_time = rte_rdtsc();
    uint64_t timeout_cycles = rte_get_timer_hz() * RETRY_TIMEOUT_MS / 1000;

    for (uint32_t i = arq->send_base; i < arq->send_next; i++) {
        uint32_t idx = i % WINDOW_SIZE;
        if (!arq->send_window[idx].in_flight) {
            continue;
        }

        if (current_time - arq->send_window[idx].send_time > timeout_cycles) {
            if (arq->send_window[idx].retry_count < MAX_RETRY_COUNT) {
                arq->send_window[idx].send_time = current_time;
                arq->send_window[idx].retry_count++;
                arq->total_retransmit++;

                struct rte_mbuf *m = arq->send_window[idx].mbuf;
                if (rte_eth_tx_burst(wan_port, 0, &m, 1) == 0) {
                    LOG_WARNF("Failed to retransmit packet QPN 0x%x PSN %u",
                              arq->qpn, arq->send_window[idx].psn);
                }
            } else {
                LOG_ERRORF("ARQ max retries exceeded for QPN 0x%x PSN %u, dropping",
                           arq->qpn, arq->send_window[idx].psn);
                rte_pktmbuf_free(arq->send_window[idx].mbuf);
                arq->send_window[idx].mbuf = NULL;
                arq->send_window[idx].in_flight = 0;
                arq->total_timeout++;
            }
        }
    }

    rte_spinlock_unlock(&arq->lock);
}

void arq_set_peer_ip(arq_context_t *arq, uint32_t peer_ip) {
    rte_spinlock_lock(&arq->lock);
    arq->peer_ip = peer_ip;
    rte_spinlock_unlock(&arq->lock);
}
