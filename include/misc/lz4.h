#pragma once

#include <misc/stdint.h>
#include <misc/stddef.h>

#define LZ4_MAX_INPUT_SIZE 0x7E000000

#define LZ4_COMPRESSBOUND(isize)                                               \
	((isize) > LZ4_MAX_INPUT_SIZE ? 0 : (isize) + ((isize) / 255) + 16)

int lz4_compress(const void *src, int src_size, void *dst, int dst_capacity);
int lz4_decompress(const void *src, int src_size, void *dst, int dst_capacity);
int lz4_compress_fast(const void *src, int src_size, void *dst,
		      int dst_capacity, int acceleration);
