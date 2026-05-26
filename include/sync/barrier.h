#pragma once

/* 编译器屏障（防止编译器重排序） */
#define barrier() __asm__ __volatile__("" ::: "memory")

/* 内存屏障（全屏障） */
static inline void mb(void)
{
	__asm__ __volatile__("fence rw,rw" ::: "memory");
}

/* 读内存屏障 */
static inline void rmb(void) { __asm__ __volatile__("fence r,r" ::: "memory"); }

/* 写内存屏障 */
static inline void wmb(void) { __asm__ __volatile__("fence w,w" ::: "memory"); }
