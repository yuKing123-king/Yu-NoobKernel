#include <trap/trap.h>
#include <task/sched.h>
#include <misc/log.h>
#include <hal/plic.h>
#include <hal/riscv.h>
#include <config.h>

extern void virtio_blk_isr(void);

extern char trampoline[], uservec[];
extern char userret[], kernelvec[];
extern bool sched_enabled;

extern void handle_timer(void);
extern void handle_external(void);
extern void handle_ipi(void);

void handle_external(void)
{
	u64 hartid = r_mhartid();
	u32 irqno = plic_claim('S', hartid);

	if (irqno == 0) {
		return;
	}

	switch (irqno) {
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
	case 7:
	case 8:
		if (irqno == 1) {
			virtio_blk_isr();
		}
		break;
	default:
		warnf("handle_external: unknown irq %d", irqno);
		break;
	}

	plic_complete('S', hartid, irqno);
}

// 关中断：支持嵌套调用
void intr_off(void)
{
	struct cpu *c = thiscpu();
	if (c->intr_depth == 0) {
		// 第一次关中断：保存原始状态
		c->intr_state = r_sstatus();
		w_sstatus(c->intr_state & ~SSTATUS_SIE);
	}
	c->intr_depth++;
}

// 开中断：只有最外层才能真正开中断
void intr_on(void)
{
	struct cpu *c = thiscpu();
	if (c->intr_depth <= 0) {
		panic("intr_on: mismatch");
	}
	c->intr_depth--;
	if (c->intr_depth == 0) {
		// 最外层退出：恢复原始中断状态
		w_sstatus(c->intr_state);
	}
}

// 恢复中断状态（用于异常/系统调用返回等场景）
void restore_intr(u64 old)
{
	struct cpu *c = thiscpu();
	// 强制重置嵌套状态
	c->intr_depth = 0;
	c->intr_state = old;
	w_sstatus(old);
}

void set_kerneltrap()
{
	w_stvec((u64)kernelvec & ~0x3); // DIRECT
}

// set up to take exceptions and traps while in the kernel.
int trap_init()
{
	set_kerneltrap();
	w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);
	w_sstatus(r_sstatus() | SSTATUS_SIE);
	return 0;
}

void kerneltrap(struct ktrapframe *ktf)
{
	u64 scause = ktf->scause;
	u64 sepc = ktf->sepc;
	u64 sstatus = ktf->sstatus;

	// 检查是否为中断（scause 最高位为 1）
	if ((scause & (1UL << 63)) == 0) {
		// ========== 异常（Exception）处理 ==========
		// 目前内核不应发生异常（如非法指令、缺页等）
		// 若发生，说明内核 bug
		switch (scause) {
		case IllegalInstruction:
			panic("kernel illegal instruction at %p", sepc);
		case LoadPageFault:  // load page fault
		case StorePageFault: // store/AMO page fault
			panic("kernel page fault at %p, stval=%p", sepc,
			      r_stval());
		case UserEnvCall: // environment call from U-mode (ecall)
			panic("kernel ecall at %p (should not happen)", sepc);
		default:
			panic("kernel exception: scause=%lx, sepc=%p", scause,
			      sepc);
		}
	}

	// ========== 中断（Interrupt）处理 ==========
	int irq = scause & 0x3FF; // 提取中断号（低 10 位）

	switch (irq) {
	case SupervisorSoft: // Supervisor Software Interrupt (SSI)
		// handle_ipi();
		break;

	case SupervisorTimer: // Supervisor Timer Interrupt (STI)
		handle_timer();
		break;

	case SupervisorExternal: // Supervisor External Interrupt (SEI)
		handle_external();
		break;

	default:
		panic("unknown kernel interrupt: irq=%d (scause=%lx)", irq,
		      scause);
	}

	if (sched_enabled && thiscpu()->need_resched)
		sched_yield();
}
