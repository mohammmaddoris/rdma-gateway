# RDMA Gateway

基于 DPDK 的 RoCEv2 WAN 网关：在两端 RDMA 集群之间透传 RoCEv2 WRITE 流量，
通过伪造 ACK + ARQ 重传，在有损的 WAN 链路上维持 RDMA 的可靠语义。

## 工作原理

LAN 侧（面向本地 RDMA 主机）：

- 拦截发往对端子网的 RoCEv2 WRITE 报文（UDP 4791）；
- 立即向源主机伪造 ACK，释放其发送窗口；
- 将数据转发到 WAN，并登记进 ARQ 发送窗口。

WAN 侧（面向对端网关）：

- 接收对端数据，按 PSN 放入抖动缓冲重组整条 WRITE 消息；
- 发现缺段时回送 NACK 请求重传，收齐后整体下发本地 LAN；
- ARQ 控制报文（ACK/NACK）走独立 UDP 端口 4792，与数据流分离。

其余：缓冲水位过高时发送 CNP 做粗粒度拥塞控制；非 RoCE 或无对端映射的流量直接透传。

## 目录结构

    include/   各模块头文件
    src/
      main.c            EAL 初始化、端口/lcore 配置与收发主循环
      processor.c       单 lcore 报文处理主逻辑
      arq.c             ARQ 发送窗口与重传
      jitter_buffer.c   WAN 侧 WRITE 重组
      congestion.c      水位线拥塞控制 + CNP
      wan_tunnel.c      WAN 转发与 ARP 缓存
      qp_ctx.c          QP 注册表
      peer_manager.c    子网 → 对端网关映射
      crc32c.c          ICRC 计算（优先 SSE4.2）
      stats.c / log.c   计数器与日志

## 构建

依赖 DPDK（`pkg-config` 能找到 `libdpdk`）：

    make

## 运行

需预先配置 1G 大页，且至少 2 个网口（LAN / WAN）。`--` 之前为 EAL 参数，之后为应用参数：

    ./rdma_gateway -l 0-3 -- -l 0 -w 1 -s 10.0.0.100 \
        --lan-cores 2 --wan-cores 2 --peer default:10.0.0.200

应用参数：

    -l / -w                   LAN / WAN 端口
    -s                        WAN 隧道源 IP（必填）
    --lan-cores / --wan-cores 两侧处理核数
    --peer                    对端映射，subnet/prefix:peer-ip 或 default:peer-ip，可重复
    -f                        日志文件路径
    -v                        调试日志
