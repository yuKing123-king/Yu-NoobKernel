#pragma once

#include <misc/stdint.h>

#define SEC(sec) __attribute__((section(sec), aligned(8)))

/* 编译时常量检测（C11 _Static_assert 的封装） */
#ifndef static_assert
#define static_assert _Static_assert
#endif

/* 防止函数内联 */
#define noinline __attribute__((noinline))

/* 强制内联 */
#define always_inline __attribute__((always_inline)) inline

/* 告知编译器某值可能被异步修改（如硬件寄存器） */
#define volatile_access(x) (*(volatile typeof(x) *)&(x))

/* 编译器提示：该分支很可能/不太可能执行 */
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* 空操作（用于占位或延迟） */
static inline void nop(void) { __asm__ __volatile__("nop"); }

static inline void wfi(void) { __asm__ __volatile__("wfi"); }
