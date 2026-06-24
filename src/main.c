/*
 * main.c - RDMA Gateway 入口：EAL 初始化、端口配置、lcore 调度
 */
#include <stdio.h>
#include <stdlib.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>
#include <rte_timer.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_memzone.h>
#include <rte_pci.h>
#include <signal.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <string.h>
#include <ctype.h>
#include "log.h"
#include "processor.h"
#include "wan_tunnel.h"
#include "peer_manager.h"
#include "roce_defs.h"

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define BURST_SIZE 32

#define LAN_PORT 0
#define WAN_PORT 1

#define APP_TIMER_INTERVAL_MS 10

uint32_t wan_tunnel_src_ip = 0;

static processor_context_t g_processors[MAX_LCORES];
static struct rte_mempool *g_mbuf_pool = NULL;
uint16_t g_lan_port = LAN_PORT;
uint16_t g_wan_port = WAN_PORT;
struct rte_mempool *g_wan_pool;
static uint8_t g_running = 1;
static uint32_t g_lan_core_count = 1;
static uint32_t g_wan_core_count = 1;

peer_manager_t g_peer_manager;

static uint16_t port_name_to_id(const char *name) {
    if (!name || !name[0]) {
        LOG_ERROR("Port name is empty");
        return (uint16_t)-1;
    }

    uint16_t nb_ports = rte_eth_dev_count_avail();

    if (isdigit(name[0])) {
        uint16_t port_id = (uint16_t)atoi(name);
        if (port_id < nb_ports) {
            return port_id;
        }
        LOG_ERRORF("Invalid port number %u, max available ports: %u", port_id, nb_ports);
        return (uint16_t)-1;
    }

    for (uint16_t port_id = 0; port_id < nb_ports; port_id++) {
        struct rte_eth_dev_info dev_info;
        if (rte_eth_dev_info_get(port_id, &dev_info) != 0) {
            continue;
        }

        if (dev_info.driver_name && strcmp(dev_info.driver_name, name) == 0) {
            return port_id;
        }

        if (dev_info.pci_dev && dev_info.pci_dev->name && strcmp(dev_info.pci_dev->name, name) == 0) {
            return port_id;
        }
    }

    LOG_ERRORF("Cannot find port with name: %s", name);
    LOG_ERROR("Available ports:");
    for (uint16_t port_id = 0; port_id < nb_ports; port_id++) {
        struct rte_eth_dev_info dev_info;
        if (rte_eth_dev_info_get(port_id, &dev_info) == 0) {
            LOG_ERRORF("  Port %u: driver=%s, pci=%s",
                       port_id,
                       dev_info.driver_name ? dev_info.driver_name : "unknown",
                       dev_info.pci_dev && dev_info.pci_dev->name ? dev_info.pci_dev->name : "unknown");
        }
    }

    return (uint16_t)-1;
}

static int port_init(uint16_t port, struct rte_mempool *mbuf_pool, uint32_t num_rx_queues) {
    struct rte_eth_conf port_conf = {0};
    struct rte_eth_dev_info dev_info;
    int retval;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;

    if (!rte_eth_dev_is_valid_port(port)) {
        LOG_ERRORF("Invalid port %u", port);
        return -1;
    }

    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        LOG_ERRORF("Failed to get dev info for port %u: %d", port, retval);
        return retval;
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    retval = rte_eth_dev_configure(port, num_rx_queues, 1, &port_conf);
    if (retval != 0) {
        LOG_ERRORF("Failed to configure port %u with %u RX queues: %d", port, num_rx_queues, retval);
        return retval;
    }

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0) {
        LOG_ERRORF("Failed to adjust descriptors for port %u: %d", port, retval);
        return retval;
    }

    for (uint32_t i = 0; i < num_rx_queues; i++) {
        retval = rte_eth_rx_queue_setup(port, i, nb_rxd, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0) {
            LOG_ERRORF("Failed to setup RX queue %u for port %u: %d", i, port, retval);
            return retval;
        }
    }

    retval = rte_eth_tx_queue_setup(port, 0, nb_txd, rte_eth_dev_socket_id(port), NULL);
    if (retval < 0) {
        LOG_ERRORF("Failed to setup TX queue for port %u: %d", port, retval);
        return retval;
    }

    retval = rte_eth_dev_start(port);
    if (retval < 0) {
        LOG_ERRORF("Failed to start port %u: %d", port, retval);
        return retval;
    }

    rte_eth_promiscuous_enable(port);
    LOG_INFOF("Port %u initialized successfully with %u RX queues", port, num_rx_queues);
    return 0;
}

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        LOG_INFOF("Received signal %d, shutting down...", signum);
        g_running = 0;
    }
}

