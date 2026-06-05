#include <misc/log.h>
#include <misc/string.h>
#include <misc/complier.h>
#include <mm/vm.h>
#include <mm/pagetable.h>
#include <mm/kalloc.h>
#include <mm/pm.h>
#include <task/sched.h>
#include <task/proc.h>
#include <trap/trap.h>
#include <hal/sbi.h>
#include <syscall/syscall_nr.h>
#include <syscall/syscall.h>
#include <syscall/syscall_impl.h>
#include <fs/vfs.h>
#include <fs/fd_table.h>
#include <fs/file.h>
#include <ipc/pipe.h>

#include <misc/errno.h>
#include <misc/align.h>
#include <hal/riscv.h>
#include <misc/cputime.h>
#include <hal/timer.h>
#include <hal/blk.h>

extern pagetable_t kpagetable;

#define AT_REMOVEDIR 0x200

#define PROT_READ	1
#define PROT_WRITE	2
#define PROT_EXEC	4
extern char trampoline[];

/* ============================================================
 * Syscall 注册表（稀疏散列，避免 512 项大数组）
 * 使用开放寻址哈希表，容量 64，负载因子 < 0.5
 * ============================================================ */

#define SC_TABLE_BITS 6
#define SC_TABLE_SIZE (1 << SC_TABLE_BITS)
#define SC_TABLE_MASK (SC_TABLE_SIZE - 1)

static struct {
	int nr;
	syscall_fn_t fn;
} sc_table[SC_TABLE_SIZE];

static inline int sc_hash(int nr) { return nr & SC_TABLE_MASK; }

void syscall_register(int nr, syscall_fn_t fn)
{
	int idx = sc_hash(nr);
	for (int i = 0; i < SC_TABLE_SIZE; i++) {
		int slot = (idx + i) & SC_TABLE_MASK;
		if (sc_table[slot].fn == NULL) {
			sc_table[slot].nr = nr;
			sc_table[slot].fn = fn;
			return;
		}
	}
	panic("syscall_register: table full");
}

syscall_fn_t syscall_lookup(int nr)
{
	int idx = sc_hash(nr);
	for (int i = 0; i < SC_TABLE_SIZE; i++) {
		int slot = (idx + i) & SC_TABLE_MASK;
		if (sc_table[slot].fn == NULL)
			return NULL;
		if (sc_table[slot].nr == nr)
			return sc_table[slot].fn;
	}
	return NULL;
}

/* ============================================================
 * 辅助函数
 * ============================================================ */

static inline struct proc *curproc(void) { return thiscpu()->proc; }

/* ============================================================
 * I/O: read / write
 * ============================================================ */

uintptr_t sys_write(int fd, uintptr_t buf, size_t len)
{
	char tmp[512];
	size_t n = len < sizeof(tmp) ? len : sizeof(tmp);
	struct proc *p = curproc();

	if (fd == 1 || fd == 2) {
		/* stdout/stderr → SBI console */
		if (copyin(p->pagetable, tmp, buf, n) < 0)
			return -EFAULT;
		for (size_t i = 0; i < n; i++)
			sbi_console_putchar(tmp[i]);
		return n;
	}

	/* file fd */
	struct file *f = fd_get(p->fd_table, fd);
	if (!f)
		return -EBADF;

	if (copyin(p->pagetable, tmp, buf, n) < 0) {
		file_put(f);
		return -EFAULT;
	}

	ssize_t ret = file_write(f, tmp, n);
	file_put(f);
	return ret < 0 ? ret : n;
}

uintptr_t sys_read(int fd, uintptr_t buf, size_t len)
{
	struct proc *p = curproc();

	if (len == 0)
		return 0;

	struct file *f = fd_get(p->fd_table, fd);
	if (!f)
		return -EBADF;

	char tmp[512];
	size_t n = len < sizeof(tmp) ? len : sizeof(tmp);
	ssize_t ret = file_read(f, tmp, n);
	if (ret > 0) {
		if (copyout(p->pagetable, buf, tmp, ret) < 0) {
			file_put(f);
			return -EFAULT;
		}
	}

	file_put(f);
	return ret;
}

/* ============================================================
 * Exit / Shutdown / Reboot
 * ============================================================ */

uintptr_t sys_exit(int status)
{
	struct proc *p = curproc();
	p->exit_code = status;
	p->state = PROC_ZOMBIE;

	/* set_tid_address: 写 0 到 clear_child_tid，唤醒 futex 等待者 */
	if (p->clear_child_tid) {
		int zero = 0;
		copyout(p->pagetable, (uintptr_t)p->clear_child_tid, (char *)&zero,
			sizeof(zero));
	}

	if (p->parent)
		wait_queue_wakeup_one(&p->parent->child_wait);
	sched_yield();
	panic("unreachable");
	return 0;
}

uintptr_t sys_exit_group(int status) { return sys_exit(status); }

uintptr_t sys_set_tid_address(uintptr_t tidptr)
{
	struct proc *p = curproc();
	p->clear_child_tid = (int *)tidptr;
	return p->pid;
}

uintptr_t sys_getrandom(uintptr_t buf, size_t len, unsigned int flags)
{
	struct proc *p = curproc();
	(void)flags;

	char tmp[256];
	size_t n = len < sizeof(tmp) ? len : sizeof(tmp);

	/* 简单伪随机：用 time + pid 混合 */
	u64 seed = r_time() ^ ((u64)p->pid << 32);
	for (size_t i = 0; i < n; i++) {
		seed = seed * 1103515245 + 12345;
		tmp[i] = (char)(seed >> 16);
	}

	if (copyout(p->pagetable, buf, tmp, n) < 0)
		return -EFAULT;
	return n;
}

uintptr_t sys_shutdown(void)
{
	infof("Shutdown requested");
	sbi_shutdown();
	panic("unreachable");
	return 0;
}

uintptr_t sys_reboot(void)
{
	infof("Reboot requested");
	sbi_reboot();
	panic("unreachable");
	return 0;
}

/* ============================================================
 * File: open / close / getdents
 * ============================================================ */

uintptr_t sys_openat(uintptr_t dirfd, uintptr_t pathname, uintptr_t flags, uintptr_t mode)
{
	struct proc *p = curproc();
	char path[256];
	if (copyinstr(p->pagetable, path, pathname, sizeof(path)) < 0)
		return -EFAULT;

	/* 解析 base dentry: 绝对路径用 root，否则看 dirfd */
	struct dentry *base = NULL;
	if (path[0] == '/') {
		base = NULL;
	} else if ((long)dirfd == AT_FDCWD) {
		base = p->pwd;
	} else {
		struct file *dir_file = fd_get(p->fd_table, (int)dirfd);
		if (dir_file && dir_file->f_dentry)
			base = dir_file->f_dentry;
		else
			base = p->pwd;
	}
	struct file *f = vfs_open_cwd(path, (u32)flags, base);
	if (IS_ERR(f)) {
		return (uintptr_t)PTR_ERR(f);
	}

	int fd = fd_alloc(p->fd_table);
	if (fd < 0) {
		vfs_close(f);
		return -EMFILE;
	}

	if (fd_install(p->fd_table, fd, f) < 0)
		return -EBADF;

	return fd;
}

