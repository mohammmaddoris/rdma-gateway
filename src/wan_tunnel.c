/*
 * wan_tunnel.c - WAN 报文转发与 ARP 缓存实现
 */
#include "wan_tunnel.h"
#include "log.h"
#include "peer_manager.h"
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_malloc.h>
#include <string.h>
#include <rte_arp.h>

extern peer_manager_t g_peer_manager;

#define ARP_CACHE_SIZE 128
#define DEFAULT_GW_MAC {{0x00, 0x01, 0x02, 0x03, 0x04, 0x05}}

typedef struct {
    uint32_t ip_addr;
    struct rte_ether_addr mac_addr;
    uint64_t timestamp;
    uint8_t valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static rte_spinlock_t arp_cache_lock;

void arp_cache_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
    rte_spinlock_init(&arp_cache_lock);
    LOG_INFO("arp_cache: ready");
}

int arp_cache_add(uint32_t ip_addr, struct rte_ether_addr *mac_addr) {
    char ip_buf[32];
    rte_spinlock_lock(&arp_cache_lock);

    int idx = ip_addr % ARP_CACHE_SIZE;
    arp_cache[idx].ip_addr = ip_addr;
    rte_ether_addr_copy(mac_addr, &arp_cache[idx].mac_addr);
    arp_cache[idx].timestamp = rte_rdtsc();
    arp_cache[idx].valid = 1;

    ip_to_str(rte_cpu_to_be_32(ip_addr), ip_buf, sizeof(ip_buf));
    LOG_DEBUGF("ARP cache added: IP=%s -> MAC=%02x:%02x:%02x:%02x:%02x:%02x",
               ip_buf,
               mac_addr->addr_bytes[0], mac_addr->addr_bytes[1],
               mac_addr->addr_bytes[2], mac_addr->addr_bytes[3],
               mac_addr->addr_bytes[4], mac_addr->addr_bytes[5]);

    rte_spinlock_unlock(&arp_cache_lock);
    return 0;
}

int arp_cache_lookup(uint32_t ip_addr, struct rte_ether_addr *mac_addr) {
    rte_spinlock_lock(&arp_cache_lock);

    int idx = ip_addr % ARP_CACHE_SIZE;
    if (arp_cache[idx].valid && arp_cache[idx].ip_addr == ip_addr) {
        rte_ether_addr_copy(&arp_cache[idx].mac_addr, mac_addr);
        rte_spinlock_unlock(&arp_cache_lock);
        return 0;
    }

    rte_spinlock_unlock(&arp_cache_lock);
    return -1;
}

static int send_arp_request(uint32_t target_ip, uint16_t port, struct rte_mempool *pool) {
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    if (!m)
        return -1;

    size_t arp_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
    char *data = rte_pktmbuf_append(m, arp_size);
    if (!data) {
        rte_pktmbuf_free(m);
        return -1;
    }

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);

    memset(eth->d_addr.addr_bytes, 0xFF, 6);
    
    struct rte_ether_addr src_mac;
    rte_eth_macaddr_get(port, &src_mac);
    rte_ether_addr_copy(&src_mac, &eth->s_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

    arp->arp_hrd = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
    arp->arp_pro = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    arp->arp_hln = 6;
    arp->arp_pln = 4;
    arp->arp_op = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);
    rte_ether_addr_copy(&src_mac, &arp->arp_sha);

    arp->arp_sip = rte_cpu_to_be_32(wan_tunnel_src_ip);
    memset(arp->arp_tha.addr_bytes, 0, 6);
    arp->arp_tip = rte_cpu_to_be_32(target_ip);

    if (rte_eth_tx_burst(port, 0, &m, 1) == 0) {
        LOG_WARN("Failed to send ARP request");
        rte_pktmbuf_free(m);
        return -1;
    }

    char ip_buf[32];
    ip_to_str(rte_cpu_to_be_32(target_ip), ip_buf, sizeof(ip_buf));
    LOG_DEBUGF("ARP request sent for IP %s", ip_buf);
    return 0;
}

