/*
 * common.h - 跨模块共用的小工具
 */
#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>
#include "qp_ctx.h"   /* PSN_MASK, MAX_PSN */

/* 24 位 PSN 前向距离，处理回绕 */
static inline uint32_t psn_forward_dist(uint32_t from, uint32_t to)
{
    /* 先掩到 24 位，避免调用方忘做 & PSN_MASK */
    from &= PSN_MASK;
    to   &= PSN_MASK;
    return (to >= from) ? (to - from) : (MAX_PSN - from + to);
}

#endif /* _COMMON_H_ */
