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

#include <misc/errno.h>
#include <misc/align.h>

extern pagetable_t kpagetable;
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
	infof("Process %d (%s) exited with status %d", p->pid, p->comm, status);
	p->exit_code = status;
	p->state = PROC_ZOMBIE;
	sched_yield();
	panic("unreachable");
	return 0;
}

uintptr_t sys_exit_group(int status) { return sys_exit(status); }

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

	struct file *f = vfs_open(path, (u32)flags);
	if (IS_ERR(f))
		return (uintptr_t)PTR_ERR(f);

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

	/* 计算需要复制的用户空间大小 (brk_end 或固定范围) */
	uintptr_t sz = parent->brk_end ? parent->brk_end : USER_TOP;
	sz = PAGE_ALIGN_UP(sz);
	if (uvmcopy(parent->pagetable, child->pagetable, sz) < 0)
		goto fail_freept;

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
		     PTE_R | PTE_X | PTE_V) != 0)
		goto fail_freetf;

	if (mappages(child->pagetable, TRAPFRAME, (uintptr_t)child->tf, 1,
		     PTE_R | PTE_W | PTE_V) != 0)
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
} elf64_ehdr_t;

typedef struct {
	u32 p_type;
	u32 p_flags;
	u64 p_offset;
	u64 p_vaddr;
	u64 p_paddr;
	u64 p_filesz;
	u64 p_memsz;
	u64 p_align;
} elf64_phdr_t;

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

	/* 打开文件 */
	struct file *f = vfs_open(path, O_RDONLY);
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

	/* 释放旧的用户空间映射（保留 trampoline 和 trapframe） */
	uintptr_t old_sz = p->brk_end ? p->brk_end : USER_TOP;
	old_sz = PAGE_ALIGN_UP(old_sz);

	for (uintptr_t a = 0; a < old_sz; a += PAGE_SIZE) {
		if (a == TRAMPOLINE || a == TRAPFRAME)
			continue;
		uintptr_t pa = walkaddr(p->pagetable, a);
		if (pa) {
			unmappages(p->pagetable, a, 1);
			kfree((void *)pa);
		}
	}

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

			int perm = PTE_U | PTE_V;
			if (phdr.p_flags & PF_R) perm |= PTE_R;
			if (phdr.p_flags & PF_W) perm |= PTE_W;
			if (phdr.p_flags & PF_X) perm |= PTE_X;

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
			     PTE_R | PTE_W | PTE_U | PTE_V) != 0) {
			kfree((void *)pa);
			goto exec_fail;
		}
	}

	/* 设置 trapframe */
	p->tf->epc = ehdr.e_entry;
	p->tf->sp = USER_TOP;
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

	spinlock_acquire(&p->lock);

	struct list_head *pos;
	list_for_each(pos, &p->children) {
		struct proc *child = list_entry(pos, struct proc, sibling);
		int target = (int)pid_arg;

		if (target == -1 || target == child->pid) {
			if (child->state == PROC_ZOMBIE) {
				/* 写回退出状态 */
				if (wstatus != 0) {
					int status = child->exit_code;
					if (copyout(p->pagetable, wstatus, (char *)&status,
						    sizeof(status)) < 0) {
						spinlock_release(&p->lock);
						return -EFAULT;
					}
				}

				pid_t cpid = child->pid;

				/* 从 children 列表移除并释放 */
				list_del(&child->sibling);
				spinlock_release(&p->lock);
				free_proc(child);

				return cpid;
			}
		}
	}

	spinlock_release(&p->lock);

	/* 没有找到 zombie 子进程，如果指定了具体 pid 则返回 ECHILD */
	if ((int)pid_arg > 0)
		return -ECHILD;

	/* pid == -1 但没有 zombie，返回 ECHILD (简化：不阻塞等待) */
	return -ECHILD;
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
				     PTE_R | PTE_W | PTE_U | PTE_V) != 0) {
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

	/* 简化实现：只支持匿名映射 */
	if (fd >= 0)
		return -ENOSYS;

	(void)prot;
	(void)offset;

	size_t sz = PAGE_ALIGN_UP(len);
	uintptr_t start;

	if (flags & 0x20 /* MAP_FIXED */) {
		start = addr;
	} else {
		/* 从 brk_end 之后分配，或默认起始地址 */
		start = PAGE_ALIGN_UP(p->brk_end ? p->brk_end : 0x10000);
	}

	for (uintptr_t a = start; a < start + sz; a += PAGE_SIZE) {
		if (walkaddr(p->pagetable, a) != 0)
			continue;
		uintptr_t pa = (uintptr_t)kzalloc(PAGE_SIZE);
		if (!pa)
			return -ENOMEM;
		if (mappages(p->pagetable, a, pa, 1,
			     PTE_R | PTE_W | PTE_U | PTE_V) != 0) {
			kfree((void *)pa);
			return -ENOMEM;
		}
	}

	/* 更新 brk_end */
	if (start + sz > p->brk_end)
		p->brk_end = start + sz;

	return start;
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
	/* stub: pipe 暂未实现 */
	return -ENOSYS;
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
	(void)dirfd;

	if (copyinstr(p->pagetable, path, pathname, sizeof(path)) < 0)
		return -EFAULT;

	/* 简化：只支持绝对路径 */
	if (path[0] != '/')
		return -ENOSYS;

	struct dentry *parent = p->pwd;
	if (!parent)
		return -ENOENT;

	/* 查找父目录，创建子目录 */
	/* TODO: 通过 VFS 创建目录 */
	return -ENOSYS;
}

uintptr_t sys_unlinkat(uintptr_t dirfd, uintptr_t pathname, uintptr_t flags)
{
	(void)dirfd;
	(void)pathname;
	(void)flags;
	return -ENOSYS;
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

	/* 进程管理 */
	syscall_register(SYS_fork,        (syscall_fn_t)sys_fork);
	syscall_register(SYS_clone,       (syscall_fn_t)sys_fork);
	syscall_register(SYS_execve,      (syscall_fn_t)sys_execve);
	syscall_register(SYS_wait4,       (syscall_fn_t)sys_wait4);
	syscall_register(SYS_getpid,      (syscall_fn_t)sys_getpid);
	syscall_register(SYS_getppid,     (syscall_fn_t)sys_getppid);
	syscall_register(SYS_exit,        (syscall_fn_t)sys_exit);
	syscall_register(SYS_exit_group,  (syscall_fn_t)sys_exit_group);

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

	/* 信号 (stub) */
	syscall_register(SYS_rt_sigaction, (syscall_fn_t)sys_rt_sigaction);
	syscall_register(SYS_rt_sigprocmask, (syscall_fn_t)sys_rt_sigprocmask);

	/* 杂项 */
	syscall_register(SYS_uname,       (syscall_fn_t)sys_uname);

	/* 自定义 */
	syscall_register(SYS_shutdown,    (syscall_fn_t)sys_shutdown);
	syscall_register(SYS_reboot,      (syscall_fn_t)sys_reboot);
}
