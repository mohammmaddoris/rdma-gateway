/*
 * qp_ctx.h - QP 上下文管理：TX/RX 窗口、PSN 跟踪与 WRITE 重组状态机
 *
 * 容量 MAX_QP_CTX=1024 为线性表查找，适用于中小规模部署；
 * 若单核 QP 数 > 数百，建议替换为 rte_hash。  -- FIXME
 */
#ifndef _QP_CTX_H_
#define _QP_CTX_H_

#include <stdint.h>
#include <rte_mbuf.h>
#include <rte_timer.h>
#include <rte_spinlock.h>
#include "roce_defs.h"

#define MAX_QP_CTX 1024
#define PSN_MASK 0x00FFFFFF
#define MAX_PSN 0x01000000
#define WINDOW_SIZE 256
#define RETRY_TIMEOUT_MS 10
#define MAX_RETRY_COUNT 3

typedef enum {
    QP_STATE_IDLE,
    QP_STATE_WRITE_FIRST_RECV,
    QP_STATE_WRITE_MIDDLE_RECV,
    QP_STATE_WRITE_COMPLETE,
    QP_STATE_ERROR
} qp_write_state_t;

typedef enum {
    WRITE_SEG_FIRST = 1,
    WRITE_SEG_MIDDLE = 2,
    WRITE_SEG_LAST = 3,
    WRITE_SEG_ONLY = 4
} write_segment_type_t;

typedef struct {
    uint32_t psn;
    struct rte_mbuf *mbuf;
    uint8_t segment_type;
    uint8_t retry_count;
    uint64_t send_time;
    uint8_t acked;
} tx_packet_entry_t;

typedef struct {
    uint32_t psn;
    struct rte_mbuf *mbuf;
    uint8_t segment_type;
    uint8_t received;
    uint64_t recv_time;
} rx_packet_entry_t;

typedef struct {
    uint32_t qpn;
    uint32_t expected_psn_tx;
    uint32_t expected_psn_rx;
    uint32_t last_ack_psn;
    qp_write_state_t write_state;
    rte_spinlock_t lock;

    tx_packet_entry_t tx_window[WINDOW_SIZE];
    uint32_t tx_window_base;
    uint32_t tx_window_size;

    rx_packet_entry_t rx_window[WINDOW_SIZE];
    uint32_t rx_window_base;

    uint64_t total_packets_sent;
    uint64_t total_packets_recv;
    uint64_t total_retransmit;
    uint64_t total_nack_sent;

    uint8_t active;
} qp_context_t;

typedef struct {
    qp_context_t qps[MAX_QP_CTX];
    rte_spinlock_t global_lock;
    uint32_t active_qp_count;
} qp_manager_t;

void qpMgrInit(qp_manager_t *mgr);
qp_context_t* qpGetOrCreate(qp_manager_t *mgr, uint32_t qpn);
qp_context_t* qpLookup(qp_manager_t *mgr, uint32_t qpn);
void qpRemove(qp_manager_t *mgr, uint32_t qpn);
int qpUpdateTxState(qp_context_t *qp, uint32_t psn, uint8_t segment_type);
int qpUpdateRxState(qp_context_t *qp, uint32_t psn, uint8_t segment_type);
int qpHandleAck(qp_context_t *qp, uint32_t ack_psn);
uint32_t qpGetNextExpPSN(qp_context_t *qp);
int qpIsWriteComplete(qp_context_t *qp);
void qpReset(qp_context_t *qp);
void qpDumpStats(qp_context_t *qp);

#endif