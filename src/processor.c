/*
 * processor.c - 单 lcore 报文处理主逻辑
 *
 * LAN 侧：识别 RoCE WRITE，伪造 ACK 回注 LAN，并把数据转发到 WAN。
 * WAN 侧：重组 WRITE 段，缺失段发 NACK，收齐后整体下发 LAN。
 */
#include "processor.h"
#include "wan_tunnel.h"
#include "arq.h"
#include "crc32c.h"
#include "log.h"
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_memcpy.h>
#include <rte_lcore.h>
#include <rte_hash.h>

uint16_t wan_tunnel_udp_port = WAN_UDP_PORT;

static uint32_t g_lan_core_count = 1;
static uint32_t g_wan_core_count = 1;

static uint8_t get_segment_type(uint8_t opcode) {
    /* WRITE_LAST_IMM 在重组时与 LAST 等价，IMM 数据由网卡处理 */
    switch (opcode) {
        case ROCE_OPCODE_WRITE_FIRST:  return WRITE_SEG_FIRST;
        case ROCE_OPCODE_WRITE_MIDDLE: return WRITE_SEG_MIDDLE;
        case ROCE_OPCODE_WRITE_LAST:   return WRITE_SEG_LAST;
        case ROCE_OPCODE_WRITE_ONLY:   return WRITE_SEG_ONLY;
        case ROCE_OPCODE_WRITE_LAST_IMM: return WRITE_SEG_LAST;
        default: return 0;
    }
}

static int forge_and_send_ack(struct rte_mbuf *orig_m, struct rte_mempool *pool, uint16_t lan_port, struct roce_bth *req_bth) {
    struct rte_mbuf *ack_m = rte_pktmbuf_alloc(pool);
    if (!ack_m)
        return -1;

    struct rte_ether_hdr *orig_eth = rte_pktmbuf_mtod(orig_m, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *orig_ip = (struct rte_ipv4_hdr *)(orig_eth + 1);
    struct rte_udp_hdr *orig_udp = (struct rte_udp_hdr *)(orig_ip + 1);

    size_t ack_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                     sizeof(struct rte_udp_hdr) + sizeof(struct roce_bth) +
                     sizeof(struct roce_aeth) + 4;

    char *ack_data = rte_pktmbuf_append(ack_m, ack_size);
    if (!ack_data) {
        rte_pktmbuf_free(ack_m);
        return -1;
    }

    struct rte_ether_hdr *ack_eth = (struct rte_ether_hdr *)ack_data;
    struct rte_ipv4_hdr *ack_ip = (struct rte_ipv4_hdr *)(ack_eth + 1);
    struct rte_udp_hdr *ack_udp = (struct rte_udp_hdr *)(ack_ip + 1);
    struct roce_bth *ack_bth = (struct roce_bth *)(ack_udp + 1);
    struct roce_aeth *ack_aeth = (struct roce_aeth *)(ack_bth + 1);
    uint32_t *icrc = (uint32_t *)(ack_aeth + 1);

    rte_ether_addr_copy(&orig_eth->s_addr, &ack_eth->d_addr);
    rte_ether_addr_copy(&orig_eth->d_addr, &ack_eth->s_addr);
    ack_eth->ether_type = orig_eth->ether_type;

    ack_ip->version_ihl = orig_ip->version_ihl;
    ack_ip->type_of_service = orig_ip->type_of_service;
    ack_ip->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct roce_bth) + sizeof(struct roce_aeth) + 4);
    ack_ip->packet_id = 0;
    ack_ip->fragment_offset = 0;
    ack_ip->time_to_live = 64;
    ack_ip->next_proto_id = IPPROTO_UDP;
    ack_ip->src_addr = orig_ip->dst_addr;
    ack_ip->dst_addr = orig_ip->src_addr;
    ack_ip->hdr_checksum = 0;

    ack_udp->src_port = orig_udp->dst_port;
    ack_udp->dst_port = orig_udp->src_port;
    ack_udp->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + sizeof(struct roce_bth) + sizeof(struct roce_aeth) + 4);
    ack_udp->dgram_cksum = 0;

    ack_bth->opcode = ROCE_OPCODE_ACK;
    ack_bth->tver_pad = req_bth->tver_pad;
    ack_bth->pkey = req_bth->pkey;
    ack_bth->f_b_se_m = 0;
    ack_bth->pad_res = 0;
    ack_bth->dqpn = req_bth->dqpn;

    /* PSN 字段低 8 位在 RoCE 中保留，此处清零；高 24 位为实际 PSN */
    uint32_t psn = rte_be_to_cpu_32(req_bth->psn) & 0x00FFFFFF;
    ack_bth->psn = rte_cpu_to_be_32((psn << 8) | 0x00);

    /* syndrome=0x00 表示正常 ACK；credit=0xFFFF 表示不限制对端信用 */
    ack_aeth->syndrome = 0x00;
    ack_aeth->msn = 0;
    ack_aeth->credit = rte_cpu_to_be_16(0xFFFF);

    /* icrc 覆盖 BTH+AETH，不含以太网/IP/UDP 头 */
    *icrc = rte_cpu_to_be_32(crc32c_calculate((uint8_t*)ack_bth, sizeof(struct roce_bth) + sizeof(struct roce_aeth)));

    if (rte_eth_tx_burst(lan_port, 0, &ack_m, 1) == 0) {
        LOG_WARN("Failed to send spoofed ACK to LAN");
        rte_pktmbuf_free(ack_m);
        return -1;
    }

    LOG_DEBUGF("Spoofed ACK sent for QPN: 0x%x, PSN: %u",
               rte_be_to_cpu_32(ack_bth->dqpn) >> 8, psn);
    return 0;
}

