#pragma once

#include <misc/stdint.h>

struct bitmap {
	u8 *data;   // 位图数据缓冲区（字节对齐）
	size_t len; // 位图总长度，单位：bit
};

// 获取指定位的状态（0 或 1），越界返回 0
static inline int bitmap_get(const struct bitmap *bm, size_t bit_idx)
{
	if (bit_idx >= bm->len)
		return 0;
	size_t byte_idx = bit_idx >> 3;
	size_t bit_off = bit_idx & 7;
	return (bm->data[byte_idx] >> bit_off) & 1;
}

// 将指定位设为 1，越界则忽略
static inline void bitmap_set(struct bitmap *bm, size_t bit_idx)
{
	if (bit_idx >= bm->len)
		return;
	size_t byte_idx = bit_idx >> 3;
	size_t bit_off = bit_idx & 7;
	bm->data[byte_idx] |= (1U << bit_off);
}

// 将指定位清为 0，越界则忽略
static inline void bitmap_clear(struct bitmap *bm, size_t bit_idx)
{
	if (bit_idx >= bm->len)
		return;
	size_t byte_idx = bit_idx >> 3;
	size_t bit_off = bit_idx & 7;
	bm->data[byte_idx] &= ~(1U << bit_off);
}

// 翻转指定位的值（0↔1），越界则忽略
static inline void bitmap_flip(struct bitmap *bm, size_t bit_idx)
{
	if (bit_idx >= bm->len)
		return;
	size_t byte_idx = bit_idx >> 3;
	size_t bit_off = bit_idx & 7;
	bm->data[byte_idx] ^= (1U << bit_off);
}

// 根据 val（应为 0 或 1）设置指定位，越界则忽略
static inline void bitmap_assign(struct bitmap *bm, size_t bit_idx, int val)
{
	if (val)
		bitmap_set(bm, bit_idx);
	else
		bitmap_clear(bm, bit_idx);
}

// 从 bit_idx 开始向高地址方向查找第一个为 1 的位，返回其索引；若未找到，返回
// bm->len
static inline size_t bitmap_find_next_set(const struct bitmap *bm,
					  size_t bit_idx)
{
	if (bit_idx >= bm->len)
		return bm->len;

	size_t byte_idx = bit_idx >> 3;
	size_t bit_off = bit_idx & 7;
	u8 mask = (u8)(0xFFU << bit_off);

	// 检查起始字节剩余位
	u8 b = bm->data[byte_idx] & mask;
	if (b) {
		// 找到：计算最低位 1 的位置
		size_t off = 0;
		while ((b & 1) == 0) {
			b >>= 1;
			off++;
		}
		return (byte_idx << 3) + bit_off + off;
	}

	// 逐字节扫描后续字节
	for (size_t i = byte_idx + 1; i < (bm->len + 7) >> 3; i++) {
		b = bm->data[i];
		if (b) {
			size_t off = 0;
			while ((b & 1) == 0) {
				b >>= 1;
				off++;
			}
			size_t pos = (i << 3) + off;
			return (pos < bm->len) ? pos : bm->len;
		}
	}
	return bm->len;
}

// 从 bit_idx 开始向高地址方向查找第一个为 0 的位，返回其索引；若未找到，返回
// bm->len
static inline size_t bitmap_find_next_clear(const struct bitmap *bm,
					    size_t bit_idx)
{
	if (bit_idx >= bm->len)
		return bm->len;

	size_t byte_idx = bit_idx >> 3;
	size_t bit_off = bit_idx & 7;
	u8 mask = (u8)(0xFFU << bit_off);

	// 检查起始字节剩余位
	u8 b = (~bm->data[byte_idx]) & mask;
	if (b) {
		size_t off = 0;
		while ((b & 1) == 0) {
			b >>= 1;
			off++;
		}
		return (byte_idx << 3) + bit_off + off;
	}

	// 逐字节扫描后续字节
	size_t max_byte = (bm->len + 7) >> 3;
	for (size_t i = byte_idx + 1; i < max_byte; i++) {
		b = ~bm->data[i];
		// 若非最后字节，可全用；否则需掩码末尾无效位
		if (i == max_byte - 1) {
			size_t tail_bits = bm->len & 7;
			if (tail_bits) {
				b &= (1U << tail_bits) - 1;
			}
		}
		if (b) {
			size_t off = 0;
			while ((b & 1) == 0) {
				b >>= 1;
				off++;
			}
			return (i << 3) + off;
		}
	}
	return bm->len;
}
