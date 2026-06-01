#pragma once

#include <misc/stddef.h>
#include <hal/riscv.h>
#include <task/proc.h>

struct trapframe {
	/*   0 */ u64 kernel_satp;   // kernel page table
	/*   8 */ u64 kernel_sp;     // top of process's kernel stack
	/*  16 */ u64 kernel_trap;   // usertrap()
	/*  24 */ u64 epc;	     // saved user program counter
	/*  32 */ u64 kernel_hartid; // saved kernel tp
	/*  40 */ u64 ra;
	/*  48 */ u64 sp;
	/*  56 */ u64 gp;
	/*  64 */ u64 tp;
	/*  72 */ u64 t0;
	/*  80 */ u64 t1;
	/*  88 */ u64 t2;
	/*  96 */ u64 s0;
	/* 104 */ u64 s1;
	/* 112 */ u64 a0;
	/* 120 */ u64 a1;
	/* 128 */ u64 a2;
	/* 136 */ u64 a3;
	/* 144 */ u64 a4;
	/* 152 */ u64 a5;
	/* 160 */ u64 a6;
	/* 168 */ u64 a7;
	/* 176 */ u64 s2;
	/* 184 */ u64 s3;
	/* 192 */ u64 s4;
	/* 200 */ u64 s5;
	/* 208 */ u64 s6;
	/* 216 */ u64 s7;
	/* 224 */ u64 s8;
	/* 232 */ u64 s9;
	/* 240 */ u64 s10;
	/* 248 */ u64 s11;
	/* 256 */ u64 t3;
	/* 264 */ u64 t4;
	/* 272 */ u64 t5;
	/* 280 */ u64 t6;
};

// scause 异常码（同步陷阱，scause 最高位为 0）
enum Exception {
	InstructionMisaligned = 0,  // 取指地址未对齐
	InstructionAccessFault = 1, // 取指权限/地址非法（如访存超出物理内存）
	IllegalInstruction = 2,     // 非法指令（未定义的指令编码）
	Breakpoint = 3,             // 断点（ebreak 指令）
	LoadMisaligned = 4,         // load 地址未对齐
	LoadAccessFault = 5,        // load 权限/地址非法（如只读页上写入）
	StoreMisaligned = 6,        // store/AMO 地址未对齐
	StoreAccessFault = 7,       // store/AMO 权限/地址非法
	UserEnvCall = 8,            // 用户态 ecall（系统调用入口）
	SupervisorEnvCall = 9,      // S 态 ecall
	MachineEnvCall = 11,        // M 态 ecall
	InstructionPageFault = 12,  // 取指缺页（页表项无效/无权限）
	LoadPageFault = 13,         // 读缺页
	StorePageFault = 15,        // 写缺页
};

// scause 中断码（异步中断，scause 最高位为 1）
enum Interrupt {
	UserSoft = 0,         // U 态软件中断（其他 hart 写 mip.USIP）
	SupervisorSoft,       // S 态软件中断（核间中断 IPI）
	UserTimer = 4,        // U 态定时器中断
	SupervisorTimer,      // S 态定时器中断（时钟节拍/调度）
	UserExternal = 8,     // U 态外部中断（PLIC）
	SupervisorExternal,   // S 态外部中断（virtio/uart 等设备）
};
void intr_off();
void intr_on();
void restore_intr(u64 old);

int trap_init();

/* 用户态陷阱处理 */
void usertrap(void);
void usertrapret(struct proc *p);
void forkret(void);
