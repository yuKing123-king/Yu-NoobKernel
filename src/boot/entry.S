#define __ASSEMBLY__
#include <config.h>

	.section .text.entry
	.globl _entry
_entry:
	li t0, BOOT_STACK_SIZE
	mul t0, a0, t0
	la t1, boot_stack_top
	sub sp, t1, t0
	call main

	.section .bss.stack
	.globl boot_stack
boot_stack:
	.space BOOT_STACK_SIZE * CPU_NUM
	.globl boot_stack_top
boot_stack_top:
