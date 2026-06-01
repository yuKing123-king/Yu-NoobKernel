#define __ASSEMBLY__
#include <config.h>

	.section .text.entry
	.globl _entry
/*
 * 系统启动入口点
 * 根据 HART ID 计算每个核心的栈指针（基于 BOOT_STACK_SIZE），
 * 然后跳转到 main 函数
 */
_entry:
	li t0, BOOT_STACK_SIZE
	mul t0, a0, t0
	la t1, boot_stack_top
	sub sp, t1, t0
	call main

	.section .bss.stack
	.globl boot_stack
/*
 * 启动栈空间
 * 为每个 CPU 核心分配 BOOT_STACK_SIZE 字节的栈空间
 */
boot_stack:
	.space BOOT_STACK_SIZE * CPU_NUM
	.globl boot_stack_top
/*
 * 启动栈顶标签
 * 指向 boot_stack 的末尾（栈区域最高地址）
 */
boot_stack_top:
