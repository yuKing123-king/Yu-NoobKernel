#include <hal/riscv.h>
#include <hal/sbi.h>
#include <hal/timer.h>
#include <task/sched.h>
#include <misc/cputime.h>
#include <misc/log.h>

static volatile tick_t kernel_tick;
static u64 last;
static u64 interval;

/*
 * 获取当前系统滴答数
 * @return: 系统启动以来的滴答计数值
 */
inline tick_t tick() { return kernel_tick; }

/*
 * 初始化定时器，设置定时器中断间隔并触发第一次定时器中断
 */
void timer_init()
{
	interval = us_to_cputime(1000000ULL / TIMER_IRQ_HZ);
	last = r_time();
	sbi_set_timer(last + interval);
	kernel_tick = last / interval;
}

/*
 * 定时器中断处理函数，更新滴答计数并设置下一次定时器中断
 * 每100个滴答输出一次日志，同时标记当前CPU需要调度
 */
void handle_timer()
{
	struct cpu *c = thiscpu();
	last += interval;
	kernel_tick++;
	u64 next = last + interval;
	sbi_set_timer(last + interval);
	c->need_resched = true;
}