uintptr_t sys_close(uintptr_t fd)
{
	struct proc *p = curproc();
	fd_free(p->fd_table, (int)fd);
	return 0;
}

uintptr_t sys_getdents(uintptr_t fd, uintptr_t buf, uintptr_t count)
{
	struct proc *p = curproc();
	struct file *f = fd_get(p->fd_table, (int)fd);
	if (!f)
		return -EBADF;

	struct dirent *kd = kzalloc(count < 4096 ? count : 4096);
	if (!kd) {
		file_put(f);
		return -ENOMEM;
	}

	int kcount = count < 4096 ? count : 4096;
	int ret = file_getdents(f, kd, kcount);
	if (ret <= 0) {
		file_put(f);
		kfree(kd);
		return ret;
	}

	/* 将内核固定大小 dirent 转换为 Linux ABI linux_dirent64 格式
	 * linux_dirent64: d_ino(8) + d_off(8) + d_reclen(2) + d_type(1) + d_name[]
	 * 每条记录 d_reclen = 19 + strlen(name) + 1, 8字节对齐 */
	int nents = ret / sizeof(struct dirent);
	char *out = (char *)kzalloc(count);
	if (!out) {
		file_put(f);
		kfree(kd);
		return -ENOMEM;
	}

	int written = 0;
	for (int i = 0; i < nents; i++) {
		struct dirent *de = &kd[i];
		int nlen = 0;
		while (de->d_name[nlen]) nlen++;
		int reclen = (19 + nlen + 1 + 7) & ~7;

		if (written + reclen > count) break;

		/* 填充 linux_dirent64 */
		*(u64 *)(out + written + 0)  = de->d_ino;
		*(u64 *)(out + written + 8)  = de->d_off;
		*(u16 *)(out + written + 16) = (u16)reclen;
		/* 映射 ext4 file_type → Linux d_type */
			u8 ltype;
			switch (de->d_type) {
			case 1: ltype = 8;  break; /* EXT4_FT_REG_FILE → DT_REG */
			case 2: ltype = 4;  break; /* EXT4_FT_DIR     → DT_DIR */
			case 7: ltype = 10; break; /* EXT4_FT_SYMLINK → DT_LNK */
			default: ltype = 0; break;
			}
			*(u8  *)(out + written + 18) = ltype;
		for (int j = 0; j < nlen; j++)
			out[written + 19 + j] = de->d_name[j];
		out[written + 19 + nlen] = '\0';

		written += reclen;
	}

	if (written > 0) {
		if (copyout(p->pagetable, buf, out, written) < 0) {
			file_put(f);
			kfree(kd);
			kfree(out);
			return -EFAULT;
		}
	}

	file_put(f);
	kfree(kd);
	kfree(out);
	return written;
}

/* ============================================================
 * Process: fork / execve / wait4 / getpid
 * ============================================================ */

uintptr_t sys_fork(void)
{
	struct proc *parent = curproc();
	struct proc *child = alloc_proc();
	if (!child)
		return -ENOMEM;

	child->pid = alloc_pid();
	child->tgid = child->pid;
	child->parent = parent;

	/* 复制用户页表 */
	child->pagetable = uvmcreate();
	if (!child->pagetable)
		goto fail;

	/* 复制用户页表（树遍历，O(已映射页数)） */
	if (uvmcopy_tree(parent->pagetable, child->pagetable) < 0) {
		goto fail_freept;
	}

	/* 释放 alloc_proc 分配的空 fd_table，用 dup 的替换 */
	fd_table_free(child->fd_table);
	child->fd_table = fd_table_dup(parent->fd_table);
	if (!child->fd_table)
		goto fail_freept;

	/* 分配内核栈 */
	child->kstack = kzalloc(KSTACK_SIZE);
	if (!child->kstack)
		goto fail_freetable;

	/* 分配 trapframe 并复制 */
	child->tf = kzalloc(PAGE_SIZE);
	if (!child->tf)
		goto fail_freestack;

	memcpy(child->tf, parent->tf, sizeof(struct trapframe));
	child->tf->a0 = 0; /* 子进程返回 0 */

	/* 映射 trampoline 和 trapframe 到子进程页表 */
	if (mappages(child->pagetable, TRAMPOLINE, (uintptr_t)trampoline, 1,
		     PTE_R | PTE_X) != 0)
		goto fail_freetf;

	if (mappages(child->pagetable, TRAPFRAME, (uintptr_t)child->tf, 1,
		     PTE_R | PTE_W) != 0)
		goto fail_freetf;

	/* 复制 pwd dentry */
	if (parent->pwd) {
		child->pwd = parent->pwd;
		dentry_get(child->pwd);
	}

	/* 继承 brk */
	child->brk_end = parent->brk_end;

	/* 设置上下文：forkret → usertrapret → 用户态 */
	child->ctx.ra = (uintptr_t)forkret;
	child->ctx.sp = (uintptr_t)child->kstack + KSTACK_SIZE;
	child->ctx.sstatus = 0;	/* 首次进入不启用中断，由 usertrapret 控制 */

	/* 加入父进程 children 列表 */
	spinlock_acquire(&parent->lock);
	list_add_tail(&child->sibling, &parent->children);
	spinlock_release(&parent->lock);

	/* 更新 trapframe 中的内核指针 */
	child->tf->kernel_satp = MAKE_SATP(kpagetable);
	child->tf->kernel_sp = (uintptr_t)child->kstack + KSTACK_SIZE;
	child->tf->kernel_trap = (uintptr_t)usertrap;
	child->tf->kernel_hartid = r_tp();

	child->state = PROC_RUNNABLE;
	enqueue_proc(r_tp(), child);
	return child->pid;

fail_freetf:
	kfree(child->tf);
fail_freestack:
	kfree(child->kstack);
fail_freetable:
	fd_table_free(child->fd_table);
fail_freept:
	pagetable_destroy(child->pagetable);
fail:
	kfree(child);
	return -ENOMEM;
}

