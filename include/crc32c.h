/*
 * crc32c.h - CRC32C (iSCAST 多项式) 计算，硬件优先
 *
 * 批量接口 crc32c_batch_calculate 当前逐个调用单次实现；
 * 之前的 AVX512 多缓冲并行路径因 CRC32C 不可按位并行而废弃。
 */
#ifndef _CRC32C_H_
#define _CRC32C_H_

#include <stdint.h>
#include <stddef.h>

#define CRC32C_BATCH_SIZE 16

typedef struct {
    const uint8_t *data;
    size_t length;
    uint32_t result;
} crc32c_task_t;

uint32_t crc32c_calculate(const uint8_t *data, size_t length);
void crc32c_batch_calculate(crc32c_task_t *tasks, int count);
int crc32c_hw_supported(void);

#endif
