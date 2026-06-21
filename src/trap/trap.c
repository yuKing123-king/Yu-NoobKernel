#include <trap/trap.h>
#include <task/sched.h>
#include <task/proc.h>
#include <misc/log.h>
#include <hal/plic.h>
#include <hal/riscv.h>
#include <hal/sbi.h>
#include <mm/pagetable.h>
#include <mm/vm.h>
#include <syscall/syscall.h>
#include <config.h>

extern void virtio_blk_isr(int irqno);
extern void virtio_net_isr(void);

extern char trampoline[], uservec[];
extern char userret[], kernelvec[];
extern bool sched_enabled;

extern void handle_timer(void);
extern void handle_external(void);
extern pagetable_t kpagetable;

/*
 * 处理外部中断（来自PLIC），根据中断号分发到对应设备的中断处理函数
 */
void handle_external(void)
{
	u64 hartid = r_mhartid();
	u32 irqno = plic_claim('S', hartid);

	if (irqno == 0) {
		return;
	}

	switch (irqno) {
	case 1:
		virtio_blk_isr(irqno);
		break;
	case 2:
		virtio_net_isr();
		break;
	default:
		warnf("handle_external: unknown irq %d", irqno);
		break;
	}

	plic_complete('S', hartid, irqno);
}

/*
 * 关闭中断，支持嵌套调用（第一次关闭时保存状态并屏蔽SIE，
 * 后续嵌套仅增加深度计数）
 */
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

/*
 * 开启中断，支持嵌套调用（只有最外层退出时才能真正恢复中断状态）
 */
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

/*
 * 恢复中断状态，强制重置中断嵌套状态（用于异常/系统调用返回等场景）
 * @param old: 要恢复的sstatus值
 */
void restore_intr(u64 old)
{
	struct cpu *c = thiscpu();
	// 强制重置嵌套状态
	c->intr_depth = 0;
	c->intr_state = old;
	w_sstatus(old);
}

/*
 * 设置内核陷阱向量为kernelvec（Direct模式）
 */
void set_kerneltrap()
{
	w_stvec((u64)kernelvec & ~0x3); // DIRECT
}

/*
 * 初始化内核陷阱处理，设置stvec、使能中断并开启SIE
 * @return: 成功返回0
 */
int trap_init()
{
	set_kerneltrap();
	w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);
	w_sstatus(r_sstatus() | SSTATUS_SIE | SSTATUS_FS_INIT);
	return 0;
}

/*
 * 内核陷阱/中断总入口处理函数，区分异常和中断并分发处理
 * @param ktf: 内核陷阱帧指针，包含中断/异常时保存的寄存器状态
 */
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
		case InstructionAccessFault:
		case InstructionPageFault:
			panic("kernel instruction fault at %p, stval=%p",
			      sepc, r_stval());
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

extern char trampoline[], uservec[], userret[];

/**
 * @brief 用户态陷阱入口，处理从用户态进入内核的异常/中断
 *        由 uservec 跳转到此函数（通过 p->tf->kernel_trap 设置）
 * @param 无（通过 thiscpu()->proc->tf 访问 trapframe）
 * @return 无返回值，最终通过 usertrapret 返回用户态
 */
void usertrap(void)
{
	uintptr_t scause = r_scause();
	struct proc *p = thiscpu()->proc;

	if ((scause & (1UL << 63)) == 0) {
		switch (scause) {
		case UserEnvCall:
			p->tf->epc += 4;
			intr_on();
			syscall();
			intr_off();
			break;
		default:
			panic("usertrap: unexpected exception scause=%lx, "
			      "sepc=%lx stval=%lx (pid %d %s)",
			      scause, r_sepc(), r_stval(), p->pid, p->comm);
		}
	} else {
		int irq = scause & 0x3FF;
		switch (irq) {
		case SupervisorTimer:
			handle_timer();
			break;
		case SupervisorExternal:
			handle_external();
			break;
		default:
			break;
		}
	}
	if (thiscpu()->need_resched)
		sched_yield();
	usertrapret(thiscpu()->proc);
}

/**
 * @brief 从用户态陷阱返回，设置 stvec/sepc/sstatus 并通过跳板页的 userret 返回到用户态
 * @param p 当前进程
 * @return 无返回值（通过 userret 的 sret 回到用户态）
 */
void usertrapret(struct proc *p)
{
	intr_off();

	w_stvec(TRAMPOLINE + ((uintptr_t)uservec - (uintptr_t)trampoline));

	p->tf->kernel_satp = MAKE_SATP(kpagetable);
	p->tf->kernel_sp = (uintptr_t)p->kstack + KSTACK_SIZE;
	p->tf->kernel_trap = (uintptr_t)usertrap;
	p->tf->kernel_hartid = r_tp();

	uintptr_t sstatus = r_sstatus();
	sstatus &= ~SSTATUS_SPP;
	sstatus |= SSTATUS_SPIE;
	sstatus |= SSTATUS_FS_INIT;  /* 启用用户态 FPU */
	w_sstatus(sstatus);

	w_sepc(p->tf->epc);
	w_sscratch(TRAPFRAME);

	uintptr_t userret_addr = TRAMPOLINE +
				 ((uintptr_t)userret - (uintptr_t)trampoline);
	void (*userret_func)(uintptr_t, uintptr_t) =
		(void (*)(uintptr_t, uintptr_t))userret_addr;

	userret_func(TRAPFRAME, MAKE_SATP(p->pagetable));
}

/**
 * @brief 用户进程首次内核上下文恢复时的入口
 *        由 context_switch 切换到用户进程时执行，然后跳转到用户态
 * @param 无
 * @return 无返回值（通过 usertrapret 进入用户态，不再返回）
 */
void forkret(void)
{
	usertrapret(thiscpu()->proc);
}
