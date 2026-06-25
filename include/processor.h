/*
 * per-lcore processor context
 *
 * each lcore holds its own processor_context_t, flow-to-lcore mapping
 * pins same QPN to same core so state machines don't need cross-core locks.
 */
#ifndef _PROCESSOR_H_
#define _PROCESSOR_H_

#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include "qp_ctx.h"
#include "arq.h"
#include "jitter_buffer.h"
#include "congestion.h"
#include "stats.h"

#define MAX_LCORES RTE_MAX_LCORE
#define CONGESTION_CHECK_INTERVAL 100  // 每发送100个包检查一次拥塞

typedef struct {
    qp_manager_t qp_mgr;
    arq_manager_t arq_mgr;
    jitter_buffer_t jitter_buffer;
    congestion_control_t congestion_ctrl;
    gateway_stats_t stats;
    uint32_t lcore_id;
    uint8_t is_lan_core;  // 1 = LAN侧核, 0 = WAN侧核
    uint32_t packet_counter;  // 用于拥塞检查的包计数器
} processor_context_t;

typedef struct {
    processor_context_t *proc;
    uint16_t lan_port;
    uint16_t wan_port;
    struct rte_mempool *mbuf_pool;
    uint8_t is_lan_core;
} lcore_args_t;

void proc_init(processor_context_t *proc, uint32_t lcore_id, uint8_t is_lan_core);
void proc_process_lan_rx(processor_context_t *proc, struct rte_mbuf *m, uint16_t lan_port, uint16_t wan_port, struct rte_mempool *pool);
void proc_process_wan_rx(processor_context_t *proc, struct rte_mbuf *m, uint16_t lan_port, uint16_t wan_port, struct rte_mempool *pool);
void proc_timer_callback(processor_context_t *proc, uint16_t wan_port);

uint32_t proc_flow_to_lan_lcore(struct rte_ipv4_hdr *ip, struct rte_udp_hdr *udp, uint32_t qpn);
uint32_t proc_flow_to_wan_lcore(struct rte_ipv4_hdr *ip, struct rte_udp_hdr *udp, uint32_t qpn);

void proc_set_core_counts(uint32_t lan_core_count, uint32_t wan_core_count);

#endif