static void cleanup(void) {
    log_close_file();
    LOG_INFO("Cleanup completed");
}

static int parse_peer_string(const char *str, uint32_t *subnet, uint32_t *mask, uint32_t *peer_ip) {
    char subnet_str[32];
    char peer_str[32];
    int prefix_len;

    if (strstr(str, "default:") == str) {
        *subnet = 0;
        *mask = 0;
        *peer_ip = inet_addr(str + 8);
        return 1;
    }

    if (sscanf(str, "%31[^/]/%d:%31s", subnet_str, &prefix_len, peer_str) == 3) {
        *subnet = inet_addr(subnet_str);
        *mask = htonl((0xFFFFFFFF << (32 - prefix_len)) & 0xFFFFFFFF);
        *peer_ip = inet_addr(peer_str);
        return 0;
    }

    return -1;
}

static int lan_main_loop(void *arg) {
    lcore_args_t *args = (lcore_args_t *)arg;
    processor_context_t *proc = args->proc;
    uint16_t lan_port = args->lan_port;
    uint16_t wan_port = args->wan_port;
    struct rte_mempool *pool = args->mbuf_pool;

    struct rte_mbuf *lan_bufs[BURST_SIZE];
    uint16_t nb_rx;

    uint32_t lcore_id = rte_lcore_id();

    while (g_running) {
        for (uint32_t queue_id = 0; queue_id < g_lan_core_count; queue_id++) {
            nb_rx = rte_eth_rx_burst(lan_port, queue_id, lan_bufs, BURST_SIZE);
            if (nb_rx > 0) {
                for (uint16_t i = 0; i < nb_rx; i++) {
                    struct rte_mbuf *m = lan_bufs[i];
                    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
                    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
                    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
                    struct roce_bth *bth = (struct roce_bth *)(udp_hdr + 1);
                    uint32_t qpn = rte_be_to_cpu_32(bth->dqpn) >> 8;
                    
                    uint32_t target_lcore = proc_flow_to_lan_lcore(ip_hdr, udp_hdr, qpn);
                    if (target_lcore == lcore_id) {
                        proc_process_lan_rx(proc, m, lan_port, wan_port, pool);
                    } else {
                        rte_pktmbuf_free(m);
                    }
                }
            }
        }

        rte_pause();
    }

    LOG_INFOF("LAN main loop exiting on lcore %u", lcore_id);
    return 0;
}

static int wan_main_loop(void *arg) {
    lcore_args_t *args = (lcore_args_t *)arg;
    processor_context_t *proc = args->proc;
    uint16_t lan_port = args->lan_port;
    uint16_t wan_port = args->wan_port;
    struct rte_mempool *pool = args->mbuf_pool;

    struct rte_mbuf *wan_bufs[BURST_SIZE];
    uint16_t nb_rx;
    uint64_t timer_count = 0;

    uint32_t lcore_id = rte_lcore_id();

    uint64_t timer_interval = rte_get_timer_hz() * APP_TIMER_INTERVAL_MS / 1000;
    uint64_t last_timer = rte_rdtsc();

    while (g_running) {
        for (uint32_t queue_id = 0; queue_id < g_wan_core_count; queue_id++) {
            nb_rx = rte_eth_rx_burst(wan_port, queue_id, wan_bufs, BURST_SIZE);
            if (nb_rx > 0) {
                for (uint16_t i = 0; i < nb_rx; i++) {
                    struct rte_mbuf *m = wan_bufs[i];
                    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
                    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
                    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
                    struct roce_bth *bth = (struct roce_bth *)(udp_hdr + 1);
                    uint32_t qpn = rte_be_to_cpu_32(bth->dqpn) >> 8;
                    
                    uint32_t target_lcore = proc_flow_to_wan_lcore(ip_hdr, udp_hdr, qpn);
                    if (target_lcore == lcore_id) {
                        proc_process_wan_rx(proc, m, lan_port, wan_port, pool);
                    } else {
                        rte_pktmbuf_free(m);
                    }
                }
            }
        }

        uint64_t current = rte_rdtsc();
        if (current - last_timer >= timer_interval) {
            proc_timer_callback(proc, wan_port, pool);
            last_timer = current;
            timer_count++;

            if (timer_count % 100 == 0) {
                LOG_DEBUGF("Timer tick %lu on WAN lcore %u", timer_count, lcore_id);
            }
        }

        rte_pause();
    }

    LOG_INFOF("WAN main loop exiting on lcore %u", lcore_id);
    return 0;
}