/*
 * sys_clone — 创建子进程（支持 CLONE_VM 共享页表 + 指定子进程栈）
 * RISC-V ABI: clone(flags, child_stack, ptid, ctid, tls)
 * 当 CLONE_VM 置位时，父子共享页表（线程风格）
 * child_stack 指定子进程的初始 sp（不为 0 时使用，否则继承父进程 sp）
 */
uintptr_t sys_clone(uintptr_t flags, uintptr_t child_stack, uintptr_t ptid,
		    uintptr_t ctid, uintptr_t tls)
{
	struct proc *parent = curproc();
	struct proc *child = alloc_proc();
	if (!child)
		return -ENOMEM;

	child->pid = alloc_pid();
	child->tgid = child->pid;
	child->parent = parent;

	/*
	 * CLONE_VM: Linux 语义应共享页表，但共享页表意味着 TRAPFRAME 也需要
	 * 在上下文切换时重新映射（两个线程共用页表但各有自己的 trapframe）。
	 * 当前调度器不支持动态更新 TRAPFRAME 映射，因此这里统一复制页表。
	 * 子进程仍能通过 child_stack 获得独立的栈。
	 */
	child->pagetable = uvmcreate();
	if (!child->pagetable)
		goto fail;

	if (uvmcopy_tree(parent->pagetable, child->pagetable) < 0) {
		goto fail_freept;
	}

	/*
	 * fd_table: CLONE_FILES 置位时共享，否则复制 (dup)
	 * fork (flags=0) 走复制分支，clone with CLONE_FILES 走共享分支
	 */
	fd_table_free(child->fd_table);
	if (flags & CLONE_FILES) {
		child->fd_table = parent->fd_table;
	} else {
		child->fd_table = fd_table_dup(parent->fd_table);
	}
	if (!child->fd_table)
		goto fail_freept;

	/* 分配内核栈 */
	child->kstack = kzalloc(KSTACK_SIZE);
	if (!child->kstack)
		goto fail_freetable;

	/* 分配 trapframe 并复制 */
	child->tf = kzalloc(PAGE_SIZE);
	if (!child->tf)
		goto fail_freestack;

	memcpy(child->tf, parent->tf, sizeof(struct trapframe));

	/* 子进程返回 0 */
	child->tf->a0 = 0;

	/*
	 * fork 时继承父进程 sp，clone 时使用指定的 child_stack。
	 * 仅当 child_stack 是合法用户态地址时才设为子进程栈：
	 * 要求 >= PAGE_SIZE（过滤 fd 号等小值垃圾）且 < USER_TOP。
	 * 防止用户态 fork 调用者未清零 a1 传入的残留值被误用。
	 */
	if (child_stack >= PAGE_SIZE && child_stack < USER_TOP) {
		child->tf->sp = child_stack;
	}

	/* 映射 trampoline 和 trapframe */
	if (mappages(child->pagetable, TRAMPOLINE, (uintptr_t)trampoline, 1,
		     PTE_R | PTE_X) != 0)
		goto fail_freetf;

	if (mappages(child->pagetable, TRAPFRAME, (uintptr_t)child->tf, 1,
		     PTE_R | PTE_W) != 0)
		goto fail_freetf;

	/* 复制 pwd */
	if (parent->pwd) {
		child->pwd = parent->pwd;
		dentry_get(child->pwd);
	}

	/* 继承 brk */
	child->brk_end = parent->brk_end;

	/* 设置上下文：forkret → usertrapret → 用户态 */
	child->ctx.ra = (uintptr_t)forkret;
	child->ctx.sp = (uintptr_t)child->kstack + KSTACK_SIZE;
	child->ctx.sstatus = 0;

	/* 加入父进程 children 列表 */
	spinlock_acquire(&parent->lock);
	list_add_tail(&child->sibling, &parent->children);
	spinlock_release(&parent->lock);

	/* 更新 trapframe 中的内核指针 */
	child->tf->kernel_satp = MAKE_SATP(kpagetable);
	child->tf->kernel_sp = (uintptr_t)child->kstack + KSTACK_SIZE;
	child->tf->kernel_trap = (uintptr_t)usertrap;
	child->tf->kernel_hartid = r_tp();

	child->state = PROC_RUNNABLE;
	enqueue_proc(r_tp(), child);
	return child->pid;

fail_freetf:
	kfree(child->tf);
fail_freestack:
	kfree(child->kstack);
fail_freetable:
	fd_table_free(child->fd_table);
fail_freept:
	pagetable_destroy(child->pagetable);
fail:
	kfree(child);
	return -ENOMEM;
}

/* 简单 ELF64 头部结构 */
typedef struct {
	u8 e_ident[16];
	u16 e_type;
	u16 e_machine;
	u32 e_version;
	u64 e_entry;
	u64 e_phoff;
	u64 e_shoff;
	u32 e_flags;
	u16 e_ehsize;
	u16 e_phentsize;
	u16 e_phnum;
	u16 e_shentsize;
	u16 e_shnum;
	u16 e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
	u32 p_type;
	u32 p_flags;
	u64 p_offset;
	u64 p_vaddr;
	u64 p_paddr;
	u64 p_filesz;
	u64 p_memsz;
	u64 p_align;
} __attribute__((packed)) elf64_phdr_t;

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

