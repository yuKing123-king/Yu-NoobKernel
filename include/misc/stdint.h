#pragma once

typedef signed char __int8_t;
typedef short __int16_t;
typedef int __int32_t;
typedef long long __int64_t;
typedef unsigned char __uint8_t;
typedef unsigned short __uint16_t;
typedef unsigned int __uint32_t;
typedef unsigned long long __uint64_t;

#define INT8_TYPE __int8_t
#define INT16_TYPE __int16_t
#define INT32_TYPE __int32_t
#define INT64_TYPE __int64_t
#define UINT8_TYPE __uint8_t
#define UINT16_TYPE __uint16_t
#define UINT32_TYPE __uint32_t
#define UINT64_TYPE __uint64_t
#define INTMAX_TYPE __int64_t
#define UINTMAX_TYPE __uint64_t
#define INTPTR_TYPE __int64_t
#define UINTPTR_TYPE __uint64_t
#define SIZE_TYPE __uint64_t
#define SSIZE_TYPE __int64_t
#define PTRDIFF_TYPE __int64_t

#define INT8_MAX 0x7f
#define INT16_MAX 0x7fff
#define INT32_MAX 0x7fffffff
#define INT64_MAX 0x7fffffffffffffff
#define UINT8_MAX 0xff
#define UINT16_MAX 0xffff
#define UINT32_MAX 0xffffffff
#define UINT64_MAX 0xffffffffffffffff
#define INT8_MIN (-INT8_MAX - 1)
#define INT16_MIN (-INT16_MAX - 1)
#define INT32_MIN (-INT32_MAX - 1)
#define INT64_MIN (-INT64_MAX - 1)
#define UINT8_MIN 0
#define UINT16_MIN 0
#define UINT32_MIN 0
#define UINT64_MIN 0
#define INT8_C(c) c
#define INT16_C(c) c
#define INT32_C(c) c
#define INT64_C(c) c##LL
#define UINT8_C(c) c##U
#define UINT16_C(c) c##U
#define UINT32_C(c) c##U
#define UINT64_C(c) c##ULL
#define INTMAX_MAX INT64_MAX
#define INTMAX_MIN INT64_MIN
#define UINTMAX_MAX UINT64_MAX
#define INTMAX_C(c) c##LL
#define UINTMAX_C(c) c##ULL
#define SIZE_MAX UINT64_MAX
#define SIZE_MIN UINT64_MIN
#define SIZE_C(c) c##ULL
#define PTRDIFF_MAX INT64_MAX
#define PTRDIFF_MIN INT64_MIN
#define PTRDIFF_C(c) c##LL
#define SSIZE_MAX INT64_MAX
#define SSIZE_MIN INT64_MIN
#define SSIZE_C(c) c##LL
#define INTPTR_MAX INT64_MAX
#define INTPTR_MIN INT64_MIN
#define UINTPTR_MAX UINT64_MAX
#define UINTPTR_MIN UINT64_MIN
#define INTPTR_C(c) c##LL
#define UINTPTR_C(c) c##ULL

#define INT8_WIDTH 8
#define INT16_WIDTH 16
#define INT32_WIDTH 32
#define INT64_WIDTH 64
#define UINT8_WIDTH 8
#define UINT16_WIDTH 16
#define UINT32_WIDTH 32
#define UINT64_WIDTH 64
#define INTMAX_WIDTH 64
#define UINTMAX_WIDTH 64
#define INTPTR_WIDTH 64
#define UINTPTR_WIDTH 64
#define SIZE_WIDTH 64
#define SSIZE_WIDTH 64
#define PTRDIFF_WIDTH 64

typedef INT8_TYPE int8_t;
typedef INT16_TYPE int16_t;
typedef INT32_TYPE int32_t;
typedef INT64_TYPE int64_t;
typedef UINT8_TYPE uint8_t;
typedef UINT16_TYPE uint16_t;
typedef UINT32_TYPE uint32_t;
typedef UINT64_TYPE uint64_t;

typedef SIZE_TYPE size_t;
typedef SSIZE_TYPE ssize_t;
typedef INTPTR_TYPE intptr_t;
typedef UINTPTR_TYPE uintptr_t;
typedef PTRDIFF_TYPE ptrdiff_t;
typedef INTMAX_TYPE intmax_t;
typedef UINTMAX_TYPE uintmax_t;

typedef INT8_TYPE s8;
typedef INT16_TYPE s16;
typedef INT32_TYPE s32;
typedef INT64_TYPE s64;
typedef UINT8_TYPE u8;
typedef UINT16_TYPE u16;
typedef UINT32_TYPE u32;
typedef UINT64_TYPE u64;

