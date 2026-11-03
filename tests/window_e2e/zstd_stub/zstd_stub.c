/* Test double for zstd (tests/window_e2e only). */
#include "zstd.h"
#include <string.h>
#define ERR ((size_t)-1)
size_t ZSTD_compressBound(size_t n) { return n; }
size_t ZSTD_compress(void* d, size_t cap, const void* s, size_t n, int level)
{
	(void)level;
	if (n > cap) return ERR;
	memcpy(d, s, n);
	return n;
}
size_t ZSTD_decompress(void* d, size_t cap, const void* s, size_t n)
{
	if (n > cap) return ERR;
	if (n) memcpy(d, s, n);
	return n;
}
unsigned ZSTD_isError(size_t code) { return code == ERR; }
