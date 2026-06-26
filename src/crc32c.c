/*
 * IEEE CRC-32 (poly 0xEDB88320), used for the RoCEv2 ICRC
 */
#include "crc32c.h"

static uint32_t crc32_table[256];
static int crc32_table_ready = 0;

static void init_crc32_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t length) {
    if (!crc32_table_ready) init_crc32_table();
    for (size_t i = 0; i < length; i++)
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