static int resolve_next_hop(uint32_t dst_ip_be, uint16_t out_port,
                            struct rte_ether_addr *out_mac, int *cache_hit) {
    char ip_buf[32];
    uint32_t dst_ip = rte_be_to_cpu_32(dst_ip_be);

    if (arp_cache_lookup(dst_ip, out_mac) == 0) {
        if (cache_hit) *cache_hit = 1;
        return 0;
    }
    if (cache_hit) *cache_hit = 0;

    ip_to_str(dst_ip_be, ip_buf, sizeof(ip_buf));
    LOG_WARNF("ARP cache miss for destination IP %s, using default gateway", ip_buf);

    /* ARP miss 时回退默认网关 MAC，部署时需替换为真实地址 */
    struct rte_ether_addr def_gw_mac = DEFAULT_GW_MAC;
    rte_ether_addr_copy(&def_gw_mac, out_mac);

    /* 节流 ARP 请求：5 秒内只触发一次，避免 miss 风暴 */
    static uint64_t last_arp_time = 0;
    uint64_t current_time = rte_rdtsc();
    if (current_time - last_arp_time > rte_get_timer_hz() * 5) {
        last_arp_time = current_time;
        return 1;
    }
    return 0;
}

struct rte_mbuf* wan_fwd_roce(struct rte_mbuf *m, uint16_t out_port) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
    char src_ip_buf[32], dst_ip_buf[32];

    ip_to_str(ip->src_addr, src_ip_buf, sizeof(src_ip_buf));
    ip_to_str(ip->dst_addr, dst_ip_buf, sizeof(dst_ip_buf));
    LOG_DEBUGF("Forwarding RoCEv2 packet to WAN: src=%s:%u, dst=%s:%u",
               src_ip_buf, rte_be_to_cpu_16(udp->src_port),
               dst_ip_buf, rte_be_to_cpu_16(udp->dst_port));

    uint32_t original_dst_ip = ip->dst_addr;

    ip->src_addr = rte_cpu_to_be_32(wan_tunnel_src_ip);
    ip->hdr_checksum = 0;

    struct rte_ether_addr next_hop_mac;
    int need_arp = resolve_next_hop(original_dst_ip, out_port, &next_hop_mac, NULL);
    rte_ether_addr_copy(&next_hop_mac, &eth->d_addr);

    if (need_arp) {
        struct rte_mempool *pool = rte_mbuf_from_lib_mp(m);
        send_arp_request(rte_be_to_cpu_32(original_dst_ip), out_port, pool);
    }

    struct rte_ether_addr src_mac;
    rte_eth_macaddr_get(out_port, &src_mac);
    rte_ether_addr_copy(&src_mac, &eth->s_addr);

    return m;
}

int wan_fwd_passthrough(struct rte_mbuf *m, uint16_t out_port) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    char src_ip_buf[32], dst_ip_buf[32];

    ip_to_str(ip->src_addr, src_ip_buf, sizeof(src_ip_buf));
    ip_to_str(ip->dst_addr, dst_ip_buf, sizeof(dst_ip_buf));
    LOG_DEBUGF("Passthrough forward to WAN: src=%s, dst=%s", src_ip_buf, dst_ip_buf);

    struct rte_ether_addr next_hop_mac;
    int need_arp = resolve_next_hop(ip->dst_addr, out_port, &next_hop_mac, NULL);
    rte_ether_addr_copy(&next_hop_mac, &eth->d_addr);

    if (need_arp) {
        struct rte_mempool *pool = rte_mbuf_from_lib_mp(m);
        send_arp_request(rte_be_to_cpu_32(ip->dst_addr), out_port, pool);
    }

    struct rte_ether_addr src_mac;
    rte_eth_macaddr_get(out_port, &src_mac);
    rte_ether_addr_copy(&src_mac, &eth->s_addr);

    if (rte_eth_tx_burst(out_port, 0, &m, 1) == 0) {
        LOG_WARN("Failed to send passthrough packet to WAN");
        rte_pktmbuf_free(m);
        return -1;
    }

    return 0;
}

int wan_prepare_for_lan(struct rte_mbuf *m) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);

    ip->src_addr = rte_cpu_to_be_32(wan_tunnel_src_ip);
    ip->hdr_checksum = 0;

    return 0;
}

uint32_t wan_get_peer_ip(uint32_t dst_ip) {
    return pm_lookup(&g_peer_manager, dst_ip);
}
