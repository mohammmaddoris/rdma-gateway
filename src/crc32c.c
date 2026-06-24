/*
 * crc32c.c - CRC32C 实现，SSE4.2 硬件优先，软件回退
 */
#include "crc32c.h"
#include <cpuid.h>

#ifdef __SSE4_2__
#include <nmmintrin.h>
#endif

static uint32_t crc32c_table[256];
static int table_initialized = 0;
static int hw_accel_supported = -1;

static void init_crc32c_table(void) {
    uint32_t i, j, crc;
    for (i = 0; i < 256; i++) {
        crc = i;
        for (j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0x82F63B78;
            else
                crc >>= 1;
        }
        crc32c_table[i] = crc;
    }
    table_initialized = 1;
}

int crc32c_hw_supported(void) {
    if (hw_accel_supported >= 0) {
        return hw_accel_supported;
    }

    unsigned int eax, ebx, ecx, edx;

    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
#ifdef __SSE4_2__
        if (ecx & bit_SSE4_2) {
            hw_accel_supported = 1;
            return 1;
        }
#endif
    }

    hw_accel_supported = 0;
    return 0;
}

static uint32_t crc32c_sw(const uint8_t *data, size_t length) {
    if (!table_initialized) init_crc32c_table();

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc = (crc >> 8) ^ crc32c_table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

static uint32_t crc32c_sse42(const uint8_t *data, size_t length) {
#ifdef __SSE4_2__
    uint32_t crc = 0xFFFFFFFF;

    while (length >= 8) {
        crc = _mm_crc32_u32(crc, *(uint32_t *)data);
        crc = _mm_crc32_u32(crc, *(uint32_t *)(data + 4));
        data += 8;
        length -= 8;
    }

    while (length--) {
        crc = _mm_crc32_u8(crc, *data++);
    }

    return crc ^ 0xFFFFFFFF;
#else
    return crc32c_sw(data, length);
#endif
}

uint32_t crc32c_calculate(const uint8_t *data, size_t length) {
    if (length == 0) return 0;

    if (crc32c_hw_supported()) {
        return crc32c_sse42(data, length);
    } else {
        return crc32c_sw(data, length);
    }
}

/* 批量 CRC32C：逐个调用单缓冲实现 */
void crc32c_batch_calculate(crc32c_task_t *tasks, int count) {
    if (count <= 0) return;

    for (int i = 0; i < count; i++) {
        tasks[i].result = crc32c_calculate(tasks[i].data, tasks[i].length);
    }
}
