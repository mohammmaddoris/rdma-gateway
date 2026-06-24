/*
 * wan_tunnel.h - WAN 侧报文转发与 ARP 缓存
 *
 */
#ifndef _WAN_TUNNEL_H_
#define _WAN_TUNNEL_H_

#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mempool.h>

#define WAN_UDP_PORT 4791
#define ARQ_CTRL_PORT 4792

extern uint32_t wan_tunnel_src_ip;

struct rte_mbuf* wan_fwdRoCE(struct rte_mbuf *m, uint16_t out_port);
int wan_fwdPassthrough(struct rte_mbuf *m, uint16_t out_port);
int wan_prepareForLAN(struct rte_mbuf *m);
uint32_t wan_getPeerIP(uint32_t dst_ip);

void arp_cache_init(void);
int arp_cache_add(uint32_t ip_addr, struct rte_ether_addr *mac_addr);
int arp_cache_lookup(uint32_t ip_addr, struct rte_ether_addr *mac_addr);

#endif