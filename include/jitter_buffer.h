/*
 * jitter_buffer.h - WAN 侧 WRITE 消息重组缓冲
 *
 * 收齐 FIRST..(MIDDLE)..LAST/ONLY 后整体下发 LAN，
 * 期间用 NACK 请求缺失段。槽位为定长数组，简单但占用固定内存。
 */
#ifndef _JITTER_BUFFER_H_
#define _JITTER_BUFFER_H_

#include <rte_mbuf.h>
#include <rte_spinlock.h>
#include "qp_ctx.h"

#define JB_MAX_SEGMENTS 256
#define JB_MAX_PENDING_WRITES 64

typedef enum {
    JB_SEGMENT_FREE = 0,
    JB_SEGMENT_RECEIVED = 1,
    JB_SEGMENT_WAITING = 2
} jb_segment_state_t;

typedef struct {
    uint32_t psn;
    struct rte_mbuf *mbuf;
    jb_segment_state_t state;
    uint8_t segment_type;
    uint64_t recv_time;
} jb_segment_t;

typedef struct {
    uint32_t qpn;
    uint32_t expected_psn;
    jb_segment_t segments[JB_MAX_SEGMENTS];
    uint32_t first_psn;
    uint32_t segment_count;
    uint8_t write_complete;
    rte_spinlock_t lock;
} jb_write_context_t;

typedef struct {
    jb_write_context_t writes[JB_MAX_PENDING_WRITES];
    rte_spinlock_t global_lock;
    uint32_t active_write_count;
} jitter_buffer_t;

void jb_init(jitter_buffer_t *jb);
jb_write_context_t* jb_get_or_create_write(jitter_buffer_t *jb, uint32_t qpn, uint32_t first_psn);
int jb_add_seg(jitter_buffer_t *jb, uint32_t qpn, uint32_t psn, uint8_t segment_type, struct rte_mbuf *m);
int jb_is_write_complete(jitter_buffer_t *jb, uint32_t qpn);
/*
 * 取出按序排列的段指针。out 由调用方提供（容量需 >= JB_MAX_SEGMENTS），
 * 返回 0 表示成功，段数写入 *count。
 */
int jb_get_ordered_segs(jitter_buffer_t *jb, uint32_t qpn, struct rte_mbuf **out, uint32_t *count);
void jb_remove_write(jitter_buffer_t *jb, uint32_t qpn);
uint32_t jb_check_missing(jitter_buffer_t *jb, uint32_t qpn);

#endif