/*
 * jitter_buffer.c - WRITE 消息重组实现
 */
#include "jitter_buffer.h"
#include "log.h"
#include "common.h"
#include <string.h>
#include <rte_cycles.h>

extern uint16_t g_wan_port;

void jb_init(jitter_buffer_t *jb) {
    memset(jb, 0, sizeof(jitter_buffer_t));
    rte_spinlock_init(&jb->global_lock);
    jb->active_write_count = 0;
}

/* 按 QPN 查找重组上下文，未命中返回 NULL；调用方需持有 global_lock */
static jb_write_context_t *jb_find(jitter_buffer_t *jb, uint32_t qpn) {
    for (int i = 0; i < JB_MAX_PENDING_WRITES; i++) {
        if (jb->writes[i].qpn == qpn) {
            return &jb->writes[i];
        }
    }
    return NULL;
}

jb_write_context_t* jb_get_or_create_write(jitter_buffer_t *jb, uint32_t qpn, uint32_t first_psn) {
    rte_spinlock_lock(&jb->global_lock);

    for (int i = 0; i < JB_MAX_PENDING_WRITES; i++) {
        if (jb->writes[i].qpn == qpn && jb->writes[i].segment_count > 0) {
            rte_spinlock_unlock(&jb->global_lock);
            return &jb->writes[i];
        }
    }

    for (int i = 0; i < JB_MAX_PENDING_WRITES; i++) {
        if (jb->writes[i].segment_count == 0 && !jb->writes[i].qpn) {
            memset(&jb->writes[i], 0, sizeof(jb_write_context_t));
            jb->writes[i].qpn = qpn;
            jb->writes[i].expected_psn = first_psn;
            jb->writes[i].first_psn = first_psn;
            rte_spinlock_init(&jb->writes[i].lock);
            jb->active_write_count++;
            rte_spinlock_unlock(&jb->global_lock);
            return &jb->writes[i];
        }
    }

    rte_spinlock_unlock(&jb->global_lock);
    LOG_ERRORF("Jitter Buffer full: cannot create new write context for QPN: 0x%x", qpn);
    return NULL;
}

int jb_add_seg(jitter_buffer_t *jb, uint32_t qpn, uint32_t psn, uint8_t segment_type, struct rte_mbuf *m) {
    rte_spinlock_lock(&jb->global_lock);

    jb_write_context_t *write_ctx = jb_find(jb, qpn);
    if (!write_ctx) {
        write_ctx = jb_get_or_create_write(jb, qpn, psn);
        if (!write_ctx) {
            rte_spinlock_unlock(&jb->global_lock);
            return -1;
        }
    }

    rte_spinlock_lock(&write_ctx->lock);

    uint32_t idx = psn_forward_dist(write_ctx->first_psn, psn) % JB_MAX_SEGMENTS;

    if (write_ctx->segments[idx].state == JB_SEGMENT_RECEIVED) {
        LOG_DEBUGF("Duplicate segment PSN %u for QPN 0x%x, ignoring", psn, qpn);
        rte_spinlock_unlock(&write_ctx->lock);
        rte_spinlock_unlock(&jb->global_lock);
        rte_pktmbuf_free(m);
        return 0;
    }

    write_ctx->segments[idx].psn = psn;
    write_ctx->segments[idx].mbuf = m;
    write_ctx->segments[idx].state = JB_SEGMENT_RECEIVED;
    write_ctx->segments[idx].segment_type = segment_type;
    write_ctx->segments[idx].recv_time = rte_rdtsc();

    write_ctx->segment_count++;

    uint32_t expected = write_ctx->expected_psn;
    uint32_t dist = psn_forward_dist(expected, psn);

    /* 乱序：超前到达则标记中间空缺为 WAITING，供 NACK 检测 */
    if (dist == 0) {
        write_ctx->expected_psn = (write_ctx->expected_psn + 1) & 0x00FFFFFF;
    } else if (dist < JB_MAX_SEGMENTS) {
        for (uint32_t i = 0; i < dist; i++) {
            uint32_t check_psn = (expected + i) & 0x00FFFFFF;
            uint32_t check_idx = psn_forward_dist(write_ctx->first_psn, check_psn) % JB_MAX_SEGMENTS;
            if (write_ctx->segments[check_idx].state == JB_SEGMENT_FREE) {
                write_ctx->segments[check_idx].state = JB_SEGMENT_WAITING;
            }
        }
    }

    if (segment_type == WRITE_SEG_LAST || segment_type == WRITE_SEG_ONLY) {
        write_ctx->write_complete = 1;
    }

    rte_spinlock_unlock(&write_ctx->lock);
    rte_spinlock_unlock(&jb->global_lock);
    return 1;
}