uintptr_t sys_execve(uintptr_t filename, uintptr_t argv, uintptr_t envp)
{
	struct proc *p = curproc();
	char path[256];

	if (copyinstr(p->pagetable, path, filename, sizeof(path)) < 0)
		return -EFAULT;

	/* 绝对路径用 root，相对路径用 CWD */
	struct file *f = vfs_open_cwd(path, O_RDONLY, path[0] == '/' ? NULL : p->pwd);
	if (IS_ERR(f))
		return (uintptr_t)PTR_ERR(f);

	/* 读取 ELF header */
	elf64_ehdr_t ehdr;
	loff_t pos = 0;
	ssize_t n = file_read(f, &ehdr, sizeof(ehdr));

	/* 校验 ELF magic */
	if (n < (ssize_t)sizeof(ehdr) ||
	    ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' ||
	    ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
		file_put(f);
		return -ENOEXEC;
	}

	/* 校验架构：RISC-V = 0xF3 */
	if (ehdr.e_machine != 0xF3) {
		file_put(f);
		return -ENOEXEC;
	}

	/* 释放旧的用户空间映射（保留 trampoline 和 trapframe）
	 * 页表树遍历替代线性扫描，复杂度 O(已映射页数) */
	uvm_free_user_pages(p->pagetable);
	sfence_vma();

	p->brk_end = 0;

	/* 加载 PT_LOAD segments */
	uintptr_t max_va = 0;
	for (int i = 0; i < ehdr.e_phnum; i++) {
		elf64_phdr_t phdr;
		pos = ehdr.e_phoff + i * ehdr.e_phentsize;
		file_lseek(f, pos, SEEK_SET);

		if (file_read(f, &phdr, sizeof(phdr)) < (ssize_t)sizeof(phdr))
			continue;

		if (phdr.p_type != PT_LOAD)
			continue;

		uintptr_t va = PAGE_ALIGN_DOWN(phdr.p_vaddr);
		uintptr_t end = PAGE_ALIGN_UP(phdr.p_vaddr + phdr.p_memsz);

		/* Validate: must not overlap TRAMPOLINE or TRAPFRAME */
		if (end > USER_TOP - PAGE_SIZE ||
		    (end > TRAPFRAME && va < TRAPFRAME + PAGE_SIZE)) {
			file_put(f);
			return -EINVAL;
		}

		if (end > max_va)
			max_va = end;

		/* 分配物理页并映射 */
		for (uintptr_t a = va; a < end; a += PAGE_SIZE) {
			uintptr_t pa = (uintptr_t)kzalloc(PAGE_SIZE);
			if (!pa)
				goto exec_fail;

			int perm = PTE_U | PTE_R | PTE_W | PTE_X;

			if (mappages(p->pagetable, a, pa, 1, perm) != 0) {
				kfree((void *)pa);
				goto exec_fail;
			}
		}

		/* 从文件读取 segment 内容 */
		if (phdr.p_filesz > 0) {
			u64 off = 0;
			u64 remaining = phdr.p_filesz;
			uintptr_t dst = phdr.p_vaddr;

			file_lseek(f, phdr.p_offset, SEEK_SET);

			while (remaining > 0) {
				char tmp[512];
				u64 chunk = remaining < sizeof(tmp) ? remaining : sizeof(tmp);
				ssize_t rd = file_read(f, tmp, chunk);
				if (rd <= 0)
					break;
				if (copyout(p->pagetable, dst + off, tmp, rd) < 0)
					goto exec_fail;
				off += rd;
				remaining -= rd;
			}
		}
	}

	file_put(f);

	if (max_va == 0) {
		return -ENOEXEC;
	}

	/* 设置 brk_end */
	p->brk_end = max_va;

	/* 设置用户栈 */
	uintptr_t stack_bot = USER_TOP - PAGE_SIZE;
	if (walkaddr(p->pagetable, stack_bot) == 0) {
		uintptr_t pa = (uintptr_t)kzalloc(PAGE_SIZE);
		if (!pa) {
			warnf("sys_execve: failed to alloc stack");
			goto exec_fail;
		}
		if (mappages(p->pagetable, stack_bot, pa, 1,
			     PTE_R | PTE_W | PTE_U) != 0) {
			kfree((void *)pa);
			goto exec_fail;
		}
	}

	/* 设置 trapframe */
	p->tf->epc = ehdr.e_entry;
	p->tf->sp = USER_TOP - 16;
	/* 传递 argc (简化：暂不传 argv) */
	p->tf->a0 = 0;

	return 0;

exec_fail:
	file_put(f);
	return -ENOMEM;
}

uintptr_t sys_wait4(uintptr_t pid_arg, uintptr_t wstatus, uintptr_t options, uintptr_t rusage)
{
	struct proc *p = curproc();
	(void)options;
	(void)rusage;

	while (1) {
		int found_child = 0;

		spinlock_acquire(&p->lock);

		struct list_head *pos;
		list_for_each(pos, &p->children) {
			struct proc *child = list_entry(pos, struct proc, sibling);
			int target = (int)pid_arg;

			if (target == -1 || target == child->pid) {
				if (child->state == PROC_ZOMBIE) {
					if (wstatus != 0) {
						int status = (child->exit_code & 0xff) << 8;
						if (copyout(p->pagetable, wstatus, (char *)&status,
							    sizeof(status)) < 0) {
							spinlock_release(&p->lock);
							return -EFAULT;
						}
					}

					pid_t cpid = child->pid;
					list_del(&child->sibling);
					spinlock_release(&p->lock);
					free_proc(child);

					return cpid;
				}
				found_child = 1;
			}
		}

		spinlock_release(&p->lock);

		if (found_child) {
			wait_queue_sleep(&p->child_wait, p);
		} else {
			return -ECHILD;
		}
	}
}

uintptr_t sys_getpid(void)
{
	return curproc()->pid;
}

uintptr_t sys_getppid(void)
{
	struct proc *p = curproc();
	return p->parent ? p->parent->pid : 0;
}

/* ============================================================
 * Memory: brk / mmap / munmap / mprotect
 * ============================================================ */

uintptr_t sys_brk(uintptr_t addr)
{
	struct proc *p = curproc();

	if (addr == 0) {
		/* 首次调用：如果 brk_end 为 0，返回一个合理的默认起始值 */
		if (p->brk_end == 0)
			return (1ULL << 20); /* 默认起始 1MB */
		return p->brk_end;
	}

	/* 首次设置 brk，从默认起始地址开始 */
	if (p->brk_end == 0)
		p->brk_end = (1ULL << 20);

	if (addr < p->brk_end) {
		/* 缩小堆 */
		uintptr_t old = PAGE_ALIGN_UP(p->brk_end);
		p->brk_end = addr;
		uintptr_t new = PAGE_ALIGN_UP(addr);
		/* 释放多余页 */
		for (uintptr_t a = new; a < old; a += PAGE_SIZE) {
			uintptr_t pa = walkaddr(p->pagetable, a);
			if (pa) {
				unmappages(p->pagetable, a, 1);
				kfree((void *)pa);
			}
		}
		return p->brk_end;
	}

	if (addr > p->brk_end) {
		/* 扩大堆 */
		uintptr_t old = PAGE_ALIGN_UP(p->brk_end);
		uintptr_t new_end = PAGE_ALIGN_UP(addr);
		for (uintptr_t a = old; a < new_end; a += PAGE_SIZE) {
			if (walkaddr(p->pagetable, a) != 0)
				continue;
			uintptr_t pa = (uintptr_t)kzalloc(PAGE_SIZE);
			if (!pa)
				return p->brk_end;
			if (mappages(p->pagetable, a, pa, 1,
				     PTE_R | PTE_W | PTE_U) != 0) {
				kfree((void *)pa);
				return p->brk_end;
			}
		}
		p->brk_end = addr;
	}

	return p->brk_end;
}

