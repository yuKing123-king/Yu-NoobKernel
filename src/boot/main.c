#include <misc/log.h>
#include <misc/string.h>
#include <misc/align.h>
#include <mm/pm.h>
#include <misc/complier.h>
#include <trap/trap.h>
#include <hal/timer.h>
#include <hal/plic.h>
#include <hal/blk.h>
#include <hal/riscv.h>
#include <hal/sbi.h>
#include <mm/vm.h>
#include <mm/kalloc.h>
#include <mm/pagetable.h>
#include <mm/bcache.h>
#include <task/kthread.h>
#include <task/proc.h>
#include <task/sched.h>
#include <config.h>
#include <fs/vfs.h>
#include <fs/file.h>
#include <fs/fd_table.h>
#include <misc/errno.h>

/* External functions for filesystem support */
extern int ext4_init_fs(void);
extern struct file_system_type *get_fs(const char *name);
extern int vfs_mount_root(struct file_system_type *fs_type, dev_t dev);
extern void syscall_init(void);

extern char *s_bss;
extern char *e_bss;
extern char trampoline[];
extern int virtio_init(void);
extern pagetable_t kpagetable;
extern void forkret(void);

/*
 * 清除 BSS 段（将 BSS 段所有字节置零）
 * BSS 段存放未初始化的全局/静态变量，在进入 main 前必须清零
 */
static inline void clear_bss() { memset(s_bss, 0, e_bss - s_bss); }
volatile bool sched_enabled = false;

extern void kalloc_test(void);

/*
 * 嵌入的 init 程序二进制数据（由 ld -r -b binary 生成）
 * 编译位置：src/user/init.S → initcode.bin → initcode.o
 */
extern char _binary_z_start[];
extern char _binary_z_end[];

/* ——— Console file operations (SBI-based) ——— */

static ssize_t console_read(struct file *file, void *buf, size_t count,
			    loff_t *pos)
{
	if (!buf || count == 0)
		return -EINVAL; /* <misc/errno.h> */

	char *p = (char *)buf;
	size_t total = 0;

	for (size_t i = 0; i < count; i++) {
		int c = sbi_console_getchar(); /* <hal/sbi.h> */
		if (c < 0)
			break;
		*p++ = (char)c;
		total++;
	}

	return (ssize_t)total;
}

static ssize_t console_write(struct file *file, const void *buf, size_t count,
			     loff_t *pos)
{
	if (!buf || count == 0)
		return -EINVAL; /* <misc/errno.h> */

	const char *p = (const char *)buf;
	for (size_t i = 0; i < count; i++)
		sbi_console_putchar(p[i]); /* <hal/sbi.h> */

	return (ssize_t)count;
}

static struct file_operations console_fops = {
	.read = console_read,
	.write = console_write,
};

/**
 * @brief 创建初始用户态进程（init），加载嵌入的 init 二进制到用户空间
 * @param 无
 * @return 成功返回进程指针，失败 panic
 */
static struct proc *create_init_process(void)
{
	struct proc *p = alloc_proc();
	if (!p)
		panic("create_init_process: alloc_proc failed");

	/* 安装控制台文件到 stdin/stdout/stderr（fd 0/1/2）*/
	{
		struct file *con = file_alloc(); /* <fs/file.h> */
		if (IS_ERR(con)) /* <misc/errno.h> */
			panic("create_init_process: console file_alloc failed");
		con->f_op = &console_fops;
		con->f_mode = S_IFCHR; /* 字符设备，让 file_read/write 走 f_op 路径 */

		/* fd_install 不增加引用计数，手动 file_get 保证三个槽位各持有一个引用 */
		file_get(con); /* <fs/file.h> */
		fd_install(p->fd_table, 0, con); /* <fs/fd_table.h> */
		file_get(con);
		fd_install(p->fd_table, 1, con);
		fd_install(p->fd_table, 2, con);
	}

	p->pid = alloc_pid();
	p->state = PROC_RUNNABLE;
	strcpy(p->comm, "init");

	p->pagetable = uvmcreate();
	if (!p->pagetable)
		panic("create_init_process: uvmcreate failed");

