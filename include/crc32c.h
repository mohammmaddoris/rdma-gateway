/*
 * crc32c.h - CRC32C (Castagnoli 多项式) 计算，优先走 SSE4.2 硬件指令
 */
#ifndef _CRC32C_H_
#define _CRC32C_H_

#include <stdint.h>
#include <stddef.h>

uint32_t crc32c_calculate(const uint8_t *data, size_t length);
int crc32c_hw_supported(void);

#endif