int jb_is_write_complete(jitter_buffer_t *jb, uint32_t qpn) {
    rte_spinlock_lock(&jb->global_lock);

    jb_write_context_t *write_ctx = jb_find(jb, qpn);
    int complete = write_ctx ? write_ctx->write_complete : 0;

    rte_spinlock_unlock(&jb->global_lock);
    return complete;
}

int jb_get_ordered_segs(jitter_buffer_t *jb, uint32_t qpn, struct rte_mbuf **out, uint32_t *count) {
    *count = 0;

    rte_spinlock_lock(&jb->global_lock);

    jb_write_context_t *write_ctx = jb_find(jb, qpn);
    if (!write_ctx) {
        rte_spinlock_unlock(&jb->global_lock);
        return -1;
    }

    rte_spinlock_lock(&write_ctx->lock);

    uint8_t all_received = 1;

    for (int i = 0; i < JB_MAX_SEGMENTS; i++) {
        if (write_ctx->segments[i].state == JB_SEGMENT_FREE) {
            all_received = 0;
            break;
        }

        if (write_ctx->segments[i].state == JB_SEGMENT_WAITING) {
            all_received = 0;
            break;
        }

        if (write_ctx->segments[i].mbuf) {
            out[*count] = write_ctx->segments[i].mbuf;
            (*count)++;
        }

        if (write_ctx->segments[i].segment_type == WRITE_SEG_LAST ||
            write_ctx->segments[i].segment_type == WRITE_SEG_ONLY) {
            break;
        }
    }

    if (!all_received) {
        *count = 0;
    }

    rte_spinlock_unlock(&write_ctx->lock);
    rte_spinlock_unlock(&jb->global_lock);

    return all_received ? 0 : -1;
}

void jb_remove_write(jitter_buffer_t *jb, uint32_t qpn) {
    rte_spinlock_lock(&jb->global_lock);

    jb_write_context_t *write_ctx = jb_find(jb, qpn);
    if (write_ctx) {
        for (int j = 0; j < JB_MAX_SEGMENTS; j++) {
            if (write_ctx->segments[j].mbuf) {
                rte_pktmbuf_free(write_ctx->segments[j].mbuf);
                write_ctx->segments[j].mbuf = NULL;
            }
        }
        memset(write_ctx, 0, sizeof(jb_write_context_t));
        jb->active_write_count--;
    }

    rte_spinlock_unlock(&jb->global_lock);
}

uint32_t jb_check_missing(jitter_buffer_t *jb, uint32_t qpn) {
    rte_spinlock_lock(&jb->global_lock);

    jb_write_context_t *write_ctx = jb_find(jb, qpn);
    if (!write_ctx) {
        rte_spinlock_unlock(&jb->global_lock);
        return 0;
    }

    rte_spinlock_lock(&write_ctx->lock);

    uint32_t expected = write_ctx->expected_psn;
    uint32_t check_idx = psn_forward_dist(write_ctx->first_psn, expected) % JB_MAX_SEGMENTS;
    uint32_t missing_psn = 0;

    if (write_ctx->segments[check_idx].state != JB_SEGMENT_RECEIVED) {
        missing_psn = expected;
    }

    rte_spinlock_unlock(&write_ctx->lock);
    rte_spinlock_unlock(&jb->global_lock);

    return missing_psn;
}