uintptr_t sys_mmap(uintptr_t addr, size_t len, int prot, int flags, int fd, loff_t offset)
{
	struct proc *p = curproc();

	if (len == 0)
		goto fail;

	int pte_flags = PTE_U;
	if (prot & PROT_READ) pte_flags |= PTE_R;
	if (prot & PROT_WRITE) pte_flags |= PTE_W;
	if (prot & PROT_EXEC) pte_flags |= PTE_X;
	if (pte_flags == PTE_U)
		pte_flags = PTE_R | PTE_W | PTE_U;

	size_t sz = PAGE_ALIGN_UP(len);
	uintptr_t start;

	if (flags & 0x20 /* MAP_FIXED */) {
		start = addr;
	} else {
		start = PAGE_ALIGN_UP(p->brk_end ? p->brk_end : 0x10000);
	}

	/* File-backed mmap */
	if (fd >= 0) {
		struct file *f = fd_get(p->fd_table, fd);
		if (!f)
			goto fail;

		loff_t saved_pos = f->f_pos;
		f->f_pos = offset;

		for (uintptr_t a = start; a < start + sz; a += PAGE_SIZE) {
			if (walkaddr(p->pagetable, a) != 0)
				continue;
			uintptr_t pa = (uintptr_t)kzalloc(PAGE_SIZE);
			if (!pa) {
				f->f_pos = saved_pos;
				file_put(f);
				return -ENOMEM;
			}
			if (mappages(p->pagetable, a, pa, 1, pte_flags) != 0) {
				kfree((void *)pa);
				f->f_pos = saved_pos;
				file_put(f);
				return -ENOMEM;
			}
			size_t to_read = PAGE_SIZE;
			size_t remain = len - (a - start);
			if (to_read > remain)
				to_read = remain;
			if (to_read > 0)
				file_read(f, (void *)pa, to_read);
		}

		f->f_pos = saved_pos;
		file_put(f);

		if (start + sz > p->brk_end)
			p->brk_end = start + sz;
		return start;
	}

	/* Anonymous mmap */
	for (uintptr_t a = start; a < start + sz; a += PAGE_SIZE) {
		if (walkaddr(p->pagetable, a) != 0)
			continue;
		uintptr_t pa = (uintptr_t)kzalloc(PAGE_SIZE);
		if (!pa)
			return -ENOMEM;
		if (mappages(p->pagetable, a, pa, 1, pte_flags) != 0) {
			kfree((void *)pa);
			return -ENOMEM;
		}
	}

	if (start + sz > p->brk_end)
		p->brk_end = start + sz;
	return start;

fail:
	return (uintptr_t)-1; /* MAP_FAILED */
}

uintptr_t sys_munmap(uintptr_t addr, size_t len)
{
	if (addr == 0 || len == 0)
		return 0;
	size_t sz = PAGE_ALIGN_UP(len);
	uintptr_t range_end = addr + sz;
	/* 不允许 unmap TRAMPOLINE/TRAPFRAME/栈区域 */
	if ((addr >= TRAMPOLINE && addr < TRAMPOLINE + PAGE_SIZE) ||
	    (addr >= TRAPFRAME && addr < TRAPFRAME + PAGE_SIZE) ||
	    (range_end > USER_TOP - PAGE_SIZE && addr < USER_TOP))
		return -EINVAL;
	for (uintptr_t a = addr; a < range_end; a += PAGE_SIZE) {
		uintptr_t pa = walkaddr(curproc()->pagetable, a);
		if (pa) {
			unmappages(curproc()->pagetable, a, 1);
			kfree((void *)pa);
		}
	}
	return 0;
}

uintptr_t sys_mprotect(uintptr_t addr, size_t len, int prot)
{
	/* stub: 返回成功 */
	(void)addr;
	(void)len;
	(void)prot;
	return 0;
}

/* ============================================================
 * FD: dup / dup3 / pipe2 / lseek / fcntl / ioctl
 * ============================================================ */

uintptr_t sys_dup(uintptr_t oldfd)
{
	struct proc *p = curproc();
	struct file *f = fd_get(p->fd_table, (int)oldfd);
	if (!f)
		return -EBADF;

	int newfd = fd_alloc(p->fd_table);
	if (newfd < 0) {
		file_put(f);
		return -EMFILE;
	}

	if (fd_install(p->fd_table, newfd, f) < 0) {
		file_put(f);
		return -EBADF;
	}

	/* fd_install 增加了引用，fd_get 也增加了，需要释放 fd_get 的一次 */
	file_put(f);

	return newfd;
}

uintptr_t sys_dup3(uintptr_t oldfd, uintptr_t newfd, uintptr_t flags)
{
	(void)flags;
	struct proc *p = curproc();

	if ((int)newfd < 0 || (int)newfd >= (int)p->fd_table->max_fds)
		return -EBADF;

	if ((int)oldfd == (int)newfd)
		return -EINVAL;

	struct file *f = fd_get(p->fd_table, (int)oldfd);
	if (!f)
		return -EBADF;

	/* 关闭 newfd 上已有的文件 */
	fd_free(p->fd_table, (int)newfd);

	if (fd_install(p->fd_table, (int)newfd, f) < 0) {
		file_put(f);
		return -EBADF;
	}

	file_put(f);
	return newfd;
}

uintptr_t sys_pipe2(uintptr_t pipefd, uintptr_t flags)
{
	(void)flags;
	struct proc *p = curproc();

	struct file *rf = file_alloc();
	struct file *wf = file_alloc();
	if (!rf || !wf) {
		if (rf) file_free(rf);
		if (wf) file_free(wf);
		return -ENOMEM;
	}

	if (pipe_create(rf, wf) < 0) {
		file_free(rf);
		file_free(wf);
		return -ENOMEM;
	}

	int rfd = fd_alloc(p->fd_table);
	int wfd = fd_alloc(p->fd_table);
	if (rfd < 0 || wfd < 0) {
		if (rfd >= 0)
			fd_free(p->fd_table, rfd);
		if (wfd >= 0)
			fd_free(p->fd_table, wfd);
		file_free(rf);
		file_free(wf);
		return -EMFILE;
	}

	fd_install(p->fd_table, rfd, rf);
	fd_install(p->fd_table, wfd, wf);

	int fds[2] = { rfd, wfd };
	if (copyout(p->pagetable, pipefd, (char *)fds, sizeof(fds)) < 0) {
		fd_free(p->fd_table, rfd);
		fd_free(p->fd_table, wfd);
		return -EFAULT;
	}

	return 0;
}

uintptr_t sys_lseek(uintptr_t fd, loff_t offset, int whence)
{
	struct proc *p = curproc();
	struct file *f = fd_get(p->fd_table, (int)fd);
	if (!f)
		return -EBADF;

	loff_t ret = file_lseek(f, offset, whence);
	file_put(f);
	return ret;
}

