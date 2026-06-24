/*
 * qp_ctx.c - QP 上下文管理实现
 */
#include "qp_ctx.h"
#include "log.h"
#include "common.h"
#include <rte_malloc.h>
#include <string.h>

void qpMgrInit(qp_manager_t *mgr) {
    memset(mgr, 0, sizeof(qp_manager_t));
    rte_spinlock_init(&mgr->global_lock);
    mgr->active_qp_count = 0;
}

qp_context_t* qpGetOrCreate(qp_manager_t *mgr, uint32_t qpn) {
    rte_spinlock_lock(&mgr->global_lock);

    for (uint32_t i = 0; i < MAX_QP_CTX; i++) {
        if (mgr->qps[i].active && mgr->qps[i].qpn == qpn) {
            rte_spinlock_unlock(&mgr->global_lock);
            return &mgr->qps[i];
        }
    }

    for (uint32_t i = 0; i < MAX_QP_CTX; i++) {
        if (!mgr->qps[i].active) {
            memset(&mgr->qps[i], 0, sizeof(qp_context_t));
            mgr->qps[i].qpn = qpn;
            mgr->qps[i].active = 1;
            mgr->qps[i].write_state = QP_STATE_IDLE;
            rte_spinlock_init(&mgr->qps[i].lock);
            mgr->active_qp_count++;
            rte_spinlock_unlock(&mgr->global_lock);
            LOG_INFOF("Created new QP context for QPN: 0x%x", qpn);
            return &mgr->qps[i];
        }
    }

    rte_spinlock_unlock(&mgr->global_lock);
    LOG_ERRORF("Failed to create QP context: reached max capacity %d", MAX_QP_CTX);
    return NULL;
}

qp_context_t* qpLookup(qp_manager_t *mgr, uint32_t qpn) {
    rte_spinlock_lock(&mgr->global_lock);

    for (uint32_t i = 0; i < MAX_QP_CTX; i++) {
        if (mgr->qps[i].active && mgr->qps[i].qpn == qpn) {
            rte_spinlock_unlock(&mgr->global_lock);
            return &mgr->qps[i];
        }
    }

    rte_spinlock_unlock(&mgr->global_lock);
    return NULL;
}

void qpRemove(qp_manager_t *mgr, uint32_t qpn) {
    rte_spinlock_lock(&mgr->global_lock);

    for (uint32_t i = 0; i < MAX_QP_CTX; i++) {
        if (mgr->qps[i].active && mgr->qps[i].qpn == qpn) {
            for (uint32_t j = 0; j < WINDOW_SIZE; j++) {
                if (mgr->qps[i].tx_window[j].mbuf) {
                    rte_pktmbuf_free(mgr->qps[i].tx_window[j].mbuf);
                    mgr->qps[i].tx_window[j].mbuf = NULL;
                }
                if (mgr->qps[i].rx_window[j].mbuf) {
                    rte_pktmbuf_free(mgr->qps[i].rx_window[j].mbuf);
                    mgr->qps[i].rx_window[j].mbuf = NULL;
                }
            }
            mgr->qps[i].active = 0;
            mgr->active_qp_count--;
            break;
        }
    }

    rte_spinlock_unlock(&mgr->global_lock);
}

int qpUpdateTxState(qp_context_t *qp, uint32_t psn, uint8_t segment_type) {
    rte_spinlock_lock(&qp->lock);

    uint32_t idx = (qp->tx_window_base + qp->tx_window_size) % WINDOW_SIZE;
    if (qp->tx_window_size >= WINDOW_SIZE) {
        rte_spinlock_unlock(&qp->lock);
        return -1;
    }

    qp->tx_window[idx].psn = psn;
    qp->tx_window[idx].segment_type = segment_type;
    qp->tx_window[idx].retry_count = 0;
    qp->tx_window[idx].send_time = rte_rdtsc();
    qp->tx_window[idx].acked = 0;
    qp->tx_window_size++;
    qp->expected_psn_tx = (psn + 1) & PSN_MASK;

    rte_spinlock_unlock(&qp->lock);
    return 0;
}