void proc_init(processor_context_t *proc, uint32_t lcore_id, uint8_t is_lan_core) {
    memset(proc, 0, sizeof(processor_context_t));
    qpMgrInit(&proc->qp_mgr);
    arqMgrInit(&proc->arq_mgr);
    jbInit(&proc->jitter_buffer);
    cc_init(&proc->congestion_ctrl, BUFFER_HIGH_WATER, BUFFER_LOW_WATER);
    stat_init(&proc->stats);
    proc->lcore_id = lcore_id;
    proc->is_lan_core = is_lan_core;
    proc->packet_counter = 0;
    LOG_INFOF("proc[%u]: ready (%s)", lcore_id, is_lan_core ? "LAN" : "WAN");
}

void proc_set_core_counts(uint32_t lan_core_count, uint32_t wan_core_count) {
    g_lan_core_count = lan_core_count;
    g_wan_core_count = wan_core_count;
    LOG_INFOF("cores: lan=%u wan=%u", lan_core_count, wan_core_count);
}

/* 流哈希：同一流固定到同一 lcore，避免跨核同步 */
static inline uint32_t flow_hash(struct rte_ipv4_hdr *ip, struct rte_udp_hdr *udp, uint32_t qpn) {
    uint32_t hash = rte_hash_crc_32b(&ip->src_addr, sizeof(ip->src_addr), 0);
    hash = rte_hash_crc_32b(&ip->dst_addr, sizeof(ip->dst_addr), hash);
    hash = rte_hash_crc_32b(&udp->src_port, sizeof(udp->src_port), hash);
    hash = rte_hash_crc_32b(&qpn, sizeof(qpn), hash);
    return hash;
}

uint32_t proc_flow_to_lan_lcore(struct rte_ipv4_hdr *ip, struct rte_udp_hdr *udp, uint32_t qpn) {
    return flow_hash(ip, udp, qpn) % g_lan_core_count;
}

uint32_t proc_flow_to_wan_lcore(struct rte_ipv4_hdr *ip, struct rte_udp_hdr *udp, uint32_t qpn) {
    return g_lan_core_count + (flow_hash(ip, udp, qpn) % g_wan_core_count);
}

static uint32_t calculate_arq_window_usage(processor_context_t *proc) {
    uint32_t used_buffers = 0;
    for (uint32_t i = 0; i < MAX_QP_CTX; i++) {
        if (proc->arq_mgr.contexts[i].qpn != 0) {
            used_buffers += proc->arq_mgr.contexts[i].send_next - proc->arq_mgr.contexts[i].send_base;
        }
    }
    return used_buffers;
}

static void check_congestion(processor_context_t *proc, uint16_t lan_port, struct rte_mempool *pool) {
    uint32_t used_buffers = calculate_arq_window_usage(proc);
    uint32_t total_buffers = MAX_QP_CTX * WINDOW_SIZE;
    
    cc_checkBuffer(&proc->congestion_ctrl, used_buffers, total_buffers);
    if (proc->congestion_ctrl.congestion_detected) {
        for (uint32_t i = 0; i < MAX_QP_CTX; i++) {
            if (proc->arq_mgr.contexts[i].qpn != 0) {
                if (cc_shouldSendCNP(&proc->congestion_ctrl, proc->arq_mgr.contexts[i].qpn)) {
                    struct rte_mbuf *cnp = cc_buildCNP(&proc->congestion_ctrl, proc->arq_mgr.contexts[i].qpn, pool);
                    if (cnp) {
                        if (rte_eth_tx_burst(lan_port, 0, &cnp, 1) == 0) {
                            LOG_WARN("Failed to send CNP");
                            rte_pktmbuf_free(cnp);
                        } else {
                            LOG_DEBUGF("CNP sent for QPN 0x%x", proc->arq_mgr.contexts[i].qpn);
                            stat_inc(&proc->stats, STAT_CNP_SENT);
                        }
                    }
                }
            }
        }
    }
}

