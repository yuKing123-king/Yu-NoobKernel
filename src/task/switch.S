#
# 保存当前寄存器到old上下文，从new上下文中恢复寄存器
# @param a0 (old): 保存当前寄存器状态的context结构指针
# @param a1 (new): 待恢复的context结构指针
#
.globl context_switch
context_switch:
	sd ra, 0(a0)
	sd sp, 8(a0)
	sd gp, 16(a0)
	sd s0, 24(a0)
	sd s1, 32(a0)
	sd s2, 40(a0)
	sd s3, 48(a0)
	sd s4, 56(a0)
	sd s5, 64(a0)
	sd s6, 72(a0)
	sd s7, 80(a0)
	sd s8, 88(a0)
	sd s9, 96(a0)
	sd s10, 104(a0)
	sd s11, 112(a0)

	csrr t0, sstatus
	sd t0, 120(a0)

	ld ra, 0(a1)
	ld sp, 8(a1)
	ld gp, 16(a1)
	ld s0, 24(a1)
	ld s1, 32(a1)
	ld s2, 40(a1)
	ld s3, 48(a1)
	ld s4, 56(a1)
	ld s5, 64(a1)
	ld s6, 72(a1)
	ld s7, 80(a1)
	ld s8, 88(a1)
	ld s9, 96(a1)
	ld s10, 104(a1)
	ld s11, 112(a1)
	
	ld t0, 120(a1)
	csrw sstatus, t0

	ret

#
# 从new上下文中恢复所有寄存器（不保存当前状态）
# @param a0 (new): 待恢复的context结构指针
#
.globl context_switch_to
context_switch_to:
	sd s0, 24(a0)
	sd s1, 32(a0)
	sd s2, 40(a0)
	sd s3, 48(a0)
	sd s4, 56(a0)
	sd s5, 64(a0)
	sd s6, 72(a0)
	sd s7, 80(a0)
	sd s8, 88(a0)
	sd s9, 96(a0)
	sd s10, 104(a0)
	sd s11, 112(a0)

	ld ra, 0(a0)
	ld sp, 8(a0)
	ld s0, 24(a0)
	ld s1, 32(a0)
	ld s2, 40(a0)
	ld s3, 48(a0)
	ld s4, 56(a0)
	ld s5, 64(a0)
	ld s6, 72(a0)
	ld s7, 80(a0)
	ld s8, 88(a0)
	ld s9, 96(a0)
	ld s10, 104(a0)
	ld s11, 112(a0)

	ret