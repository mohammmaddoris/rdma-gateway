/* cross-module helpers */
#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>
#include "qp_ctx.h"   /* PSN_MASK, MAX_PSN */

static inline uint32_t psn_forward_dist(uint32_t from, uint32_t to)
{
    from &= PSN_MASK;
    to   &= PSN_MASK;
    return (to >= from) ? (to - from) : (MAX_PSN - from + to);
}

#endif /* _COMMON_H_ */