uintptr_t sys_readv(uintptr_t fd, uintptr_t iov, uintptr_t iovcnt)
{
	/* 简化：逐个 iovec 调用 sys_read */
	struct iovec {
		uintptr_t iov_base;
		size_t iov_len;
	};
	size_t total = 0;
	for (uintptr_t i = 0; i < iovcnt; i++) {
		struct iovec v;
		if (copyin(curproc()->pagetable, (char *)&v, iov + i * sizeof(v), sizeof(v)) < 0)
			return -EFAULT;
		if (v.iov_len == 0) continue;
		ssize_t n = sys_read(fd, v.iov_base, v.iov_len);
		if (n < 0)
			return total > 0 ? total : n;
		total += n;
		if ((size_t)n < v.iov_len)
			break;
	}
	return total;
}

uintptr_t sys_writev(uintptr_t fd, uintptr_t iov, uintptr_t iovcnt)
{
	struct iovec {
		uintptr_t iov_base;
		size_t iov_len;
	};
	size_t total = 0;
	for (uintptr_t i = 0; i < iovcnt; i++) {
		struct iovec v;
		if (copyin(curproc()->pagetable, (char *)&v, iov + i * sizeof(v), sizeof(v)) < 0)
			return -EFAULT;
		if (v.iov_len == 0) continue;
		ssize_t n = sys_write(fd, v.iov_base, v.iov_len);
		if (n < 0)
			return total > 0 ? total : n;
		total += n;
		if ((size_t)n < v.iov_len)
			break;
	}
	return total;
}

uintptr_t sys_fcntl(uintptr_t fd, uintptr_t cmd, uintptr_t arg)
{
	/* stub: 大部分 fcntl 命令直接返回成功 */
	(void)fd;
	(void)cmd;
	(void)arg;
	return 0;
}

uintptr_t sys_ioctl(uintptr_t fd, uintptr_t request, uintptr_t arg)
{
	/* stub: 终端 ioctl 暂不实现 */
	(void)fd;
	(void)request;
	(void)arg;
	return 0;
}

/* ============================================================
 * FS: mkdirat / unlinkat
 * ============================================================ */

uintptr_t sys_mkdirat(uintptr_t dirfd, uintptr_t pathname, uintptr_t mode)
{
	struct proc *p = curproc();
	char path[256];

	if (copyinstr(p->pagetable, path, pathname, sizeof(path)) < 0)
		return -EFAULT;

	struct dentry *base = (path[0] == '/') ? NULL : p->pwd;
	return vfs_mkdir_cwd(path, (umode_t)mode, base);
}

uintptr_t sys_unlinkat(uintptr_t dirfd, uintptr_t pathname, uintptr_t flags)
{
	struct proc *p = curproc();
	char path[256];

	if (copyinstr(p->pagetable, path, pathname, sizeof(path)) < 0)
		return -EFAULT;

	struct dentry *base = (path[0] == '/') ? NULL : p->pwd;
	if (flags & AT_REMOVEDIR)
		return vfs_rmdir(path);
	return vfs_unlink_cwd(path, base);
}

/* ============================================================
 * Signals (stub)
 * ============================================================ */

uintptr_t sys_rt_sigaction(int sig, uintptr_t act, uintptr_t oact, size_t sigsetsize)
{
	/* stub: 信号处理暂不实现 */
	(void)sig;
	(void)act;
	(void)oact;
	(void)sigsetsize;
	return 0;
}

uintptr_t sys_rt_sigprocmask(int how, uintptr_t set, uintptr_t oset, size_t sigsetsize)
{
	(void)how;
	(void)set;
	(void)oset;
	(void)sigsetsize;
	return 0;
}

/* ============================================================
 * Misc: uname
 * ============================================================ */

struct utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

uintptr_t sys_uname(uintptr_t buf)
{
	struct utsname u;
	strcpy(u.sysname, "NoobKernel");
	strcpy(u.nodename, "qemu");
	strcpy(u.release, "1.0.0");
	strcpy(u.version, "Yu-NoobKernel riscv64");
	strcpy(u.machine, "riscv64");
	strcpy(u.domainname, "");

	if (copyout(curproc()->pagetable, buf, (char *)&u, sizeof(u)) < 0)
		return -EFAULT;

	return 0;
}

/* ============================================================
 * FS: getcwd / chdir
 * ============================================================ */

uintptr_t sys_getcwd(uintptr_t buf, size_t size)
{
	struct proc *p = curproc();
	struct dentry *d = p->pwd;
	if (!d)
		return -ENOENT;

	struct dentry *root = vfs_get_root();
	char path[256];
	int len = 0;
	path[255] = '\0';

	/* 从当前 dentry 向上遍历到根，逆向拼接 */
	struct dentry *stack[32];
	int depth = 0;
	while (d != root && depth < 32) {
		stack[depth++] = d;
		d = d->d_parent;
	}

	for (int i = depth - 1; i >= 0; i--) {
		if (len < 255) path[len++] = '/';
		struct qstr *name = &stack[i]->d_name;
		for (u32 j = 0; j < name->len && len < 255; j++)
			path[len++] = name->name[j];
	}
	if (len == 0) {
		path[len++] = '/';
	}
	path[len] = '\0';

	if (copyout(p->pagetable, buf, path, len + 1) < 0)
		return -EFAULT;
	return buf;
}

uintptr_t sys_chdir(uintptr_t pathname)
{
	struct proc *p = curproc();
	char path[256];
	if (copyinstr(p->pagetable, path, pathname, sizeof(path)) < 0)
		return -EFAULT;

	struct dentry *dentry = vfs_path_lookup(path[0] == '/' ? 
										NULL : p->pwd, path, LOOKUP_FOLLOW);
	if (IS_ERR(dentry) || !dentry)
		return dentry ? PTR_ERR(dentry) : -ENOENT;

	if (!dentry->d_inode || !S_ISDIR(dentry->d_inode->i_mode)) {
		dentry_put(dentry);
		return -ENOTDIR;
	}

	struct dentry *old = p->pwd;
	p->pwd = dentry;
	if (old)
		dentry_put(old);
	return 0;
}

/* ============================================================
 * FS: fstat
 * ============================================================ */

