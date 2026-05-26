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

enum Exception {
	InstructionMisaligned = 0,
	InstructionAccessFault = 1,
	IllegalInstruction = 2,
	Breakpoint = 3,
	LoadMisaligned = 4,
	LoadAccessFault = 5,
	StoreMisaligned = 6,
	StoreAccessFault = 7,
	UserEnvCall = 8,
	SupervisorEnvCall = 9,
	MachineEnvCall = 11,
	InstructionPageFault = 12,
	LoadPageFault = 13,
	StorePageFault = 15,
};

enum Interrupt {
	UserSoft = 0,
	SupervisorSoft,
	UserTimer = 4,
	SupervisorTimer,
	UserExternal = 8,
	SupervisorExternal,
};
void intr_off();
void intr_on();
void restore_intr(u64 old);

int trap_init();
