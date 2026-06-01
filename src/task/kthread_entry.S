.section .text
#
# 内核线程入口点
# 从栈中取出线程函数(fn)和参数(arg)，调用fn(arg)，返回后自动调用kthread_exit
#
.globl kthread_start
.align 4
kthread_start:
	ld t0, 0(sp)   # fn
	ld a0, 8(sp)   # arg
	addi sp, sp, 16
	jalr t0

	# 线程函数返回后，自动退出
	call kthread_exit