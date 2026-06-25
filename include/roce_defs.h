/*
 * roce_defs.h - RoCEv2 协议字段与 OpCode 定义
 *
 * 仅保留网关实际处理 WRITE 路径所需的最小集合，其它 OpCode
 * (SEND/READ/ATOMIC 等) 不在此声明，避免误导调用方。
 */
#ifndef _ROCE_DEFS_H_
#define _ROCE_DEFS_H_

#include <stdint.h>
#include <rte_byteorder.h>

/* RoCEv2 OpCode 定义 */
#define ROCE_OPCODE_WRITE_FIRST     0x0A
#define ROCE_OPCODE_WRITE_MIDDLE    0x0B
#define ROCE_OPCODE_WRITE_LAST      0x0C
#define ROCE_OPCODE_WRITE_ONLY      0x0D
#define ROCE_OPCODE_WRITE_LAST_IMM  0x0E
#define ROCE_OPCODE_ACK             0x11

/* Base Transport Header (BTH)，12 字节 */
struct roce_bth {
    uint8_t  opcode;
    uint8_t  tver_pad;      // 高 4 位 TVer，低 4 位 Pad 计数
    uint16_t pkey;
    uint8_t  f_b_se_m;      // 标志位 F/B/SE/M 各 1 位，余 4 位保留
    uint8_t  pad_res;       // Pad 2 位，保留 6 位
    uint32_t dqpn;          // 目的 QPN(24 位) + 保留(8 位)
    uint32_t psn;           // PSN(24 位) + 保留(8 位)，ACK 中为 APSN
} __attribute__((packed));

/* ACK Extended Transport Header (AETH)，4 字节 */
struct roce_aeth {
    uint8_t  syndrome;      // ACK 为 0x00，NAK 为 0x60
    uint8_t  msn;           // 消息序号
    uint16_t credit;        // 信用值(0xFFFF 表示不限)
} __attribute__((packed));

/* RDMA Extension Header (RETH)，16 字节，仅 WRITE/READ 使用 */
struct roce_reth {
    uint64_t va;            // 虚拟地址
    uint32_t rkey;          // 远程内存键
    uint32_t len;           // DMA 长度
} __attribute__((packed));

static inline int is_write_opcode(uint8_t opcode) {
    return (opcode >= ROCE_OPCODE_WRITE_FIRST && opcode <= ROCE_OPCODE_WRITE_LAST_IMM);
}

#endif