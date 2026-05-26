#include <hal/riscv.h>
#include <hal/sbi.h>
#include <hal/timer.h>
#include <task/sched.h>
#include <misc/cputime.h>
#include <misc/log.h>

static volatile tick_t kernel_tick;
static u64 last;
static u64 interval;

inline tick_t tick() { return kernel_tick; }

void timer_init()
{
	interval = us_to_cputime(1000000ULL / TIMER_IRQ_HZ);
	last = r_time();
	sbi_set_timer(last + interval);
	kernel_tick = last / interval;
}

void handle_timer()
{
	struct cpu *c = thiscpu();
	last += interval;
	kernel_tick++;
	u64 next = last + interval;
	sbi_set_timer(last + interval);
	if(kernel_tick % 100 == 0)
		infof("tick: %zu", kernel_tick);
	c->need_resched = true;
}