static void print_usage(const char *prgname) {
    printf("Usage: %s [EAL options] -- [APP options]\n"
           "  -h, --help         Show this help message\n"
           "  -l, --lan-port     LAN port number (default: %d)\n"
           "  -w, --wan-port     WAN port number (default: %d)\n"
           "  -s, --src-ip       WAN source IP address (required)\n"
           "  --lan-cores        Number of cores for LAN side processing (default: 1)\n"
           "  --wan-cores        Number of cores for WAN side processing (default: 1)\n"
           "  --peer             Peer gateway mapping (format: subnet/prefix:peer-ip or default:peer-ip)\n"
           "                     Can be specified multiple times\n"
           "  -v, --verbose      Enable verbose logging\n"
           "  -f, --log-file     Log file path (logs will be written to both file and stdout)\n"
           "\n"
           "Examples:\n"
           "  %s -l 0-3 -- -l 0 -w 1 -s 10.0.0.100 --lan-cores 2 --wan-cores 2 --peer default:10.0.0.200\n"
           "  %s -l 0-5 -- -l 0 -w 1 -s 10.0.0.100 --lan-cores 3 --wan-cores 3 --peer 192.168.1.0/24:10.0.0.200\n"
           "  %s -l 0-3 -- -l 0 -w 1 -s 10.0.0.100 --peer default:10.0.0.200 -f /var/log/rdma_gateway.log\n",
           prgname, LAN_PORT, WAN_PORT, prgname, prgname, prgname);
}

