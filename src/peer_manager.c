/*
 * peer_manager.c - 子网到对端网关映射实现
 */
#include "peer_manager.h"
#include "log.h"
#include <string.h>

void pm_init(peer_manager_t *pm) {
    memset(pm, 0, sizeof(peer_manager_t));
}

int pm_add(peer_manager_t *pm, uint32_t subnet, uint32_t subnet_mask, uint32_t peer_ip) {
    if (pm->peer_count >= MAX_PEERS) {
        LOG_ERROR("Peer manager full, cannot add more peers");
        return -1;
    }

    pm->peers[pm->peer_count].subnet = subnet;
    pm->peers[pm->peer_count].subnet_mask = subnet_mask;
    pm->peers[pm->peer_count].peer_ip = peer_ip;
    memset(&pm->peers[pm->peer_count].peer_mac, 0, sizeof(struct rte_ether_addr));
    pm->peer_count++;

    char subnet_buf[32], peer_buf[32];
    ip_to_str(subnet, subnet_buf, sizeof(subnet_buf));
    ip_to_str(peer_ip, peer_buf, sizeof(peer_buf));
    LOG_INFOF("Added peer mapping: subnet=%s/%u -> peer=%s",
              subnet_buf, __builtin_popcount(subnet_mask), peer_buf);

    return 0;
}

int pm_set_default(peer_manager_t *pm, uint32_t peer_ip) {
    pm->default_peer_ip = peer_ip;
    memset(&pm->default_peer_mac, 0, sizeof(struct rte_ether_addr));

    char ip_buf[32];
    ip_to_str(peer_ip, ip_buf, sizeof(ip_buf));

    return 0;
}

uint32_t pm_lookup(peer_manager_t *pm, uint32_t dst_ip) {
    char dst_buf[32], peer_buf[32];

    for (uint32_t i = 0; i < pm->peer_count; i++) {
        if ((dst_ip & pm->peers[i].subnet_mask) == pm->peers[i].subnet) {
            ip_to_str(dst_ip, dst_buf, sizeof(dst_buf));
            ip_to_str(pm->peers[i].peer_ip, peer_buf, sizeof(peer_buf));
            LOG_DEBUGF("Peer lookup hit: dst=%s -> peer=%s", dst_buf, peer_buf);
            return pm->peers[i].peer_ip;
        }
    }

    if (pm->default_peer_ip != 0) {
        ip_to_str(dst_ip, dst_buf, sizeof(dst_buf));
        ip_to_str(pm->default_peer_ip, peer_buf, sizeof(peer_buf));
        LOG_DEBUGF("Peer lookup using default: dst=%s -> default_peer=%s", dst_buf, peer_buf);
        return pm->default_peer_ip;
    }

    ip_to_str(dst_ip, dst_buf, sizeof(dst_buf));
    LOG_WARNF("Peer lookup failed: no mapping found for dst=%s", dst_buf);
    return 0;
}

void pm_dump(peer_manager_t *pm) {
    LOG_INFO("=== Peer Manager Configuration ===");
    char subnet_buf[32], peer_buf[32];

    LOG_INFOF("Total peers: %u", pm->peer_count);

    for (uint32_t i = 0; i < pm->peer_count; i++) {
        ip_to_str(pm->peers[i].subnet, subnet_buf, sizeof(subnet_buf));
        ip_to_str(pm->peers[i].peer_ip, peer_buf, sizeof(peer_buf));
        LOG_INFOF("Peer %u: subnet=%s/%u -> peer=%s",
                  i, subnet_buf,
                  __builtin_popcount(pm->peers[i].subnet_mask),
                  peer_buf);
    }

    if (pm->default_peer_ip != 0) {
        ip_to_str(pm->default_peer_ip, peer_buf, sizeof(peer_buf));
        LOG_INFOF("Default peer: %s", peer_buf);
    } else {
        LOG_INFO("No default peer configured");
    }
    LOG_INFO("===================================");
}
