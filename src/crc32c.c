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

static uint32_t crc32_slice[8][256];
static int crc32_slice_ready = 0;

static void init_crc32_slice(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        crc32_slice[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = crc32_slice[0][i];
        for (int n = 1; n < 8; n++) {
            c = crc32_slice[0][c & 0xFF] ^ (c >> 8);
            crc32_slice[n][i] = c;
        }
    }
    crc32_slice_ready = 1;
}

uint32_t crc32_le_slice8(uint32_t crc, const uint8_t *data, size_t length) {
    if (!crc32_slice_ready) init_crc32_slice();
    while (length >= 8) {
        crc ^= (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
               ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        uint32_t t = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                     ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
        crc = crc32_slice[7][crc & 0xFF] ^ crc32_slice[6][(crc >> 8) & 0xFF] ^
              crc32_slice[5][(crc >> 16) & 0xFF] ^ crc32_slice[4][(crc >> 24) & 0xFF] ^
              crc32_slice[3][t & 0xFF] ^ crc32_slice[2][(t >> 8) & 0xFF] ^
              crc32_slice[1][(t >> 16) & 0xFF] ^ crc32_slice[0][(t >> 24) & 0xFF];
        data += 8;
        length -= 8;
    }
    while (length--)
        crc = crc32_slice[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    return crc;
}
