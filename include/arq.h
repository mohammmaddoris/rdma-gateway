/*
 * arq.h - 可选的 ARQ 重传层，运行在 WAN 之上补偿丢包
 *
 * 控制报文走 UDP 4792，与 RoCEv2 数据流(4791)分离。
 * 每个 QPN 一个 arq_context_t，由 arq_manager_t 统一管理。
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

    struct rte_timer retransmit_timer;
    uint8_t timer_initialized;
    
    uint32_t peer_ip;
} arq_context_t;

typedef struct {
    arq_context_t contexts[MAX_QP_CTX];
    rte_spinlock_t global_lock;
    struct rte_timer_stat *stats;
} arq_manager_t;

void arqMgrInit(arq_manager_t *mgr);
arq_context_t* arqGetOrCreate(arq_manager_t *mgr, uint32_t qpn);
arq_context_t* arqLookup(arq_manager_t *mgr, uint32_t qpn);
void arqRemove(arq_manager_t *mgr, uint32_t qpn);

int arqSendPkt(arq_context_t *arq, struct rte_mbuf *m, uint32_t psn, uint8_t segment_type);
int arqHandleAck(arq_context_t *arq, uint32_t ack_psn);
int arqHandleNack(arq_context_t *arq, uint32_t nack_psn, uint16_t wan_port, struct rte_mempool *pool);
void arqCheckTimeouts(arq_context_t *arq, uint16_t wan_port, struct rte_mempool *pool);
int arqGetRetransList(arq_context_t *arq, uint32_t *psn_list, uint32_t *count);
void arqSetPeerIP(arq_context_t *arq, uint32_t peer_ip);

struct rte_mbuf* arqBuildCtrlMsg(uint32_t qpn, uint8_t type, uint32_t psn,
                                 uint32_t dst_ip, uint32_t *psn_list, uint32_t count,
                                 struct rte_mempool *pool);
int arqParseCtrlMsg(struct rte_mbuf *m, arq_control_msg_t *msg);

#endif