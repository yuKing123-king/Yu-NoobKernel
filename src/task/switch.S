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

	# Save FPU callee-saved registers (fs0-fs11) and fcsr
	fsd fs0, 128(a0)
	fsd fs1, 136(a0)
	fsd fs2, 144(a0)
	fsd fs3, 152(a0)
	fsd fs4, 160(a0)
	fsd fs5, 168(a0)
	fsd fs6, 176(a0)
	fsd fs7, 184(a0)
	fsd fs8, 192(a0)
	fsd fs9, 200(a0)
	fsd fs10, 208(a0)
	fsd fs11, 216(a0)
	frcsr t0
	sd t0, 224(a0)

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
	li t1, 0x2000             # SSTATUS_FS_INIT = bit 13
	csrs sstatus, t1           # 确保 FPU 始终启用

	# Restore FPU callee-saved registers (fs0-fs11) and fcsr
	fld fs0, 128(a1)
	fld fs1, 136(a1)
	fld fs2, 144(a1)
	fld fs3, 152(a1)
	fld fs4, 160(a1)
	fld fs5, 168(a1)
	fld fs6, 176(a1)
	fld fs7, 184(a1)
	fld fs8, 192(a1)
	fld fs9, 200(a1)
	fld fs10, 208(a1)
	fld fs11, 216(a1)
	ld t0, 224(a1)
	fscsr t0

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
	ld gp, 16(a0)
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
