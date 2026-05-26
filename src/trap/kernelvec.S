# 约定：
#   - gp = 指向当前进程的 struct ktrapframe
#   - 不修改 sp（中断前后使用同一栈）
#   - 所有寄存器保存/恢复通过 gp 直接寻址
#
.section .text
.globl kernelvec
.align 4

kernelvec:
    # 1. 读取关键 CSR（必须最先做，避免 sepc 被覆盖）
    csrr t0, sepc
    csrr t1, sstatus
    csrr t2, scause

    # 2. 保存 CSR 到 ktrapframe（偏移 0, 8, 16）
    sd t0, 0(gp)        # ktf->sepc
    sd t1, 8(gp)        # ktf->sstatus
    sd t2, 16(gp)       # ktf->scause

    # 3. 保存通用寄存器（按你 struct ktrapframe 偏移）
    sd ra,  24(gp)      # 24
    sd sp,  32(gp)      # 32 ← 保存中断前的 sp
    sd gp,  40(gp)      # 40 ← 保存旧 gp（即旧 ktf 指针）
    sd tp,  48(gp)      # 48
    sd t0,  56(gp)      # 56
    sd t1,  64(gp)      # 64
    sd t2,  72(gp)      # 72
    sd s0,  80(gp)      # 80
    sd s1,  88(gp)      # 88
    sd a0,  96(gp)      # 96
    sd a1, 104(gp)      # 104
    sd a2, 112(gp)      # 112
    sd a3, 120(gp)      # 120
    sd a4, 128(gp)      # 128
    sd a5, 136(gp)      # 136
    sd a6, 144(gp)      # 144
    sd a7, 152(gp)      # 152
    sd s2, 160(gp)      # 160
    sd s3, 168(gp)      # 168
    sd s4, 176(gp)      # 176
    sd s5, 184(gp)      # 184
    sd s6, 192(gp)      # 192
    sd s7, 200(gp)      # 200
    sd s8, 208(gp)      # 208
    sd s9, 216(gp)      # 216
    sd s10,224(gp)      # 224
    sd s11,232(gp)      # 232
    sd t3, 240(gp)      # 240
    sd t4, 248(gp)      # 248
    sd t5, 256(gp)      # 256
    sd t6, 264(gp)      # 264

    # 4. 调用 C 中断处理函数，传 ktf = gp
    mv a0, gp
    call kerneltrap

    # fall through to kernelret

.globl kernelret
kernelret:
    # 1. 恢复通用寄存器（顺序无关，因 gp 不变）
    ld ra,  24(gp)
    ld sp,  32(gp)      # ← 恢复中断前的 sp
    ld gp,  40(gp)      # ← 恢复 gp（即新进程的 ktf 指针）
    ld tp,  48(gp)
    ld t0,  56(gp)
    ld t1,  64(gp)
    ld t2,  72(gp)
    ld s0,  80(gp)
    ld s1,  88(gp)
    ld a0,  96(gp)
    ld a1, 104(gp)
    ld a2, 112(gp)
    ld a3, 120(gp)
    ld a4, 128(gp)
    ld a5, 136(gp)
    ld a6, 144(gp)
    ld a7, 152(gp)
    ld s2, 160(gp)
    ld s3, 168(gp)
    ld s4, 176(gp)
    ld s5, 184(gp)
    ld s6, 192(gp)
    ld s7, 200(gp)
    ld s8, 208(gp)
    ld s9, 216(gp)
    ld s10,224(gp)
    ld s11,232(gp)
    ld t3, 240(gp)
    ld t4, 248(gp)
    ld t5, 256(gp)
    ld t6, 264(gp)

    # 2. 恢复 CSR
    ld t0, 0(gp)        # sepc
    ld t1, 8(gp)        # sstatus
    csrw sepc, t0
    csrw sstatus, t1

    # 3. 返回 —— sp 已恢复为中断前栈，gp 为新进程 ktf
    sret