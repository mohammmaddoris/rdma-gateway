/*
 * destination subnet -> peer gateway IP mapping table
 */
#ifndef _PEER_MANAGER_H_
#define _PEER_MANAGER_H_

#include <stdint.h>
#include <rte_ether.h>

#define MAX_PEERS 32

typedef struct {
    uint32_t subnet;
    uint32_t subnet_mask;
    uint32_t peer_ip;
    struct rte_ether_addr peer_mac;
} peer_mapping_t;

typedef struct {
    peer_mapping_t peers[MAX_PEERS];
    uint32_t peer_count;
    uint32_t default_peer_ip;
    struct rte_ether_addr default_peer_mac;
} peer_manager_t;

void pm_init(peer_manager_t *pm);
int pm_add(peer_manager_t *pm, uint32_t subnet, uint32_t subnet_mask, uint32_t peer_ip);
int pm_set_default(peer_manager_t *pm, uint32_t peer_ip);
uint32_t pm_lookup(peer_manager_t *pm, uint32_t dst_ip);
void pm_dump(peer_manager_t *pm);

#endif