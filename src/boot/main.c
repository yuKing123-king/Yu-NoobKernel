#include <misc/log.h>
#include <misc/string.h>
#include <mm/pm.h>
#include <misc/complier.h>
#include <trap/trap.h>
#include <hal/timer.h>
#include <hal/plic.h>
#include <hal/blk.h>
#include <mm/vm.h>
#include <mm/kalloc.h>
#include <mm/bcache.h>
#include <task/kthread.h>
#include <task/sched.h>

extern char *s_bss;
extern char *e_bss;
extern int virtio_init(void);
extern void virtio_disk_test(void);

static inline void clear_bss() { memset(s_bss, 0, e_bss - s_bss); }
volatile bool sched_enabled = false;

extern void kalloc_test(void);

void main(u64 hartid, void *_)
{
	if (hartid == 0) {
		clear_bss();
		init_cpu(hartid);
		infof("Hello world on cpu: %d", r_tp());
		print_pm_layout();
		pm_init();
		plic_init();
		trap_init();
		timer_init();
		kvminit();
		init_runq();
		blk_init();
		bcache_init();
		virtio_init();
		sched_enabled = true;
		context_switch_to(&thiscpu()->idle.ctx);
		sched_yield();
	}
	while (1) {
		wfi();
	}
}
