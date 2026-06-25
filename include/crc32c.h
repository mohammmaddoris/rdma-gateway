/*
 * CRC32C (Castagnoli), SSE4.2 hw if available
 */
#ifndef _CRC32C_H_
#define _CRC32C_H_

#include <stdint.h>
#include <stddef.h>

uint32_t crc32c_calculate(const uint8_t *data, size_t length);
int crc32c_hw_supported(void);

#endif
