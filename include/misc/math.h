#pragma once

#include <misc/stdint.h>

/**
 * @brief 计算大于或等于输入值的最小2的幂次的对数
 * @param size 要计算的数值
 * @return 如果size为0返回-1，否则返回log2(向上取整到2的幂次)
 */
static int log2_ceil(size_t size) {
	if (size == 0) {
		return -1; // 返回错误值表示无效输入
	}

	size_t value = size;
	int log2_result = 0;

	// 使用二分查找方式确定最高位1的位置
	if (value >= 0x100000000ULL) {
		log2_result += 32;
		value >>= 32;
	}
	if (value >= 0x10000) {
		log2_result += 16;
		value >>= 16;
	}
	if (value >= 0x100) {
		log2_result += 8;
		value >>= 8;
	}
	if (value >= 0x10) {
		log2_result += 4;
		value >>= 4;
	}
	if (value >= 0x4) {
		log2_result += 2;
		value >>= 2;
	}
	if (value >= 0x2) {
		log2_result += 1;
	}

	// 如果size不是2的幂，需要加1
	if ((size & (size - 1)) != 0) {
		log2_result++;
	}

	return log2_result;
}
