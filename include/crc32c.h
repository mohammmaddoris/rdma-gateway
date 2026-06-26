/*
 * IEEE CRC-32 (poly 0xEDB88320) for the RoCEv2 ICRC
 */
#ifndef _CRC32C_H_
#define _CRC32C_H_

#include <stdint.h>
#include <stddef.h>

/* raw reflected IEEE CRC-32, no input/output inversion */
uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t length);
uint32_t crc32_le_slice8(uint32_t crc, const uint8_t *data, size_t length);

#endif