void proc_process_lan_rx(processor_context_t *proc, struct rte_mbuf *m, uint16_t lan_port, uint16_t wan_port, struct rte_mempool *pool) {
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

    if (rte_be_to_cpu_16(eth_hdr->ether_type) != RTE_ETHER_TYPE_IPV4) {
        stat_inc(&proc->stats, STAT_PKTS_DROPPED);
        rte_pktmbuf_free(m);
        return;
    }

    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    uint32_t dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
    uint32_t peer_ip = wan_getPeerIP(dst_ip);

    if (peer_ip == 0) {
        stat_inc(&proc->stats, STAT_PKTS_PASSTHROUGH);
        wan_fwdPassthrough(m, wan_port);
        return;
    }

    if (ip_hdr->next_proto_id != IPPROTO_UDP) {
        stat_inc(&proc->stats, STAT_PKTS_PASSTHROUGH);
        wan_fwdPassthrough(m, wan_port);
        return;
    }

    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
    uint16_t dst_port = rte_be_to_cpu_16(udp_hdr->dst_port);

    if (dst_port != 4791) {
        stat_inc(&proc->stats, STAT_PKTS_PASSTHROUGH);
        wan_fwdPassthrough(m, wan_port);
        return;
    }

    struct roce_bth *bth = (struct roce_bth *)(udp_hdr + 1);
    uint8_t opcode = bth->opcode;

    if (!is_write_opcode(opcode)) {
        stat_inc(&proc->stats, STAT_PKTS_PASSTHROUGH);
        wan_fwdPassthrough(m, wan_port);
        return;
    }

    uint32_t qpn = rte_be_to_cpu_32(bth->dqpn) >> 8;
    uint32_t psn = rte_be_to_cpu_32(bth->psn) & 0x00FFFFFF;
    uint8_t segment_type = get_segment_type(opcode);

    qp_context_t *qp_ctx = qpGetOrCreate(&proc->qp_mgr, qpn);
    if (!qp_ctx) {
        rte_pktmbuf_free(m);
        stat_inc(&proc->stats, STAT_PKTS_DROPPED);
        return;
    }

    stat_inc(&proc->stats, STAT_WRITE_PKTS_PROCESSED);
    stat_inc(&proc->stats, STAT_PKTS_LAN_TO_WAN);

    /* 先伪造 ACK 回注 LAN，释放对端发送缓冲；WAN 可靠性由 ARQ 保证 */
    forge_and_send_ack(m, pool, lan_port, bth);
    stat_inc(&proc->stats, STAT_ACK_SPOOFED_SENT);

    struct rte_mbuf *wan_m = wan_fwdRoCE(m, wan_port);

    /* ARQ 上下文与 QP 一一对应，此处复用 qpn 做查找 */
    arq_context_t *arq = arqGetOrCreate(&proc->arq_mgr, qpn);
    if (arq) {
        arqSendPkt(arq, wan_m, psn, segment_type);
        arqSetPeerIP(arq, peer_ip);
    }

    if (rte_eth_tx_burst(wan_port, 0, &wan_m, 1) == 0) {
        LOG_WARN("Failed to send packet to WAN");
        rte_pktmbuf_free(wan_m);
        stat_inc(&proc->stats, STAT_PKTS_DROPPED);
    } else {
        LOG_DEBUGF("Packet sent to WAN for QPN 0x%x, PSN %u", qpn, psn);
    }

    proc->packet_counter++;
    if (proc->packet_counter % CONGESTION_CHECK_INTERVAL == 0) {
        check_congestion(proc, lan_port, pool);
    }
}