int qpUpdateRxState(qp_context_t *qp, uint32_t psn, uint8_t segment_type) {
    rte_spinlock_lock(&qp->lock);

    uint32_t idx = psn_forward_dist(qp->rx_window_base, psn) % WINDOW_SIZE;

    if (qp->rx_window[idx].received) {
        /* 重复段：RoCE 允许对端重传，直接忽略即可 */
        rte_spinlock_unlock(&qp->lock);
        return 0;
    }

    qp->rx_window[idx].psn = psn;
    qp->rx_window[idx].segment_type = segment_type;
    qp->rx_window[idx].received = 1;
    qp->rx_window[idx].recv_time = rte_rdtsc();
    qp->expected_psn_rx = (psn + 1) & PSN_MASK;

    switch (segment_type) {
        case WRITE_SEG_FIRST:
            qp->write_state = QP_STATE_WRITE_FIRST_RECV;
            break;
        case WRITE_SEG_MIDDLE:
            if (qp->write_state == QP_STATE_WRITE_FIRST_RECV) {
                qp->write_state = QP_STATE_WRITE_MIDDLE_RECV;
            }
            break;
        case WRITE_SEG_LAST:
        case WRITE_SEG_ONLY:
            if (qp->write_state == QP_STATE_WRITE_FIRST_RECV || qp->write_state == QP_STATE_WRITE_MIDDLE_RECV) {
                qp->write_state = QP_STATE_WRITE_COMPLETE;
            }
            break;
        default:
            break;
    }

    rte_spinlock_unlock(&qp->lock);
    return 0;
}

int qpHandleAck(qp_context_t *qp, uint32_t ack_psn) {
    rte_spinlock_lock(&qp->lock);

    uint32_t dist = psn_forward_dist(qp->tx_window_base, ack_psn);
    if (dist >= WINDOW_SIZE) {
        rte_spinlock_unlock(&qp->lock);
        return -1;
    }

    while (qp->tx_window_size > 0) {
        uint32_t idx = qp->tx_window_base;
        uint32_t entry_psn = qp->tx_window[idx].psn;
        uint32_t d = psn_forward_dist(entry_psn, ack_psn);

        if (d <= dist) {
            if (qp->tx_window[idx].mbuf) {
                rte_pktmbuf_free(qp->tx_window[idx].mbuf);
                qp->tx_window[idx].mbuf = NULL;
            }
            qp->tx_window_base = (qp->tx_window_base + 1) % WINDOW_SIZE;
            qp->tx_window_size--;
            qp->last_ack_psn = entry_psn;
        } else {
            break;
        }
    }

    rte_spinlock_unlock(&qp->lock);
    return 0;
}

uint32_t qpGetNextExpPSN(qp_context_t *qp) {
    rte_spinlock_lock(&qp->lock);
    uint32_t psn = qp->expected_psn_rx;
    rte_spinlock_unlock(&qp->lock);
    return psn;
}

int qpIsWriteComplete(qp_context_t *qp) {
    rte_spinlock_lock(&qp->lock);
    int complete = (qp->write_state == QP_STATE_WRITE_COMPLETE);
    rte_spinlock_unlock(&qp->lock);
    return complete;
}

void qpReset(qp_context_t *qp) {
    rte_spinlock_lock(&qp->lock);

    for (uint32_t i = 0; i < WINDOW_SIZE; i++) {
        if (qp->tx_window[i].mbuf) {
            rte_pktmbuf_free(qp->tx_window[i].mbuf);
            qp->tx_window[i].mbuf = NULL;
        }
        if (qp->rx_window[i].mbuf) {
            rte_pktmbuf_free(qp->rx_window[i].mbuf);
            qp->rx_window[i].mbuf = NULL;
        }
    }

    qp->tx_window_base = 0;
    qp->tx_window_size = 0;
    qp->rx_window_base = 0;
    qp->expected_psn_tx = 0;
    qp->expected_psn_rx = 0;
    qp->last_ack_psn = 0;
    qp->write_state = QP_STATE_IDLE;

    rte_spinlock_unlock(&qp->lock);
}

void qpDumpStats(qp_context_t *qp) {
    rte_spinlock_lock(&qp->lock);
    LOG_INFOF("QP 0x%x Stats: sent=%lu recv=%lu retrans=%lu nack=%lu",
             qp->qpn, qp->total_packets_sent, qp->total_packets_recv,
             qp->total_retransmit, qp->total_nack_sent);
    rte_spinlock_unlock(&qp->lock);
}
