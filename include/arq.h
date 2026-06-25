/*
 * ARQ retransmit layer over WAN
 *
 * control messages on UDP 4792, separate from RoCEv2 data (4791)
 */
#ifndef _ARQ_H_
#define _ARQ_H_

#include <rte_mbuf.h>
#include <rte_timer.h>
#include "qp_ctx.h"

#define ARQ_TIMER_INTERVAL_MS 5
#define MAX_RETX_BURST 64

typedef struct arq_control_msg {
    uint32_t magic;
    uint8_t type;
    uint32_t qpn;
    uint32_t psn;
    uint32_t count;
    uint32_t psn_list[MAX_RETX_BURST];
} arq_control_msg_t;

#define ARQ_MSG_ACK 0x01
#define ARQ_MSG_NACK 0x02
#define ARQ_MSG_RETX 0x03

typedef struct arq_send_entry {
    uint32_t psn;
    struct rte_mbuf *mbuf;
    uint64_t send_time;
    uint8_t retry_count;
    uint8_t segment_type;
    uint8_t in_flight;
} arq_send_entry_t;

typedef struct {
    uint32_t qpn;
    arq_send_entry_t send_window[WINDOW_SIZE];
    uint32_t send_base;
    uint32_t send_next;
    uint32_t send_max;
    rte_spinlock_t lock;

    uint64_t total_sent;
    uint64_t total_ack_received;
    uint64_t total_retransmit;
    uint64_t total_timeout;

    uint32_t peer_ip;
} arq_context_t;

typedef struct {
    arq_context_t contexts[MAX_QP_CTX];
    rte_spinlock_t global_lock;
} arq_manager_t;

void arq_mgr_init(arq_manager_t *mgr);
arq_context_t* arq_get_or_create(arq_manager_t *mgr, uint32_t qpn);
arq_context_t* arq_lookup(arq_manager_t *mgr, uint32_t qpn);

int arq_send_pkt(arq_context_t *arq, struct rte_mbuf *m, uint32_t psn, uint8_t segment_type);
int arq_handle_ack(arq_context_t *arq, uint32_t ack_psn);
int arq_handle_nack(arq_context_t *arq, uint32_t nack_psn, uint16_t wan_port);
void arq_check_timeouts(arq_context_t *arq, uint16_t wan_port);
void arq_set_peer_ip(arq_context_t *arq, uint32_t peer_ip);

struct rte_mbuf* arq_build_ctrl_msg(uint32_t qpn, uint8_t type, uint32_t psn,
                                 uint32_t dst_ip, uint32_t *psn_list, uint32_t count,
                                 struct rte_mempool *pool);
int arq_parse_ctrl_msg(struct rte_mbuf *m, arq_control_msg_t *msg);

#endif