	/* 在用户页表中映射跳板页（不带 PTE_U，用户态不可访问） */
	if (mappages(p->pagetable, TRAMPOLINE, (uintptr_t)trampoline, 1,
		     PTE_R | PTE_X) != 0)
		panic("create_init_process: map trampoline failed");



	/* 分配陷阱帧页并映射到用户页表的 TRAPFRAME 处 */
	p->tf = kzalloc(PAGE_SIZE);
	if (!p->tf)
		panic("create_init_process: alloc trapframe failed");
	if (mappages(p->pagetable, TRAPFRAME, (uintptr_t)p->tf, 1,
		     PTE_R | PTE_W) != 0)
		panic("create_init_process: map trapframe failed");

	/* 分配内核栈 */
	p->kstack = kzalloc(KSTACK_SIZE);
	if (!p->kstack)
		panic("create_init_process: alloc kstack failed");

	/* 计算 init 程序大小并分配用户内存 */
	uintptr_t init_size = (uintptr_t)_binary_z_end -
			      (uintptr_t)_binary_z_start;
	uintptr_t mem_sz = PAGE_ALIGN_UP(init_size) + PAGE_SIZE;

	uintptr_t alloc_sz = uvmalloc(p->pagetable, 0, mem_sz);
	if (alloc_sz == 0)
		panic("create_init_process: uvmalloc failed");

	/* 将 init 二进制复制到用户内存 */
	if (copyout(p->pagetable, 0, _binary_z_start, init_size) < 0)
		panic("create_init_process: copyout failed");

	/* 分配用户栈（8 页） */
	alloc_sz = uvmalloc(p->pagetable, USER_TOP - 8 * PAGE_SIZE, USER_TOP);
	if (alloc_sz == 0)
		panic("create_init_process: stack uvmalloc failed");

	/* 初始化陷阱帧 */
	p->tf->epc = 0;			/* 从虚拟地址 0 开始执行 */
	p->tf->sp = USER_TOP;		/* 用户栈指针指向栈顶 */
	p->tf->kernel_satp = MAKE_SATP(kpagetable);
	p->tf->kernel_sp = (uintptr_t)p->kstack + KSTACK_SIZE;
	p->tf->kernel_trap = (uintptr_t)usertrap;
	p->tf->kernel_hartid = r_tp();

	/* 设置上下文：首次调度时通过 forkret 进入用户态 */
	p->ctx.ra = (uintptr_t)forkret;
	p->ctx.sp = (uintptr_t)p->kstack + KSTACK_SIZE;
	p->ctx.sstatus = 0;	/* 初次进入不启用中断 */

	return p;
}

/*
 * 内核主入口函数
 * 仅 hart 0 执行初始化：清除 BSS、初始化 CPU、内存管理、中断、定时器、
 * 虚拟内存、调度器、块设备和 virtio；完成后创建 init 进程并切换到用户态
 * @param hartid: 当前硬件线程（HART）ID
 * @param _: 保留参数（未使用）
 */
void main(u64 hartid, void *_)
{
	if (hartid == 0) {
		clear_bss();
		init_cpu(hartid);
		print_pm_layout();
		if (pm_init())
			panic("pm_init failed");
		plic_init();
		trap_init();
		timer_init();
		kvminit();
		init_runq();
		blk_init();
		bcache_init();
		virtio_init();

		/* Initialize syscall table */
		syscall_init();

		/* Initialize filesystem layer */
		vfs_init();
		ext4_init_fs();

		/* Mount EXT4 root filesystem */
		{
			dev_t root_dev = MKDEV(BLK_MAJOR_VIRTIO, 0);
			if (vfs_mount_root(get_fs("ext4"), root_dev) != 0)
				warnf("ext4: failed to mount root");
		}

		struct proc *init_proc = create_init_process();
		enqueue_proc(r_tp(), init_proc);

		sched_enabled = true;
		context_switch_to(&thiscpu()->idle.ctx);
		sched_yield();
	}
	while (1) {
		wfi();
	}
}
