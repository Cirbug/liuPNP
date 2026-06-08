/*
 * base64.h — 轻量 Base64 编码, 无需外部依赖
 */

#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>
#include <stdint.h>

/* 编码 src[src_len] → dst[dst_len], 返回实际写入字节数 (不含 '\0')
 * dst 须至少为 ((src_len + 2) / 3) * 4 + 1 字节
 */
int base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len);

/* 计算编码后长度 */
static inline size_t base64_encoded_len(size_t raw_len) {
    return ((raw_len + 2) / 3) * 4;
}

#endif