uintptr_t sys_fstat(int fd, uintptr_t statbuf)
{
	struct proc *p = curproc();
	struct file *f = fd_get(p->fd_table, fd);
	if (!f)
		return -EBADF;

	struct inode *inode = f->f_dentry ? f->f_dentry->d_inode : NULL;

	struct {
		u64 st_dev;
		u64 st_ino;
		u32 st_mode;
		u32 st_nlink;
		u32 st_uid;
		u32 st_gid;
		u64 st_rdev;
		u64 __pad;
		u64 st_size;
		u32 st_blksize;
		int __pad2;
		u64 st_blocks;
		u64 st_atime_sec;
		u64 st_atime_nsec;
		u64 st_mtime_sec;
		u64 st_mtime_nsec;
		u64 st_ctime_sec;
		u64 st_ctime_nsec;
		u64 __unused[2];
	} kst;

	memset(&kst, 0, sizeof(kst));

	if (inode) {
		kst.st_dev = inode->i_sb ? inode->i_sb->s_dev : 0;
		kst.st_ino = inode->i_ino;
		kst.st_mode = inode->i_mode;
		kst.st_nlink = inode->i_nlink;
		kst.st_uid = inode->i_uid;
		kst.st_gid = inode->i_gid;
		kst.st_rdev = inode->i_rdev;
		kst.__pad = 0;
		kst.st_size = inode->i_size;
		kst.st_blksize = 1024;
		kst.__pad2 = 0;
		kst.st_blocks = inode->i_blocks;
		kst.st_atime_sec = inode->i_atime;
		kst.st_atime_nsec = 0;
		kst.st_mtime_sec = inode->i_mtime;
		kst.st_mtime_nsec = 0;
		kst.st_ctime_sec = inode->i_ctime;
		kst.st_ctime_nsec = 0;
		kst.__unused[0] = 0;
		kst.__unused[1] = 0;
	}

	if (copyout(p->pagetable, statbuf, (char *)&kst, sizeof(kst)) < 0) {
		file_put(f);
		return -EFAULT;
	}

	file_put(f);
	return 0;
}

uintptr_t sys_fstatat(uintptr_t dirfd, uintptr_t pathname, uintptr_t statbuf,
		     uintptr_t flags)
{
	struct proc *p = curproc();
	char path[256];

	if (copyinstr(p->pagetable, path, pathname, sizeof(path)) < 0)
		return -EFAULT;

	(void)dirfd;
	(void)flags;

	/* 通过路径查找 inode */
	struct dentry *base = p->pwd ? p->pwd : vfs_get_root();
	struct dentry *d = vfs_path_lookup(base, path, 0);
	if (!d)
		return -ENOENT;

	struct inode *inode = d->d_inode;
	if (!inode) {
		dentry_put(d);
		return -ENOENT;
	}

	struct {
		u64 st_dev;
		u64 st_ino;
		u32 st_mode;
		u32 st_nlink;
		u32 st_uid;
		u32 st_gid;
		u64 st_rdev;
		u64 __pad;
		u64 st_size;
		u32 st_blksize;
		int __pad2;
		u64 st_blocks;
		u64 st_atime_sec;
		u64 st_atime_nsec;
		u64 st_mtime_sec;
		u64 st_mtime_nsec;
		u64 st_ctime_sec;
		u64 st_ctime_nsec;
		u64 __unused[2];
	} kst;

	memset(&kst, 0, sizeof(kst));
	kst.st_dev = inode->i_sb ? inode->i_sb->s_dev : 0;
	kst.st_ino = inode->i_ino;
	kst.st_mode = inode->i_mode;
	kst.st_nlink = inode->i_nlink;
	kst.st_uid = inode->i_uid;
	kst.st_gid = inode->i_gid;
	kst.st_rdev = inode->i_rdev;
	kst.__pad = 0;
	kst.st_size = inode->i_size;
	kst.st_blksize = 1024;
	kst.__pad2 = 0;
	kst.st_blocks = inode->i_blocks;
	kst.st_atime_sec = inode->i_atime;
	kst.st_atime_nsec = 0;
	kst.st_mtime_sec = inode->i_mtime;
	kst.st_mtime_nsec = 0;
	kst.st_ctime_sec = inode->i_ctime;
	kst.st_ctime_nsec = 0;
	kst.__unused[0] = 0;
	kst.__unused[1] = 0;

	dentry_put(d);

	if (copyout(p->pagetable, statbuf, (char *)&kst, sizeof(kst)) < 0)
		return -EFAULT;

	return 0;
}

/* ============================================================
 * FS: mount / umount (stub --- 不支持 vfat)
 * ============================================================ */

/* Parse /dev/vda{N} path to dev_t
   vda  -> MKDEV(BLK_MAJOR_VIRTIO, 0)
   vda2 -> MKDEV(BLK_MAJOR_VIRTIO, 1) */
static int parse_dev_path(const char *dev_path, dev_t *out)
{
	const char *name;
	if (strncmp(dev_path, "/dev/", 5) == 0)
		name = dev_path + 5;
	else
		return -ENODEV;

	if (name[0] != 'v' || name[1] != 'd')
		return -ENODEV;

	const char *p = name + 2;
	while (*p >= 'a' && *p <= 'z')
		p++;

	int minor = 0;
	if (*p >= '1' && *p <= '9')
		minor = (*p - '0') - 1;

	*out = MKDEV(BLK_MAJOR_VIRTIO, minor);
	return 0;
}

uintptr_t sys_mount(uintptr_t dev, uintptr_t dir, uintptr_t type,
		   uintptr_t flags, uintptr_t data)
{
	struct proc *p = curproc();
	char dev_path[256], dir_path[256], fstype[64];

	(void)flags;
	(void)data;

	if (copyinstr(p->pagetable, dev_path, dev, sizeof(dev_path)) < 0)
		return -EFAULT;
	if (copyinstr(p->pagetable, dir_path, dir, sizeof(dir_path)) < 0)
		return -EFAULT;
	if (copyinstr(p->pagetable, fstype, type, sizeof(fstype)) < 0)
		return -EFAULT;

	dev_t devno;
	if (parse_dev_path(dev_path, &devno) < 0)
		return -ENODEV;

	if (!blk_lookup(devno))
		return -ENODEV;

	struct file_system_type *fs = get_fs(fstype);
	if (!fs) {
		fs = get_fs("ext4");
		if (!fs)
			return -ENODEV;
	}

	struct dentry *base = (dir_path[0] == '/') ? NULL : p->pwd;
	return vfs_mount_at(base, dir_path, fs, devno);
}

uintptr_t sys_umount(uintptr_t target, uintptr_t flags)
{
	struct proc *p = curproc();
	char target_path[256];

	(void)flags;

	if (copyinstr(p->pagetable, target_path, target,
		      sizeof(target_path)) < 0)
		return -EFAULT;

	struct dentry *base =
		(target_path[0] == '/') ? NULL : p->pwd;
	return vfs_umount_at(base, target_path);
}

/* ============================================================
 * Time: gettimeofday / nanosleep / times / sched_yield
 * ============================================================ */

