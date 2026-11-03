/* Test double for zstd (tests/window_e2e only): "compression" is a plain
 * copy. The real zstd is covered by tests/window_proto. */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
size_t ZSTD_compressBound(size_t srcSize);
size_t ZSTD_compress(void* dst, size_t dstCapacity, const void* src, size_t srcSize, int level);
size_t ZSTD_decompress(void* dst, size_t dstCapacity, const void* src, size_t srcSize);
unsigned ZSTD_isError(size_t code);
#ifdef __cplusplus
}
#endif
