/*
 * QP context table, keyed by QPN
 */
#ifndef _QP_CTX_H_
#define _QP_CTX_H_

#include <stdint.h>
#include <rte_mbuf.h>
#include <rte_timer.h>
#include <rte_spinlock.h>
#include <rte_ether.h>
#include "roce_defs.h"

#define MAX_QP_CTX 1024
#define PSN_MASK 0x00FFFFFF
#define MAX_PSN 0x01000000
#define WINDOW_SIZE 256
#define RETRY_TIMEOUT_MS 10
#define MAX_RETRY_COUNT 3

typedef enum {
    WRITE_SEG_FIRST = 1,
    WRITE_SEG_MIDDLE = 2,
    WRITE_SEG_LAST = 3,
    WRITE_SEG_ONLY = 4
} write_segment_type_t;

typedef struct {
    uint32_t qpn;
    rte_spinlock_t lock;
    uint8_t active;
    uint64_t last_cnp_tsc;   // per-flow CNP timer
    uint32_t notify_ip;      // CNP dst IP
    uint16_t pkey;           // echoed in CNP
    struct rte_ether_addr notify_mac;  // CNP dst
    struct rte_ether_addr gw_mac;      // CNP src
} qp_context_t;

typedef struct {
    qp_context_t qps[MAX_QP_CTX];
    rte_spinlock_t global_lock;
    uint32_t active_qp_count;
} qp_manager_t;

void qp_mgr_init(qp_manager_t *mgr);
qp_context_t *qp_get_or_create(qp_manager_t *mgr, uint32_t qpn);

#endif