uintptr_t sys_gettimeofday(uintptr_t tv, uintptr_t tz)
{
	struct proc *p = curproc();
	(void)tz;

	u64 now = r_time();
	u64 sec = now / TIMEBASE_FREQ;
	u64 usec = (now % TIMEBASE_FREQ) * 1000000ULL / TIMEBASE_FREQ;

	struct {
		u64 sec;
		u64 usec;
	} timeval;
	timeval.sec = sec;
	timeval.usec = usec;

	if (copyout(p->pagetable, tv, (char *)&timeval, sizeof(timeval)) < 0)
		return -EFAULT;
	return 0;
}

uintptr_t sys_nanosleep(uintptr_t req, uintptr_t rem)
{
	struct proc *p = curproc();

	struct {
		u64 tv_sec;
		u64 tv_nsec;
	} req_ts;
	if (copyin(p->pagetable, (char *)&req_ts, req, sizeof(req_ts)) < 0)
		return -EFAULT;

	u64 ns = req_ts.tv_sec * 1000000000ULL + req_ts.tv_nsec;
	u64 start = r_time();
	u64 end = start + ns_to_cputime(ns);

	while (r_time() < end) {
		/* busy-wait: 单核 QEMU 环境下简单实现 */
	}

	if (rem)
		memset((void *)rem, 0, sizeof(req_ts));

	return 0;
}

uintptr_t sys_times(uintptr_t tbuf)
{
	struct proc *p = curproc();

	struct {
		u64 tms_utime;
		u64 tms_stime;
		u64 tms_cutime;
		u64 tms_cstime;
	} tms;
	memset(&tms, 0, sizeof(tms));

	if (tbuf) {
		if (copyout(p->pagetable, tbuf, (char *)&tms, sizeof(tms)) < 0)
			return -EFAULT;
	}

	return r_time() / TIMEBASE_FREQ;
}

uintptr_t sys_sched_yield(void)
{
	sched_yield();
	return 0;
}

/* ============================================================
 * Syscall dispatcher
 * ============================================================ */

void syscall(void)
{
	struct proc *p = curproc();
	uintptr_t a0 = p->tf->a0;
	uintptr_t a1 = p->tf->a1;
	uintptr_t a2 = p->tf->a2;
	uintptr_t a3 = p->tf->a3;
	uintptr_t a4 = p->tf->a4;
	uintptr_t a5 = p->tf->a5;
	uintptr_t n = p->tf->a7;

	syscall_fn_t fn = syscall_lookup((int)n);
	if (!fn) {
		warnf("Unknown syscall %ld from pid %d", n, p->pid);
		p->tf->a0 = (uintptr_t)-ENOSYS;
		return;
	}

	p->tf->a0 = fn(a0, a1, a2, a3, a4, a5);
}

/* ============================================================
 * Syscall 注册（内核初始化时调用）
 * ============================================================ */

void syscall_init(void)
{
	/* I/O */
	syscall_register(SYS_read,        (syscall_fn_t)sys_read);
	syscall_register(SYS_write,       (syscall_fn_t)sys_write);
	syscall_register(SYS_close,       (syscall_fn_t)sys_close);
	syscall_register(SYS_openat,      (syscall_fn_t)sys_openat);
	syscall_register(SYS_getdents64,  (syscall_fn_t)sys_getdents);

	/* 进程管理 — fork/clone 共用 SYS_clone (220)，由 child_stack==0 区分 */
	syscall_register(SYS_clone,       (syscall_fn_t)sys_clone);
	syscall_register(SYS_execve,      (syscall_fn_t)sys_execve);
	syscall_register(SYS_wait4,       (syscall_fn_t)sys_wait4);
	syscall_register(SYS_getpid,      (syscall_fn_t)sys_getpid);
	syscall_register(SYS_getppid,     (syscall_fn_t)sys_getppid);
	syscall_register(SYS_exit,        (syscall_fn_t)sys_exit);
	syscall_register(SYS_exit_group,  (syscall_fn_t)sys_exit_group);
	syscall_register(SYS_set_tid_address, (syscall_fn_t)sys_set_tid_address);

	/* 内存 */
	syscall_register(SYS_brk,         (syscall_fn_t)sys_brk);
	syscall_register(SYS_mmap,        (syscall_fn_t)sys_mmap);
	syscall_register(SYS_munmap,      (syscall_fn_t)sys_munmap);
	syscall_register(SYS_mprotect,    (syscall_fn_t)sys_mprotect);

	/* FD 操作 */
	syscall_register(SYS_dup,         (syscall_fn_t)sys_dup);
	syscall_register(SYS_dup3,        (syscall_fn_t)sys_dup3);
	syscall_register(SYS_pipe2,       (syscall_fn_t)sys_pipe2);
	syscall_register(SYS_lseek,       (syscall_fn_t)sys_lseek);
	syscall_register(SYS_readv,       (syscall_fn_t)sys_readv);
	syscall_register(SYS_writev,      (syscall_fn_t)sys_writev);
	syscall_register(SYS_fcntl,       (syscall_fn_t)sys_fcntl);
	syscall_register(SYS_ioctl,       (syscall_fn_t)sys_ioctl);

	/* 文件系统 */
	syscall_register(SYS_mkdirat,     (syscall_fn_t)sys_mkdirat);
	syscall_register(SYS_unlinkat,    (syscall_fn_t)sys_unlinkat);
	syscall_register(SYS_getcwd,      (syscall_fn_t)sys_getcwd);
	syscall_register(SYS_chdir,       (syscall_fn_t)sys_chdir);
	syscall_register(SYS_fstat,       (syscall_fn_t)sys_fstat);
	syscall_register(SYS_fstatat,     (syscall_fn_t)sys_fstatat);
	syscall_register(SYS_mount,       (syscall_fn_t)sys_mount);
	syscall_register(SYS_umount,      (syscall_fn_t)sys_umount);

	/* 信号 (stub) */
	syscall_register(SYS_rt_sigaction, (syscall_fn_t)sys_rt_sigaction);
	syscall_register(SYS_rt_sigprocmask, (syscall_fn_t)sys_rt_sigprocmask);

	/* 杂项 */
	syscall_register(SYS_uname,       (syscall_fn_t)sys_uname);
	syscall_register(SYS_gettimeofday, (syscall_fn_t)sys_gettimeofday);
	syscall_register(SYS_nanosleep,    (syscall_fn_t)sys_nanosleep);
	syscall_register(SYS_times,        (syscall_fn_t)sys_times);
	syscall_register(SYS_sched_yield, (syscall_fn_t)sys_sched_yield);
	syscall_register(SYS_getrandom, (syscall_fn_t)sys_getrandom);

	/* 自定义 */
	syscall_register(SYS_shutdown,    (syscall_fn_t)sys_shutdown);
	syscall_register(SYS_reboot,      (syscall_fn_t)sys_reboot);
}
