/*
 * RoCEv2 protocol fields & opcodes
 * minimal set needed for WRITE path processing
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

/* Base Transport Header, 12 bytes */
struct roce_bth {
    uint8_t  opcode;
    uint8_t  tver_pad;
    uint16_t pkey;
    uint8_t  f_b_se_m;
    uint8_t  pad_res;
    uint32_t dqpn;          // dest QPN (24 bits) + reserved (8 bits)
    uint32_t psn;           // PSN (24 bits) + reserved (8 bits)
} __attribute__((packed));

/* ACK Extended Transport Header, 4 bytes */
struct roce_aeth {
    uint8_t  syndrome;
    uint8_t  msn;
    uint16_t credit;
} __attribute__((packed));

/* RDMA Extension Header, 16 bytes, WRITE/READ only */
struct roce_reth {
    uint64_t va;
    uint32_t rkey;
    uint32_t len;
} __attribute__((packed));

static inline int is_write_opcode(uint8_t opcode) {
    return (opcode >= ROCE_OPCODE_WRITE_FIRST && opcode <= ROCE_OPCODE_WRITE_LAST_IMM);
}

#endif