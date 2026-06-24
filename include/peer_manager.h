/*
 * peer_manager.h - 目的子网到对端网关 IP 的映射表
 *
 * 查找采用线性扫描，MAX_PEERS=32 足够覆盖典型双活/多活拓扑。
 */
#ifndef _PEER_MANAGER_H_
#define _PEER_MANAGER_H_

#include <stdint.h>
#include <rte_ether.h>

#define MAX_PEERS 32

typedef struct {
    uint32_t subnet;        // 子网地址（网络字节序）
    uint32_t subnet_mask;   // 子网掩码（网络字节序）
    uint32_t peer_ip;       // 对端网关 IP（网络字节序）
    struct rte_ether_addr peer_mac;  // 对端网关 MAC（如果已知）
} peer_mapping_t;

typedef struct {
    peer_mapping_t peers[MAX_PEERS];
    uint32_t peer_count;
    uint32_t default_peer_ip;  // 默认对端网关 IP
    struct rte_ether_addr default_peer_mac;
} peer_manager_t;

void pm_init(peer_manager_t *pm);
int pm_add(peer_manager_t *pm, uint32_t subnet, uint32_t subnet_mask, uint32_t peer_ip);
int pm_set_default(peer_manager_t *pm, uint32_t peer_ip);
uint32_t pm_lookup(peer_manager_t *pm, uint32_t dst_ip);
void pm_dump(peer_manager_t *pm);

#endif