int main(int argc, char *argv[]) {
    int ret;
    uint16_t nb_ports;
    int verbose = 0;
    char *src_ip_str = NULL;
    char *log_file_path = NULL;

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"lan-port", required_argument, 0, 'l'},
        {"wan-port", required_argument, 0, 'w'},
        {"src-ip", required_argument, 0, 's'},
        {"lan-cores", required_argument, 0, 'a'},
        {"wan-cores", required_argument, 0, 'b'},
        {"peer", required_argument, 0, 'p'},
        {"verbose", no_argument, 0, 'v'},
        {"log-file", required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };

    ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "EAL initialization failed\n");
        return -1;
    }
    argc -= ret;
    argv += ret;

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "hl:w:s:a:b:vp:f:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'l': {
                uint16_t port_id = port_name_to_id(optarg);
                if (port_id == (uint16_t)-1) {
                    fprintf(stderr, "Invalid LAN port: %s\n", optarg);
                    return -1;
                }
                g_lan_port = port_id;
                break;
            }
            case 'w': {
                uint16_t port_id = port_name_to_id(optarg);
                if (port_id == (uint16_t)-1) {
                    fprintf(stderr, "Invalid WAN port: %s\n", optarg);
                    return -1;
                }
                g_wan_port = port_id;
                break;
            }
            case 's':
                src_ip_str = optarg;
                break;
            case 'a':
                g_lan_core_count = (uint32_t)atoi(optarg);
                break;
            case 'b':
                g_wan_core_count = (uint32_t)atoi(optarg);
                break;
            case 'p': {
                uint32_t subnet, mask, peer_ip;
                int result = parse_peer_string(optarg, &subnet, &mask, &peer_ip);
                if (result == 0) {
                    pm_add(&g_peer_manager, subnet, mask, peer_ip);
                } else if (result == 1) {
                    pm_set_default(&g_peer_manager, peer_ip);
                } else {
                    LOG_ERRORF("Invalid peer format: %s", optarg);
                    return -1;
                }
                break;
            }
            case 'v':
                verbose = 1;
                break;
            case 'f':
                log_file_path = optarg;
                break;
            default:
                print_usage(argv[0]);
                return -1;
        }
    }

    if (!src_ip_str) {
        fprintf(stderr, "Missing required --src-ip argument\n");
        print_usage(argv[0]);
        return -1;
    }

    uint32_t total_cores_needed = g_lan_core_count + g_wan_core_count;
    uint32_t available_cores = rte_lcore_count();
    if (total_cores_needed > available_cores) {
        LOG_ERRORF("Not enough cores! Requested: %u (LAN:%u + WAN:%u), Available: %u",
                   total_cores_needed, g_lan_core_count, g_wan_core_count, available_cores);
        return -1;
    }

    if (verbose) {
        log_set_level(LOG_LEVEL_DEBUG);
    } else {
        log_set_level(LOG_LEVEL_INFO);
    }

    if (log_file_path) {
        log_open_file(log_file_path);
    }

    wan_tunnel_src_ip = inet_addr(src_ip_str);
    char ip_buf[32];
    ip_to_str(wan_tunnel_src_ip, ip_buf, sizeof(ip_buf));
    LOG_INFOF("WAN tunnel source IP: %s", ip_buf);

    /* 强制 1G 大页，避免 2M 页 TLB 抖动 */
    uint64_t hugepage_size = rte_mem_get_dma_pagesize();
    if (hugepage_size != RTE_PGSIZE_1G) {
        LOG_ERRORF("1G HugePages not configured! Current page size: %lu bytes", hugepage_size);
        LOG_ERROR("Please configure 1G HugePages and restart with EAL parameters:");
        LOG_ERROR("  --huge-dir=/dev/hugepages --single-file-segments");
        return -1;
    }
    LOG_INFOF("1G HugePages enabled: page size = %lu bytes", hugepage_size);

    proc_set_core_counts(g_lan_core_count, g_wan_core_count);
    pm_init(&g_peer_manager);
    arp_cache_init();

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports < 2) {
        LOG_ERROR("Need at least 2 ports (LAN and WAN)");
        return -1;
    }

    g_mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * total_cores_needed,
                                          MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                          rte_socket_id());
    if (!g_mbuf_pool) {
        LOG_ERROR("Failed to create mbuf pool");
        return -1;
    }

    if (port_init(g_lan_port, g_mbuf_pool, g_lan_core_count) != 0) {
        LOG_ERRORF("Failed to initialize LAN port %u", g_lan_port);
        return -1;
    }

    if (port_init(g_wan_port, g_mbuf_pool, g_wan_core_count) != 0) {
        LOG_ERRORF("Failed to initialize WAN port %u", g_wan_port);
        return -1;
    }

    LOG_INFOF("Initializing %u processor contexts (LAN:%u, WAN:%u)",
              total_cores_needed, g_lan_core_count, g_wan_core_count);

    for (uint32_t i = 0; i < g_lan_core_count; i++) {
        proc_init(&g_processors[i], i, 1);
    }
    for (uint32_t i = g_lan_core_count; i < total_cores_needed; i++) {
        proc_init(&g_processors[i], i, 0);
    }

    pm_dump(&g_peer_manager);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    LOG_INFOF("Starting RDMA Gateway with %u LAN cores and %u WAN cores...",
             g_lan_core_count, g_wan_core_count);

    lcore_args_t lcore_args[MAX_LCORES];

    for (uint32_t i = 0; i < g_lan_core_count; i++) {
        lcore_args[i].proc = &g_processors[i];
        lcore_args[i].lan_port = g_lan_port;
        lcore_args[i].wan_port = g_wan_port;
        lcore_args[i].mbuf_pool = g_mbuf_pool;
        lcore_args[i].is_lan_core = 1;
    }
    for (uint32_t i = g_lan_core_count; i < total_cores_needed; i++) {
        lcore_args[i].proc = &g_processors[i];
        lcore_args[i].lan_port = g_lan_port;
        lcore_args[i].wan_port = g_wan_port;
        lcore_args[i].mbuf_pool = g_mbuf_pool;
        lcore_args[i].is_lan_core = 0;
    }

    RTE_LCORE_FOREACH_SLAVE(lcore_id) {
        if (lcore_id >= total_cores_needed) {
            continue;
        }
        
        if (lcore_id < g_lan_core_count) {
            ret = rte_eal_remote_launch(lan_main_loop, &lcore_args[lcore_id], lcore_id);
        } else {
            ret = rte_eal_remote_launch(wan_main_loop, &lcore_args[lcore_id], lcore_id);
        }
        
        if (ret != 0) {
            LOG_ERRORF("Failed to launch lcore %u", lcore_id);
            return -1;
        }
    }

    uint32_t master_lcore = rte_lcore_id();
    if (master_lcore < total_cores_needed) {
        if (master_lcore < g_lan_core_count) {
            lan_main_loop(&lcore_args[master_lcore]);
        } else {
            wan_main_loop(&lcore_args[master_lcore]);
        }
    }

    rte_eal_wait_lcore();

    LOG_INFO("Shutting down...");
    rte_eth_dev_stop(g_lan_port);
    rte_eth_dev_stop(g_wan_port);
    rte_mempool_free(g_mbuf_pool);
    cleanup();

    return 0;
}