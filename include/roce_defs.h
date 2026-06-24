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

/* RoCEv2 OpCode Definitions */
#define ROCE_OPCODE_WRITE_FIRST     0x0A
#define ROCE_OPCODE_WRITE_MIDDLE    0x0B
#define ROCE_OPCODE_WRITE_LAST      0x0C
#define ROCE_OPCODE_WRITE_ONLY      0x0D
#define ROCE_OPCODE_WRITE_LAST_IMM  0x0E
#define ROCE_OPCODE_ACK             0x11

/* Base Transport Header (BTH) - 12 Bytes */
struct roce_bth {
    uint8_t  opcode;
    uint8_t  tver_pad;      // Upper 4 bits: TVer, Lower 4 bits: Pad count
    uint16_t pkey;
    uint8_t  f_b_se_m;      // Flags: F(1), B(1), SE(1), M(1), Res(4)
    uint8_t  pad_res;       // Pad(2), Res(6)
    uint32_t dqpn;          // Destination QPN (24 bits) + Res (8 bits)
    uint32_t psn;           // PSN (24 bits) + Res (8 bits) (Actually APSN in ACKs)
} __attribute__((packed));

/* ACK Extended Transport Header (AETH) - 4 Bytes */
struct roce_aeth {
    uint8_t  syndrome;      // 0x00 for ACK, 0x60 for NAK
    uint8_t  msn;           // Message Sequence Number
    uint16_t credit;        // Credit count (0xFFFF = unlimited)
} __attribute__((packed));

/* RDMA Extension Header (RETH) - 16 Bytes (Only for WRITE/READ) */
struct roce_reth {
    uint64_t va;            // Virtual Address
    uint32_t rkey;          // Remote Key
    uint32_t len;           // DMA Length
} __attribute__((packed));

static inline int is_write_opcode(uint8_t opcode) {
    return (opcode >= ROCE_OPCODE_WRITE_FIRST && opcode <= ROCE_OPCODE_WRITE_LAST_IMM);
}

#endif