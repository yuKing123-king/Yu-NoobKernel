#include <misc/stdint.h>
#include <misc/printf.h>
#include <misc/log.h>
#include <hal/riscv.h>

extern char skernel[];
extern char ekernel[];

static inline u64 read_s0(void)
{
	u64 v;
	asm volatile("mv %0, s0" : "=r"(v));
	return v;
}

static inline u64 read_ra(void)
{
	u64 v;
	asm volatile("mv %0, ra" : "=r"(v));
	return v;
}

static inline u64 read_sp(void)
{
	u64 v;
	asm volatile("mv %0, sp" : "=r"(v));
	return v;
}

/*
 * RISC-V 栈回溯：顺着 s0(fp) 链向上走，每一帧：
 *   [s0 + 0]  = 上一帧 s0
 *   [s0 + 8]  = 返回地址 ra
 * 配合 -fno-omit-frame-pointer 编译选项即可拿到准确调用链。
 */
void backtrace_print(void)
{
	u64 ra = read_ra();
	u64 s0 = read_s0();
	u64 sp_val = read_sp();

	infof("--- backtrace start ---");
	infof("  [%016lx] sp=%p (kfree+0x%lx)",
	      ra, (void *)sp_val, ra - (u64)skernel);

	int i = 0;
	while (i++ < 16) {
		if (s0 == 0)
			break;
		if ((u64)s0 < (u64)skernel || (u64)s0 > (u64)ekernel)
			break;

		u64 prev_s0, frame_ra;
		asm volatile("ld %0, 0(%1)" : "=r"(prev_s0) : "r"((u64)s0));
		asm volatile("ld %0, 8(%1)" : "=r"(frame_ra) : "r"((u64)s0));

		infof("  [%016lx] s0=%p (kfree+0x%lx)",
		      frame_ra, (void *)prev_s0,
		      frame_ra - (u64)skernel);

		if (prev_s0 == s0)
			break;
		s0 = prev_s0;
	}
	infof("--- backtrace end ---");
}
