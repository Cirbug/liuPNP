/*
 * base64.c — 轻量 Base64 编码实现
 */

#include "base64.h"

static const char table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len) {
    if (!src || !dst || dst_len < base64_encoded_len(src_len) + 1)
        return -1;

    size_t i = 0, j = 0;
    while (i < src_len) {
        uint32_t b = src[i++] << 16;
        b |= (i < src_len) ? src[i++] << 8 : 0;
        b |= (i < src_len) ? src[i++] : 0;

        dst[j++] = table[(b >> 18) & 0x3F];
        dst[j++] = table[(b >> 12) & 0x3F];
        dst[j++] = table[(b >> 6) & 0x3F];
        dst[j++] = table[b & 0x3F];
    }

    /* padding */
    size_t pad = (3 - (src_len % 3)) % 3;
    for (size_t p = 0; p < pad; p++)
        dst[j - 1 - p] = '=';

    dst[j] = '\0';
    return j;
}
