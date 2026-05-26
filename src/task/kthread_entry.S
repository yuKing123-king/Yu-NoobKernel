.section .text
.globl kthread_start
.align 4
kthread_start:
	ld t0, 0(sp)   # fn
	ld a0, 8(sp)   # arg
	addi sp, sp, 16
	jalr t0

	# 线程函数返回后，自动退出
	call kthread_exit