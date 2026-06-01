#include <config.h>
#include <hal/plic.h>
#include <hal/device_types.h>
#include <misc/bitmap.h>
#include <misc/log.h>

#define PLIC_MAX_IRQ_NUM 1024
#define PLIC_MAX_CTX_NUM 15872

#define PLIC_CTX(hart, mode) ((hart) * 2 + ((mode) == 'S' ? 1 : 0))

struct plic_regs {
	volatile u32 priority[PLIC_MAX_IRQ_NUM];
	volatile u32 pending[PLIC_MAX_IRQ_NUM / 32];
	u8 reserved1[0xF80];
	volatile u32 enable[PLIC_MAX_CTX_NUM][PLIC_MAX_IRQ_NUM / 32];
	u8 _reserved2[0xE000];
	struct {
		volatile u32 priority_threshold;
		volatile u32 claim_complete;
		u8 _reserved[0xFF8];
	} context_status[PLIC_MAX_CTX_NUM];
} __attribute__((__packed__));

struct plic_device {
	struct plic_raw raw;
	struct plic_regs * regs;
};

static struct plic_device plic = {
	.raw = PLIC
};

/*
 * 初始化PLIC设备，将PLIC寄存器基地址映射到内存中
 */
void plic_init(){
	plic.regs = (void *)plic.raw.addr;
}

/*
 * 设置指定中断源的优先级
 * @param irqno: 中断号（必须大于0且小于PLIC_MAX_IRQ_NUM）
 * @param priority: 优先级值
 */
void plic_set_priority(u32 irqno, u32 priority)
{
	assert(irqno > 0 && irqno < PLIC_MAX_IRQ_NUM);
	plic.regs->priority[irqno] = priority;
}

/*
 * 使能指定中断在指定HART上下文中的传递
 * @param mode: 特权模式（'S'表示监督模式，'M'表示机器模式）
 * @param hartid: HART编号
 * @param irqno: 中断号
 */
void plic_enable(char mode, u64 hartid, u32 irqno)
{
	assert(mode == 'S' || mode == 'M');
	assert(hartid < CPU_NUM);
	assert(irqno > 0 && irqno < PLIC_MAX_IRQ_NUM);
	struct bitmap irq_enabled = {
	    .data = (u8 *)plic.regs->enable[PLIC_CTX(hartid, mode)],
	    .len = PLIC_MAX_IRQ_NUM};
	bitmap_set(&irq_enabled, irqno);
}

/*
 * 禁用指定中断在指定HART上下文中的传递
 * @param mode: 特权模式（'S'表示监督模式，'M'表示机器模式）
 * @param hartid: HART编号
 * @param irqno: 中断号
 */
void plic_disable(char mode, u64 hartid, u32 irqno)
{
	assert(mode == 'S' || mode == 'M');
	assert(hartid < CPU_NUM);
	assert(irqno > 0 && irqno < PLIC_MAX_IRQ_NUM);
	struct bitmap irq_enabled = {
	    .data = (u8 *)plic.regs->enable[PLIC_CTX(hartid, mode)],
	    .len = PLIC_MAX_IRQ_NUM};
	bitmap_clear(&irq_enabled, irqno);
}

/*
 * 设置指定HART上下文的优先级阈值，优先级低于阈值的中断将被屏蔽
 * @param mode: 特权模式（'S'表示监督模式，'M'表示机器模式）
 * @param hartid: HART编号
 * @param threshold: 阈值
 */
void plic_set_threshold(char mode, u64 hartid, u32 threshold)
{
	assert(mode == 'S' || mode == 'M');
	assert(hartid < CPU_NUM);
	plic.regs->context_status[PLIC_CTX(hartid, mode)].priority_threshold =
	    threshold;
}

/*
 * 获取当前最高优先级的中断号（Claim操作）
 * @param mode: 特权模式（'S'表示监督模式，'M'表示机器模式）
 * @param hartid: HART编号
 * @return: 中断号，若无待处理中断则返回0
 */
u32 plic_claim(char mode, u64 hartid)
{
	assert(mode == 'S' || mode == 'M');
	assert(hartid < CPU_NUM);
	return plic.regs->context_status[PLIC_CTX(hartid, mode)].claim_complete;
}

/*
 * 完成中断处理（Complete操作），通知PLIC中断已处理完毕
 * @param mode: 特权模式（'S'表示监督模式，'M'表示机器模式）
 * @param hartid: HART编号
 * @param irqno: 中断号
 */
void plic_complete(char mode, u64 hartid, u32 irqno)
{
	assert(mode == 'S' || mode == 'M');
	assert(hartid < CPU_NUM);
	assert(irqno > 0 && irqno < PLIC_MAX_IRQ_NUM);
	plic.regs->context_status[PLIC_CTX(hartid, mode)].claim_complete =
	    irqno;
}
