/*
 * QP context table
 */
#include "qp_ctx.h"
#include "log.h"
#include "common.h"
#include <rte_malloc.h>
#include <string.h>

void qp_mgr_init(qp_manager_t *mgr) {
    memset(mgr, 0, sizeof(qp_manager_t));
    rte_spinlock_init(&mgr->global_lock);
    mgr->active_qp_count = 0;
}

qp_context_t* qp_get_or_create(qp_manager_t *mgr, uint32_t qpn) {
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
