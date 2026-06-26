/*
 * RoCEv2 protocol fields & opcodes
 * minimal set needed for WRITE path processing
 */
#ifndef _ROCE_DEFS_H_
#define _ROCE_DEFS_H_

#include <stdint.h>
#include <rte_byteorder.h>

/* RoCEv2 OpCode definitions */
#define ROCE_OPCODE_WRITE_FIRST     0x0A
#define ROCE_OPCODE_WRITE_MIDDLE    0x0B
#define ROCE_OPCODE_WRITE_LAST      0x0C
#define ROCE_OPCODE_WRITE_ONLY      0x0D
#define ROCE_OPCODE_WRITE_LAST_IMM  0x0E
#define ROCE_OPCODE_ACK             0x11
#define ROCE_OPCODE_CNP             0x81

/* Base Transport Header, 12 bytes */
struct roce_bth {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t pkey;
    uint8_t  fecn_becn_res;
    uint8_t  dest_qp[3];     // 24-bit, big-endian
    uint8_t  ack_res;
    uint8_t  psn[3];         // 24-bit, big-endian
} __attribute__((packed));

static inline uint32_t bth_get_qpn(const struct roce_bth *b) {
    return (b->dest_qp[0] << 16) | (b->dest_qp[1] << 8) | b->dest_qp[2];
}
static inline void bth_set_qpn(struct roce_bth *b, uint32_t qpn) {
    b->dest_qp[0] = qpn >> 16;
    b->dest_qp[1] = qpn >> 8;
    b->dest_qp[2] = qpn;
}
static inline uint32_t bth_get_psn(const struct roce_bth *b) {
    return (b->psn[0] << 16) | (b->psn[1] << 8) | b->psn[2];
}
static inline void bth_set_psn(struct roce_bth *b, uint32_t psn) {
    b->psn[0] = psn >> 16;
    b->psn[1] = psn >> 8;
    b->psn[2] = psn;
}

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
