#pragma once

#include <misc/stdint.h>

// 定义字节序
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN 4321

#define BYTE_ORDER LITTLE_ENDIAN

// 字节交换函数
static inline UINT16_TYPE swap16(UINT16_TYPE x)
{
	return ((x << 8) & 0xff00) | ((x >> 8) & 0x00ff);
}

static inline UINT32_TYPE swap32(UINT32_TYPE x)
{
	return ((x << 24) & 0xff000000) | ((x << 8) & 0x00ff0000) |
	       ((x >> 8) & 0x0000ff00) | ((x >> 24) & 0x000000ff);
}

static inline UINT64_TYPE swap64(UINT64_TYPE x)
{
	return ((x << 56) & 0xff00000000000000ULL) |
	       ((x << 40) & 0x00ff000000000000ULL) |
	       ((x << 24) & 0x0000ff0000000000ULL) |
	       ((x << 8) & 0x000000ff00000000ULL) |
	       ((x >> 8) & 0x00000000ff000000ULL) |
	       ((x >> 24) & 0x0000000000ff0000ULL) |
	       ((x >> 40) & 0x000000000000ff00ULL) |
	       ((x >> 56) & 0x00000000000000ffULL);
}

typedef UINT16_TYPE be16;
typedef UINT32_TYPE be32;
typedef UINT64_TYPE be64;
typedef UINT16_TYPE le16;
typedef UINT32_TYPE le32;
typedef UINT64_TYPE le64;

#if BYTE_ORDER == LITTLE_ENDIAN
typedef le16 cpu16;
typedef le32 cpu32;
typedef le64 cpu64;
#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define le64_to_cpu(x) (x)
#define cpu_to_be16(x) swap16(x)
#define cpu_to_be32(x) swap32(x)
#define cpu_to_be64(x) swap64(x)
#define be16_to_cpu(x) swap16(x)
#define be32_to_cpu(x) swap32(x)
#define be64_to_cpu(x) swap64(x)
#elif BYTE_ORDER == BIG_ENDIAN
typedef be16 cpu16;
typedef be32 cpu32;
typedef be64 cpu64;
#define cpu_to_le16(x) swap16(x)
#define cpu_to_le32(x) swap32(x)
#define cpu_to_le64(x) swap64(x)
#define le16_to_cpu(x) swap16(x)
#define le32_to_cpu(x) swap32(x)
#define le64_to_cpu(x) swap64(x)
#define cpu_to_be16(x) (x)
#define cpu_to_be32(x) (x)
#define cpu_to_be64(x) (x)
#define be16_to_cpu(x) (x)
#define be32_to_cpu(x) (x)
#define be64_to_cpu(x) (x)
#else
#error "CPU ENDIAN NOT DEFINED"
#endif