void proc_process_wan_rx(processor_context_t *proc, struct rte_mbuf *m, uint16_t lan_port, uint16_t wan_port, struct rte_mempool *pool) {
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

    if (rte_be_to_cpu_16(eth_hdr->ether_type) != RTE_ETHER_TYPE_IPV4) {
        rte_pktmbuf_free(m);
        stat_inc(&proc->stats, STAT_PKTS_DROPPED);
        return;
    }

    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    if (ip_hdr->next_proto_id != IPPROTO_UDP) {
        rte_pktmbuf_free(m);
        stat_inc(&proc->stats, STAT_PKTS_DROPPED);
        return;
    }

    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
    uint16_t dst_port = rte_be_to_cpu_16(udp_hdr->dst_port);

    if (dst_port == ARQ_CTRL_PORT) {
        arq_control_msg_t msg;
        if (arqParseCtrlMsg(m, &msg) == 0) {
            arq_context_t *arq = arqLookup(&proc->arq_mgr, msg.qpn);
            if (arq) {
                if (msg.type == ARQ_MSG_ACK) {
                    arqHandleAck(arq, msg.psn);
                    stat_inc(&proc->stats, STAT_ARQ_ACK_RECEIVED);
                } else if (msg.type == ARQ_MSG_NACK) {
                    arqHandleNack(arq, msg.psn, wan_port, pool);
                    stat_inc(&proc->stats, STAT_ARQ_NACK_RECEIVED);
                }
            }
        }
        rte_pktmbuf_free(m);
        return;
    }

    if (dst_port != 4791) {
        rte_pktmbuf_free(m);
        stat_inc(&proc->stats, STAT_PKTS_DROPPED);
        return;
    }

    struct roce_bth *bth = (struct roce_bth *)(udp_hdr + 1);
    uint8_t opcode = bth->opcode;

    if (!is_write_opcode(opcode)) {
        LOG_DEBUGF("Non-WRITE packet dropped (OpCode: 0x%x)", opcode);
        rte_pktmbuf_free(m);
        stat_inc(&proc->stats, STAT_PKTS_DROPPED);
        return;
    }

    uint32_t qpn = rte_be_to_cpu_32(bth->dqpn) >> 8;
    uint32_t psn = rte_be_to_cpu_32(bth->psn) & 0x00FFFFFF;
    uint8_t segment_type = get_segment_type(opcode);

    stat_inc(&proc->stats, STAT_PKTS_WAN_TO_LAN);

    int added = jbAddSeg(&proc->jitter_buffer, qpn, psn, segment_type, m);
    if (added < 0) {
        rte_pktmbuf_free(m);
        stat_inc(&proc->stats, STAT_PKTS_DROPPED);
        return;
    }

    if (added == 1) {
        uint32_t missing_psn = jbCheckMissing(&proc->jitter_buffer, qpn);
        if (missing_psn != 0) {
            uint32_t dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
            uint32_t peer_ip = wan_getPeerIP(dst_ip);

            struct rte_mbuf *nack_msg = arqBuildCtrlMsg(qpn, ARQ_MSG_NACK, missing_psn, peer_ip, NULL, 0, pool);
            if (nack_msg) {
                if (rte_eth_tx_burst(wan_port, 0, &nack_msg, 1) == 0) {
                    rte_pktmbuf_free(nack_msg);
                } else {
                    LOG_DEBUGF("NACK sent for QPN 0x%x, missing PSN %u", qpn, missing_psn);
                    stat_inc(&proc->stats, STAT_NACK_SENT);
                }
            }
        }
    }

    if (jbIsWriteComplete(&proc->jitter_buffer, qpn)) {
        /* 栈上缓冲足够 JB_MAX_SEGMENTS 个指针，避免 static 共享 */
        struct rte_mbuf *segments[JB_MAX_SEGMENTS];
        uint32_t seg_count = 0;

        if (jbGetOrderedSegs(&proc->jitter_buffer, qpn, segments, &seg_count) == 0 && seg_count > 0) {
            for (uint32_t i = 0; i < seg_count; i++) {
                struct rte_mbuf *lan_m = segments[i];
                wan_prepareForLAN(lan_m);

                if (rte_eth_tx_burst(lan_port, 0, &lan_m, 1) == 0) {
                    rte_pktmbuf_free(lan_m);
                }
            }

            jbRemoveWrite(&proc->jitter_buffer, qpn);
            uint32_t dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
            uint32_t peer_ip = wan_getPeerIP(dst_ip);
            struct rte_mbuf *ack_msg = arqBuildCtrlMsg(qpn, ARQ_MSG_ACK, psn, peer_ip, NULL, 0, pool);
            if (ack_msg) {
                if (rte_eth_tx_burst(wan_port, 0, &ack_msg, 1) == 0) {
                    rte_pktmbuf_free(ack_msg);
                } else {
                    LOG_DEBUGF("ARQ ACK sent for QPN 0x%x, PSN %u", qpn, psn);
                    stat_inc(&proc->stats, STAT_ARQ_ACK_SENT);
                }
            }
        }
    }
}

void proc_timer_callback(processor_context_t *proc, uint16_t wan_port, struct rte_mempool *pool) {
    for (uint32_t i = 0; i < MAX_QP_CTX; i++) {
        if (proc->arq_mgr.contexts[i].qpn != 0) {
            arqCheckTimeouts(&proc->arq_mgr.contexts[i], wan_port, pool);
        }
    }

    static uint64_t last_stats_dump = 0;
    uint64_t current_time = rte_rdtsc();
    if (current_time - last_stats_dump > rte_get_timer_hz() * 10) {
        last_stats_dump = current_time;
        stat_dump(&proc->stats);
    }
}