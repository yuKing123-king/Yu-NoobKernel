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

/* 用户栈页数。libcbench 的 regex/stdio 路径会把主线程栈压到 256KB 以下。 */
#define STACK_PAGES     128
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
#define PTE_MMAP_RESERVED (1UL << 8)
extern char trampoline[];

/* ============================================================
 * Syscall 注册表（稀疏散列，避免 512 项大数组）
 * 使用开放寻址哈希表，容量 64，负载因子 < 0.5
 * ============================================================ */

#define SC_TABLE_BITS 8
#define SC_TABLE_SIZE (1 << SC_TABLE_BITS)
#define SC_TABLE_MASK (SC_TABLE_SIZE - 1)

static struct {
	int nr;
	syscall_fn_t fn;
} sc_table[SC_TABLE_SIZE];
static int clone_debug_budget = 0;
static int thread_exit_debug_budget = 0;
static int tidaddr_debug_budget = 0;
static int munmap_debug_budget = 0;
static int mmap_debug_budget = 0;
static int brk_debug_budget = 0;
static int child_syscall_debug_budget = 0;

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)
#define FUTEX_BUCKETS 64

static struct wait_queue futex_wqs[FUTEX_BUCKETS];
static bool futex_ready;
static int futex_debug_budget = 0;

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

static inline struct wait_queue *futex_bucket(uintptr_t uaddr)
{
	return &futex_wqs[(uaddr >> 2) & (FUTEX_BUCKETS - 1)];
}

static void futex_init_once(void)
{
	if (futex_ready)
		return;
	for (int i = 0; i < FUTEX_BUCKETS; i++)
		wait_queue_init(&futex_wqs[i]);
	futex_ready = true;
}

static int futex_read_u32(struct proc *p, uintptr_t uaddr, int *val)
{
	int tmp;

	if (copyin(p->pagetable, (char *)&tmp, uaddr, sizeof(tmp)) < 0)
		return -EFAULT;
	*val = tmp;
	return 0;
}

static void futex_wake_addr(uintptr_t uaddr)
{
	struct wait_queue *wq;
	const int wake_all = 0x7fffffff;

	futex_init_once();
	wq = futex_bucket(uaddr);
	wait_queue_wakeup_addr(wq, uaddr, wake_all);
}

static void sync_shared_brk(struct proc *owner, uintptr_t brk_end)
{
	struct list_head *pos;

	if (!owner)
		return;
	owner->brk_end = brk_end;

	spinlock_acquire(&owner->lock);
	list_for_each(pos, &owner->children) {
		struct proc *child = list_entry(pos, struct proc, sibling);
		if (child->vm_shared)
			child->brk_end = brk_end;
	}
	spinlock_release(&owner->lock);
}

static struct proc *shared_vm_owner(struct proc *p)
{
	if (p && p->vm_shared && p->parent)
		return p->parent;
	return p;
}

static inline void shared_vm_lock(struct proc *p)
{
	if (p)
		spinlock_acquire(&p->vm_lock);
}

static inline void shared_vm_unlock(struct proc *p)
{
	if (p)
		spinlock_release(&p->vm_lock);
}

static int shared_map_page(struct proc *owner, uintptr_t va, uintptr_t pa,
			   int pte_flags)
{
	struct list_head *pos;
	int ret;

	ret = mappages(owner->pagetable, va, pa, 1, pte_flags);
	if (ret != 0)
		return ret;

	spinlock_acquire(&owner->lock);
	list_for_each(pos, &owner->children) {
		struct proc *child = list_entry(pos, struct proc, sibling);
		if (!child->vm_shared)
			continue;
		ret = mappages(child->pagetable, va, pa, 1, pte_flags);
		if (ret != 0) {
			spinlock_release(&owner->lock);
			return ret;
		}
	}
	spinlock_release(&owner->lock);
	sfence_vma();
	return 0;
}

static int shared_reserve_page(struct proc *owner, uintptr_t va)
{
	struct list_head *pos;
	pte_t *pte = va2pte(owner->pagetable, va, true);

	if (!pte)
		return -ENOMEM;
	*pte = PTE_MMAP_RESERVED;

	spinlock_acquire(&owner->lock);
	list_for_each(pos, &owner->children) {
		struct proc *child = list_entry(pos, struct proc, sibling);
		if (!child->vm_shared)
			continue;
		pte = va2pte(child->pagetable, va, true);
		if (!pte) {
			spinlock_release(&owner->lock);
			return -ENOMEM;
		}
		*pte = PTE_MMAP_RESERVED;
	}
	spinlock_release(&owner->lock);
	sfence_vma();
	return 0;
}

static void shared_unmap_page(struct proc *owner, uintptr_t va)
{
	struct list_head *pos;
	pte_t *pte;

	pte = va2pte(owner->pagetable, va, false);
	if (pte)
		*pte = 0;
	unmappages(owner->pagetable, va, 1);
	spinlock_acquire(&owner->lock);
	list_for_each(pos, &owner->children) {
		struct proc *child = list_entry(pos, struct proc, sibling);
		if (!child->vm_shared)
			continue;
		pte = va2pte(child->pagetable, va, false);
		if (pte)
			*pte = 0;
		unmappages(child->pagetable, va, 1);
	}
	spinlock_release(&owner->lock);
	sfence_vma();
}

static int shared_protect_page(struct proc *owner, uintptr_t va, int pte_flags)
{
	struct list_head *pos;
	pte_t *pte;
	bool prot_none = (pte_flags & PTE_U) == 0;

	pte = va2pte(owner->pagetable, va, false);
	if (!pte || !(*pte & PTE_V)) {
		if (!pte || !(*pte & PTE_MMAP_RESERVED))
			return -ENOMEM;
		if (prot_none)
			return 0;
		uintptr_t pa = (uintptr_t)kzalloc(PAGE_SIZE);
		if (!pa)
			return -ENOMEM;
		return shared_map_page(owner, va, pa, pte_flags);
	}
	*pte = PA2PTE(PTE2PA(*pte)) | pte_flags | PTE_V | PTE_A |
	       ((pte_flags & PTE_W) ? PTE_D : 0);

	spinlock_acquire(&owner->lock);
	list_for_each(pos, &owner->children) {
		struct proc *child = list_entry(pos, struct proc, sibling);
		if (!child->vm_shared)
			continue;
		pte = va2pte(child->pagetable, va, false);
		if (!pte || !(*pte & PTE_V)) {
			spinlock_release(&owner->lock);
			return -ENOMEM;
		}
		*pte = PA2PTE(PTE2PA(*pte)) | pte_flags | PTE_V | PTE_A |
		       ((pte_flags & PTE_W) ? PTE_D : 0);
	}
	spinlock_release(&owner->lock);
	sfence_vma();
	return 0;
}

static bool user_range_has_mapping(pagetable_t pt, uintptr_t start, size_t sz)
{
	uintptr_t end = start + sz;

	for (uintptr_t a = start; a < end; a += PAGE_SIZE) {
		pte_t *pte = va2pte(pt, a, false);
		if (pte && ((*pte & PTE_V) || (*pte & PTE_MMAP_RESERVED)))
			return true;
	}
	return false;
}

static uintptr_t find_free_user_range(pagetable_t pt, uintptr_t start, size_t sz,
				      uintptr_t limit)
{
	uintptr_t cur = PAGE_ALIGN_UP(start);
	uintptr_t end_limit;

	if (sz == 0 || cur >= limit)
		return 0;
	if (sz > limit - cur)
		return 0;

	end_limit = limit - sz;
	while (cur <= end_limit) {
		if (!user_range_has_mapping(pt, cur, sz))
			return cur;
		cur += PAGE_SIZE;
	}
	return 0;
}

static struct proc *find_child_by_pid(struct proc *parent, int pid)
{
	struct list_head *pos;

	if (!parent)
		return NULL;

	list_for_each(pos, &parent->children) {
		struct proc *child = list_entry(pos, struct proc, sibling);
		if (child->pid == pid)
			return child;
	}

	return NULL;
}

static void reap_zombie_threads(struct proc *parent)
{
	struct list_head *pos, *n;

	if (!parent)
		return;

	spinlock_acquire(&parent->lock);
	list_for_each_safe(pos, n, &parent->children) {
		struct proc *child = list_entry(pos, struct proc, sibling);

		if (!child->vm_shared || child->state != PROC_ZOMBIE)
			continue;

		child->parent = NULL;
		list_del(&child->sibling);
		INIT_LIST_HEAD(&child->sibling);
		spinlock_release(&parent->lock);
		free_proc(child);
		spinlock_acquire(&parent->lock);
	}
	spinlock_release(&parent->lock);
}

struct kernel_timespec {
	long tv_sec;
	long tv_nsec;
};

struct kernel_rtc_time {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

struct pseudo_text_file {
	char *data;
	size_t len;
};

#define RTC_RD_TIME 0x80247009UL

static ssize_t pseudo_text_read(struct file *file, void *buf, size_t count,
				 loff_t *pos)
{
	struct pseudo_text_file *pf = (struct pseudo_text_file *)file->f_private;
	size_t avail;

	if (!pf || !buf || !pos)
		return -EINVAL;
	if ((size_t)*pos >= pf->len)
		return 0;

	avail = pf->len - (size_t)*pos;
	if (count > avail)
		count = avail;
	memcpy(buf, pf->data + *pos, count);
	*pos += count;
	return count;
}

static int pseudo_text_release(struct inode *inode, struct file *file)
{
	(void)inode;
	struct pseudo_text_file *pf = (struct pseudo_text_file *)file->f_private;
	if (pf) {
		kfree(pf->data);
		kfree(pf);
		file->f_private = NULL;
	}
	return 0;
}

static struct file_operations pseudo_text_fops = {
	.read = pseudo_text_read,
	.release = pseudo_text_release,
};

static struct file *open_proc_mounts_file(void)
{
	static const char mounts[] = "/dev/root / ext4 rw 0 0\n";
	struct pseudo_text_file *pf;
	struct file *f;

	pf = kzalloc(sizeof(*pf));
	if (!pf)
		return PTR(-ENOMEM);
	pf->len = sizeof(mounts) - 1;
	pf->data = kmalloc(pf->len);
	if (!pf->data) {
		kfree(pf);
		return PTR(-ENOMEM);
	}
	memcpy(pf->data, mounts, pf->len);

	f = file_alloc();
	if (IS_ERR(f)) {
		kfree(pf->data);
		kfree(pf);
		return f;
	}

	f->f_op = &pseudo_text_fops;
	f->f_private = pf;
	f->f_mode = S_IFCHR;
	f->f_flags = O_RDONLY;
	return f;
}

static struct file *open_pseudo_text_file(const char *text, size_t len)
{
	struct pseudo_text_file *pf;
	struct file *f;

	pf = kzalloc(sizeof(*pf));
	if (!pf)
		return PTR(-ENOMEM);
	pf->len = len;
	pf->data = kmalloc(len);
	if (!pf->data) {
		kfree(pf);
		return PTR(-ENOMEM);
	}
	memcpy(pf->data, text, len);

	f = file_alloc();
	if (IS_ERR(f)) {
		kfree(pf->data);
		kfree(pf);
		return f;
	}

	f->f_op = &pseudo_text_fops;
	f->f_private = pf;
	f->f_mode = S_IFCHR;
	f->f_flags = O_RDONLY;
	return f;
}

static int pseudo_dir_readdir(struct file *file, struct dirent *buf, size_t count)
{
	static const char *entries[] = { ".", ".." };
	size_t written = 0;
	int idx = (int)file->f_pos;

	while (idx < 2 && written + sizeof(struct dirent) <= count) {
		struct dirent *d = (struct dirent *)((char *)buf + written);
		memset(d, 0, sizeof(*d));
		d->d_ino = 1;
		d->d_off = idx + 1;
		d->d_reclen = sizeof(struct dirent);
		d->d_type = 2;
		strcpy(d->d_name, entries[idx]);
		written += sizeof(struct dirent);
		idx++;
	}

	file->f_pos = idx;
	return (int)written;
}

static loff_t pseudo_dir_llseek(struct file *file, loff_t offset, int whence)
{
	switch (whence) {
	case SEEK_SET:
		if (offset < 0)
			return -EINVAL;
		file->f_pos = offset;
		return file->f_pos;
	case SEEK_CUR:
		if (file->f_pos + offset < 0)
			return -EINVAL;
		file->f_pos += offset;
		return file->f_pos;
	default:
		return -EINVAL;
	}
}

static struct file_operations pseudo_dir_fops = {
	.readdir = pseudo_dir_readdir,
	.llseek = pseudo_dir_llseek,
};

static struct file *open_proc_dir_file(void)
{
	struct file *f = file_alloc();
	if (IS_ERR(f))
		return f;

	f->f_op = &pseudo_dir_fops;
	f->f_mode = S_IFDIR;
	f->f_flags = O_RDONLY;
	return f;
}

static struct file *open_dev_rtc_file(void)
{
	static const char rtc_text[] = "Thu Jan  1 00:00:00 1970\n";
	return open_pseudo_text_file(rtc_text, sizeof(rtc_text) - 1);
}

static struct dentry *resolve_dirfd_base(struct proc *p, long dirfd,
					 const char *path,
					 struct file **dir_file_out)
{
	if (dir_file_out)
		*dir_file_out = NULL;

	if (!path || path[0] == '/')
		return NULL;

	if (dirfd == AT_FDCWD)
		return p->pwd;

	struct file *dir_file = fd_get(p->fd_table, (int)dirfd);
	if (!dir_file)
		return p->pwd;

	if (!dir_file->f_dentry) {
		file_put(dir_file);
		return p->pwd;
	}

	if (dir_file_out)
		*dir_file_out = dir_file;

	return dir_file->f_dentry;
}

/* ============================================================
 * I/O: read / write
 * ============================================================ */

uintptr_t sys_write(int fd, uintptr_t buf, size_t len)
{
	struct proc *p = curproc();
	size_t done = 0;

	if (len == 0)
		return 0;

	/* 即使 fd==1/2，如果已被 dup 重定向到真实文件则走 file_write */
	if (fd == 1 || fd == 2) {
		struct file *f = fd_get(p->fd_table, fd);
		if (f) {
			char tmp[512];
			while (done < len) {
				size_t n = len - done;
				if (n > sizeof(tmp))
					n = sizeof(tmp);
				if (copyin(p->pagetable, tmp, buf + done, n) < 0) {
					file_put(f);
					return done > 0 ? done : -EFAULT;
				}
				ssize_t ret = file_write(f, tmp, n);
				if (ret < 0) {
					file_put(f);
					return done > 0 ? done : ret;
				}
				done += (size_t)ret;
				if ((size_t)ret < n) {
					file_put(f);
					return done;
				}
			}
			file_put(f);
			return done;
		}

		while (done < len) {
				char tmp[512];
				size_t n = len - done;
				if (n > sizeof(tmp))
				n = sizeof(tmp);
			if (copyin(p->pagetable, tmp, buf + done, n) < 0)
				return done > 0 ? done : -EFAULT;
			for (size_t i = 0; i < n; i++)
				sbi_console_putchar(tmp[i]);
			done += n;
		}
		return done;
	}

	/* file fd */
	struct file *f = fd_get(p->fd_table, fd);
	if (!f)
		return -EBADF;

	while (done < len) {
		char tmp[512];
		size_t n = len - done;
		if (n > sizeof(tmp))
			n = sizeof(tmp);

		if (copyin(p->pagetable, tmp, buf + done, n) < 0) {
			file_put(f);
			return done > 0 ? done : -EFAULT;
		}

		ssize_t ret = file_write(f, tmp, n);
		if (ret < 0) {
			file_put(f);
			return done > 0 ? done : ret;
		}
		done += (size_t)ret;
		if ((size_t)ret < n) {
			file_put(f);
			return done;
		}
	}

	file_put(f);
	return done;
}

uintptr_t sys_read(int fd, uintptr_t buf, size_t len)
{
	struct proc *p = curproc();
	size_t done = 0;

	if (len == 0)
		return 0;

	struct file *f = fd_get(p->fd_table, fd);
	if (!f)
		return -EBADF;

	while (done < len) {
		char tmp[512];
		size_t n = len - done;
		if (n > sizeof(tmp))
			n = sizeof(tmp);
		ssize_t ret = file_read(f, tmp, n);
		if (ret < 0) {
			file_put(f);
			return done > 0 ? done : ret;
		}
		if (ret == 0)
			break;
		if (copyout(p->pagetable, buf + done, tmp, ret) < 0) {
			file_put(f);
			return done > 0 ? done : -EFAULT;
		}
		done += (size_t)ret;
		if ((size_t)ret < n)
			break;
	}

	file_put(f);
	return done;
}

/* execve 失败后用户空间已被销毁，不能 return 到用户态，
   必须像 sys_exit 一样终止当前进程 */
static void execve_die(int status)
{
	struct proc *p = curproc();
	p->exit_code = status;
	p->state = PROC_ZOMBIE;
	if (p->fd_table) {
		fd_table_free(p->fd_table);
		p->fd_table = NULL;
	}
	if (p->parent)
		wait_queue_wakeup_one(&p->parent->child_wait);
	sched_yield();
	panic("unreachable");
}

/* ============================================================
 * Exit / Shutdown / Reboot
 * ============================================================ */

uintptr_t sys_exit(int status)
{
	struct proc *p = curproc();
	if (p->vm_shared && thread_exit_debug_budget > 0) {
		infof("thread-exit: pid=%d tgid=%d status=%d clear_child_tid=%p sp=%p tp=%p",
		      p->pid, p->tgid, status, p->clear_child_tid,
		      p->tf ? p->tf->sp : 0, p->tf ? p->tf->tp : 0);
		thread_exit_debug_budget--;
	}
	p->exit_code = status;
	p->futex_uaddr = 0;

	/* 关闭所有 fd：使管道写端及时释放，避免读端永久阻塞等 EOF */
	if (p->fd_table) {
		fd_table_free(p->fd_table);
		p->fd_table = NULL;
	}

	if (p->clear_child_tid) {
		int zero = 0;
		if (p->vm_shared && p->tgid >= 7700 &&
		    thread_exit_debug_budget > 0) {
			infof("thread-exit-clear: pid=%d tgid=%d clear_child_tid=%p",
			      p->pid, p->tgid, p->clear_child_tid);
			thread_exit_debug_budget--;
		}
		copyout(p->pagetable, (uintptr_t)p->clear_child_tid,
			(char *)&zero, sizeof(zero));
		futex_wake_addr((uintptr_t)p->clear_child_tid);
	}

	p->state = PROC_ZOMBIE;
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
	if (p->vm_shared && tidaddr_debug_budget > 0) {
		infof("set_tid_address: pid=%d tgid=%d tidptr=%p old=%p tp=%p",
		      p->pid, p->tgid, tidptr, p->clear_child_tid,
		      p->tf ? p->tf->tp : 0);
		tidaddr_debug_budget--;
	}
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

uintptr_t sys_clock_gettime(int clkid, uintptr_t tp)
{
	struct proc *p = curproc();
	(void)clkid;

	struct kernel_timespec ts;
	u64 ns = cputime_to_ns(r_time());
	ts.tv_sec = (long)(ns / 1000000000ULL);
	ts.tv_nsec = (long)(ns % 1000000000ULL);

	if (copyout(p->pagetable, tp, (char *)&ts, sizeof(ts)) < 0)
		return -EFAULT;
	return 0;
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
	struct file *dir_file = NULL;
	if (copyinstr(p->pagetable, path, pathname, sizeof(path)) < 0)
		return -EFAULT;

	if (strcmp(path, "/proc/mounts") == 0) {
		struct file *f = open_proc_mounts_file();
		int fd;
		if (IS_ERR(f))
			return (uintptr_t)PTR_ERR(f);
		fd = fd_alloc(p->fd_table);
		if (fd < 0) {
			file_put(f);
			return -EMFILE;
		}
		if (fd_install(p->fd_table, fd, f) < 0) {
			file_put(f);
			return -EBADF;
		}
		return fd;
	}
	if (strcmp(path, "/proc/meminfo") == 0) {
		static const char meminfo[] =
			"MemTotal:       131072 kB\n"
			"MemFree:         65536 kB\n"
			"MemAvailable:    65536 kB\n"
			"Buffers:             0 kB\n"
			"Cached:              0 kB\n"
			"SwapCached:          0 kB\n"
			"SwapTotal:           0 kB\n"
			"SwapFree:            0 kB\n";
		struct file *f = open_pseudo_text_file(meminfo, sizeof(meminfo) - 1);
		int fd;
		if (IS_ERR(f))
			return (uintptr_t)PTR_ERR(f);
		fd = fd_alloc(p->fd_table);
		if (fd < 0) {
			file_put(f);
			return -EMFILE;
		}
		if (fd_install(p->fd_table, fd, f) < 0) {
			file_put(f);
			return -EBADF;
		}
		return fd;
	}
	if (strcmp(path, "/proc") == 0) {
		struct file *f = open_proc_dir_file();
		int fd;
		if (IS_ERR(f))
			return (uintptr_t)PTR_ERR(f);
		fd = fd_alloc(p->fd_table);
		if (fd < 0) {
			file_put(f);
			return -EMFILE;
		}
		if (fd_install(p->fd_table, fd, f) < 0) {
			file_put(f);
			return -EBADF;
		}
		return fd;
	}
	if (strcmp(path, "/dev/misc/rtc") == 0) {
		struct file *f = open_dev_rtc_file();
		int fd;
		if (IS_ERR(f))
			return (uintptr_t)PTR_ERR(f);
		fd = fd_alloc(p->fd_table);
		if (fd < 0) {
			file_put(f);
			return -EMFILE;
		}
		if (fd_install(p->fd_table, fd, f) < 0) {
			file_put(f);
			return -EBADF;
		}
		return fd;
	}

	/* 解析 base dentry: 绝对路径用 root，否则看 dirfd */
	struct dentry *base =
		resolve_dirfd_base(p, (long)dirfd, path, &dir_file);
	/* Normalize O_CREATE(0x40) -> O_CREAT(0x100) for all callers */
	u32 norm_flags = (u32)flags;
	if (norm_flags & 0x40) norm_flags |= O_CREAT;
	struct file *f = vfs_open_cwd(path, norm_flags, base);
	if (dir_file)
		file_put(dir_file);
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
		/* 映射 ext4 file_type -> Linux d_type */
			u8 ltype;
			switch (de->d_type) {
			case 1: ltype = 8;  break; /* EXT4_FT_REG_FILE -> DT_REG */
			case 2: ltype = 4;  break; /* EXT4_FT_DIR     -> DT_DIR */
			case 7: ltype = 10; break; /* EXT4_FT_SYMLINK -> DT_LNK */
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

	/* 设置上下文：forkret -> usertrapret -> 用户态 */
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
 * sys_clone - 创建子进程（支持 CLONE_VM 共享页表 + 指定子进程栈）
 * RISC-V ABI: clone(flags, child_stack, ptid, tls, ctid)
 * 当 CLONE_VM 置位时，父子共享页表（线程风格）
 * child_stack 指定子进程的初始 sp（不为 0 时使用，否则继承父进程 sp）
 */
uintptr_t sys_clone(uintptr_t flags, uintptr_t child_stack, uintptr_t ptid,
		    uintptr_t tls, uintptr_t ctid)
{
	struct proc *parent = curproc();
	struct proc *child;
	bool share_vm = (flags & CLONE_VM) != 0;
	u64 child_entry[2] = { 0, 0 };

	/*
	 * pthread_join 不走 wait4；对 CLONE_THREAD 线程在父线程再次
	 * 创建新线程前主动回收已经退出的线程内核对象，避免 proc/kstack/
	 * pagetable 长时间泄漏后把后续线程创建路径拖坏。
	 */
	if (flags & CLONE_THREAD)
		reap_zombie_threads(parent);
	child = alloc_proc();
	if (!child)
		return -ENOMEM;

	child->pid = alloc_pid();
	child->tgid = (flags & CLONE_THREAD) ? parent->tgid : child->pid;
	child->parent = parent;

	child->pagetable = uvmcreate();
	if (!child->pagetable)
		goto fail;

	if ((share_vm ? uvmshare_tree(parent->pagetable, child->pagetable)
		      : uvmcopy_tree(parent->pagetable, child->pagetable)) < 0) {
		goto fail_freept;
	}
	child->vm_shared = share_vm;

	fd_table_free(child->fd_table);
	/*
	 * Linux CLONE_FILES 语义是共享同一个 files_struct，并配套引用计数。
	 * 当前内核没有 fd_table 共享/引用计数机制，直接别名会在子进程
	 * exec/exit 时把父进程的 fd 表一起释放。
	 * 先统一退化为 fork 风格复制，保证 shell/BusyBox 进程树稳定。
	 */
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

	/* 子进程返回 0 */
	child->tf->a0 = 0;

	if (child_stack >= PAGE_SIZE && child_stack < USER_TOP) {
		child->tf->sp = child_stack;
	}
	if (flags & CLONE_PARENT_SETTID) {
		int tid = child->pid;
		if (copyout(parent->pagetable, ptid, (char *)&tid, sizeof(tid)) < 0)
			goto fail_freetf;
	}
	if (flags & CLONE_SETTLS)
		child->tf->tp = tls;
	if ((flags & CLONE_CHILD_SETTID) &&
	    ctid >= PAGE_SIZE && ctid < USER_TOP) {
		int tid = child->pid;
		if (copyout(child->pagetable, ctid, (char *)&tid, sizeof(tid)) < 0)
			goto fail_freetf;
	}
	if (flags & CLONE_CHILD_CLEARTID)
		child->clear_child_tid = (int *)ctid;
	if ((flags & CLONE_THREAD) && child->tgid >= 7700 &&
	    clone_debug_budget > 0) {
		(void)copyin(parent->pagetable, (char *)child_entry, child_stack,
			     sizeof(child_entry));
		infof("clone-debug: parent=%d child=%d tgid=%d epc=%p ra=%p flags=%lx sp=%p tls=%p ptid=%p ctid=%p stack0=%p stack1=%p",
		      parent->pid, child->pid, child->tgid, child->tf->epc,
		      child->tf->ra, flags, child_stack, tls, ptid, ctid,
		      child_entry[0], child_entry[1]);
		clone_debug_budget--;
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

	/* 设置上下文：forkret -> usertrapret -> 用户态 */
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
#define PT_INTERP 3
#define PT_TLS  4
#define PT_PHDR 6
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* Auxiliary vector types (Linux ABI) */
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9
#define AT_PHENT  4
#define AT_RANDOM 25

/* Interpreter base address - far from PIE program (vaddr ~0) */
#define INTERP_BASE  0x400000ULL

/*
 * load_elf - 加载 ELF 文件的 PT_LOAD 段到用户页表
 * @f:         已打开的 ELF 文件
 * @p:         目标进程
 * @ehdr:      已读取的 ELF header
 * @base_addr: 基址偏移（PIE 主程序=0，解释器=INTERP_BASE）
 * @entry_out: [out] 返回入口点虚拟地址
 * @phdr_addr_out: [out] 返回 program headers 在用户态的地址（可 NULL）
 * @phdr_num_out:  [out] 返回 phdr 数量（可 NULL）
 * @max_va_out:    [out] 返回加载的最大虚拟地址（可 NULL）
 * @return: 0 成功，负数错误码
 *
 * 调用者负责释放旧用户空间，以及 file_put(f)
 */
static int load_elf(struct file *f, struct proc *p, elf64_ehdr_t *ehdr,
		    uintptr_t base_addr,
		    uintptr_t *entry_out,
		    uintptr_t *phdr_addr_out, int *phdr_num_out,
		    uintptr_t *max_va_out)
{
	uintptr_t max_va = 0;

	/* 加载 PT_LOAD segments */
	for (int i = 0; i < ehdr->e_phnum; i++) {
		elf64_phdr_t phdr;
		loff_t ph_pos = ehdr->e_phoff + i * ehdr->e_phentsize;
		file_lseek(f, ph_pos, SEEK_SET);
		if (file_read(f, &phdr, sizeof(phdr)) < (ssize_t)sizeof(phdr))
			continue;

		if (phdr.p_type != PT_LOAD)
			continue;

		uintptr_t va = base_addr + PAGE_ALIGN_DOWN(phdr.p_vaddr);
		uintptr_t end = base_addr + PAGE_ALIGN_UP(phdr.p_vaddr + phdr.p_memsz);

		/* 不允许覆盖 TRAMPOLINE / TRAPFRAME / 栈区 */
		if (end > USER_TOP - STACK_PAGES * PAGE_SIZE ||
		    (end > TRAPFRAME && va < TRAPFRAME + PAGE_SIZE)) {
			return -EINVAL;
		}

		if (end > max_va)
			max_va = end;

		/* 分配物理页并映射（先 RWX，后续可根据 p_flags 收紧权限） */
		for (uintptr_t a = va; a < end; a += PAGE_SIZE) {
			uintptr_t pa = (uintptr_t)kzalloc(PAGE_SIZE);
			if (!pa)
				return -ENOMEM;

			if (mappages(p->pagetable, a, pa, 1,
				     PTE_U | PTE_R | PTE_W | PTE_X) != 0) {
				kfree((void *)pa);
				return -ENOMEM;
			}
		}

		/* 从文件读取 segment 内容到用户态 */
		if (phdr.p_filesz > 0) {
			u64 off = 0;
			u64 remaining = phdr.p_filesz;
			uintptr_t dst = base_addr + phdr.p_vaddr;

			file_lseek(f, phdr.p_offset, SEEK_SET);

			while (remaining > 0) {
				char tmp[512];
				u64 chunk = remaining < sizeof(tmp) ? remaining : sizeof(tmp);
				ssize_t rd = file_read(f, tmp, chunk);
				if (rd <= 0)
					break;
				if (copyout(p->pagetable, dst + off, tmp, rd) < 0)
					return -ENOMEM;
				off += rd;
				remaining -= rd;
			}
		}
	}

	if (max_va == 0)
		return -ENOEXEC;

	/* 计算 AT_PHDR 值：program headers 在用户态的地址 */
	if (phdr_addr_out && phdr_num_out) {
		uintptr_t phdr_user_addr = 0;
		int found = 0;

		/* 遍历 PT_LOAD 找到包含 e_phoff 的段 */
		file_lseek(f, ehdr->e_phoff, SEEK_SET);
		for (int i = 0; i < ehdr->e_phnum; i++) {
			elf64_phdr_t ph;
			if (file_read(f, &ph, sizeof(ph)) < (ssize_t)sizeof(ph))
				continue;
			if (ph.p_type == PT_LOAD &&
			    ehdr->e_phoff >= ph.p_offset &&
			    ehdr->e_phoff < ph.p_offset + ph.p_filesz) {
				phdr_user_addr = base_addr + ph.p_vaddr +
						 (ehdr->e_phoff - ph.p_offset);
				found = 1;
				break;
			}
		}
		if (!found)
			phdr_user_addr = base_addr + ehdr->e_phoff;
		/* PT_PHDR 段优先覆盖 */
		for (int i = 0; i < ehdr->e_phnum; i++) {
			elf64_phdr_t ph;
			file_lseek(f, ehdr->e_phoff + i * ehdr->e_phentsize, SEEK_SET);
			if (file_read(f, &ph, sizeof(ph)) < (ssize_t)sizeof(ph))
				break;
			if (ph.p_type == PT_PHDR) {
				phdr_user_addr = base_addr + ph.p_vaddr;
				break;
			}
		}

		*phdr_addr_out = phdr_user_addr;
		*phdr_num_out = ehdr->e_phnum;
	}

	if (entry_out)
		*entry_out = base_addr + ehdr->e_entry;
	if (max_va_out)
		*max_va_out = max_va;

	return 0;
}

static int map_zeroed_user_pages(pagetable_t pt, uintptr_t start,
				 uintptr_t size, int perm)
{
	uintptr_t end = start + PAGE_ALIGN_UP(size);

	for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
		uintptr_t pa = (uintptr_t)kzalloc(PAGE_SIZE);
		if (!pa)
			return -ENOMEM;
		if (mappages(pt, va, pa, 1, perm) != 0) {
			kfree((void *)pa);
			return -ENOMEM;
		}
	}

	return 0;
}

static int copyin_str_array(pagetable_t pt, uintptr_t uarray,
			    char ***out, int max)
{
	if (uarray == 0) { *out = NULL; return 0; }

	/* 第一遍：计数 */
	int n = 0;
	for (int i = 0; i < max; i++) {
		uintptr_t p;
		if (copyin(pt, (char *)&p, uarray + i * 8, 8) < 0) return -EFAULT;
		if (p == 0) break;
		n++;
	}
	if (n == 0) { *out = NULL; return 0; }

	/* 分配指针数组 */
	*out = kmalloc(n * sizeof(char *));
	if (!*out) return -ENOMEM;

	/* 第二遍：复制每个字符串 */
	for (int i = 0; i < n; i++) {
		uintptr_t p;
		copyin(pt, (char *)&p, uarray + i * 8, 8);
		char buf[256];
		ssize_t len = copyinstr(pt, buf, p, sizeof(buf));
		if (len < 0) { len = 0; buf[0] = '\0'; }
		(*out)[i] = kmalloc(len + 1);
		if (!(*out)[i]) {
			for (int j = 0; j < i; j++) kfree((*out)[j]);
			kfree(*out); *out = NULL; return -ENOMEM;
		}
		memcpy((*out)[i], buf, len + 1);
	}
	return n;
}

uintptr_t sys_execve(uintptr_t filename, uintptr_t argv, uintptr_t envp)
{
	struct proc *p = curproc();
	char path[256];

	if (copyinstr(p->pagetable, path, filename, sizeof(path)) < 0)
		return -EFAULT;
	/* 绝对路径用 root，相对路径用 CWD */
	struct file *f = vfs_open_cwd(path, O_RDONLY, path[0] == '/' ? NULL : p->pwd);
	if (IS_ERR(f)) {
		return (uintptr_t)PTR_ERR(f);
	}

	/* 读取头部（ELF 或 shebang 检测） */
	char header[256];
	ssize_t n = file_read(f, header, sizeof(header));

	/* shebang (#!) 处理 */
	char shebang_arg[128] = {0};
	char shebang_script[256] = {0};
	int is_shebang = 0;

	if (n >= 2 && header[0] == '#' && header[1] == '!') {
		char *cp = header + 2;
		while (cp < header + n && (*cp == ' ' || *cp == '\t')) cp++;
		if (cp >= header + n || *cp == '\n' || *cp == '\r') {
			file_put(f);
			return -ENOEXEC;
		}
		char interp[256] = {0};
		int i = 0;
		while (cp < header + n && *cp != ' ' && *cp != '\t'
		       && *cp != '\n' && *cp != '\r' && i < 254)
			interp[i++] = *cp++;
		if (i == 0) {
			file_put(f);
			return -ENOEXEC;
		}
		/* 读取可选参数（最多一个） */
		if (cp < header + n && (*cp == ' ' || *cp == '\t')) {
			while (cp < header + n && (*cp == ' ' || *cp == '\t')) cp++;
			i = 0;
			while (cp < header + n && *cp != '\n' && *cp != '\r' && i < 126)
				shebang_arg[i++] = *cp++;
		}
		memcpy(shebang_script, path, sizeof(shebang_script));
		is_shebang = 1;

		/* 关闭脚本，打开解释器 ELF */
		file_put(f);
		memcpy(path, interp, sizeof(interp));
		f = vfs_open_cwd(path, O_RDONLY, path[0] == '/' ? NULL : p->pwd);
		if (IS_ERR(f)) {
			/* shebang 解释器不存在时回退到脚本所在目录 */
			const char *bname = path;
			const char *slash = strrchr(path, '/');
			if (slash) bname = slash + 1;
			slash = strrchr(shebang_script, '/');
			if (slash) {
				char fallback[256];
				int n = snprintf(fallback, sizeof(fallback),
						 "%.*s%s",
						 (int)(slash - shebang_script + 1),
						 shebang_script, bname);
				if (n > 0 && (size_t)n < sizeof(fallback))
					f = vfs_open_cwd(fallback, O_RDONLY, NULL);
			}
		}
		if (IS_ERR(f))
			return (uintptr_t)PTR_ERR(f);

		n = file_read(f, header, sizeof(header));
	}

	/* 校验 ELF magic */
	elf64_ehdr_t ehdr;
	if (n < (ssize_t)sizeof(ehdr) ||
	    header[0] != 0x7f || header[1] != 'E' ||
	    header[2] != 'L' || header[3] != 'F') {
		file_put(f);
		return -ENOEXEC;
	}
	memcpy(&ehdr, header, sizeof(ehdr));

	/* 校验架构：RISC-V = 0xF3 */
	if (ehdr.e_machine != 0xF3) {
		file_put(f);
		return -ENOEXEC;
	}

	/* 扫描 program headers：提取 PT_INTERP 路径 */
	char interp_path[256] = {0};
	{
		for (int i = 0; i < ehdr.e_phnum; i++) {
			elf64_phdr_t phdr;
			loff_t ph_pos = ehdr.e_phoff + i * ehdr.e_phentsize;
			file_lseek(f, ph_pos, SEEK_SET);
			if (file_read(f, &phdr, sizeof(phdr)) < (ssize_t)sizeof(phdr))
				continue;
			if (phdr.p_type == PT_INTERP && phdr.p_filesz > 0 &&
			    phdr.p_filesz < sizeof(interp_path)) {
				file_lseek(f, phdr.p_offset, SEEK_SET);
				if (file_read(f, interp_path, phdr.p_filesz) > 0)
					break;
			}
		}
	}

	/* 从旧地址空间读取 argv / envp（释放前必须先复制到内核） */
	char **saved_argv = NULL, **saved_envp = NULL;
	int saved_argc = 0, saved_envc = 0;
	{
		saved_argc = copyin_str_array(p->pagetable, argv, &saved_argv, 32);
		if (saved_argc < 0) { file_put(f); return saved_argc; }
		saved_envc = copyin_str_array(p->pagetable, envp, &saved_envp, 32);
		if (saved_envc < 0) { file_put(f); return saved_envc; }
	}

	/* 预扫描 PT_TLS 段（在 file_put 之前读取段头和文件内容） */
	uintptr_t tls_memsz = 0, tls_align = 1, tls_filesz = 0;
	char *tls_file_data = NULL;
	{
		for (int i = 0; i < ehdr.e_phnum; i++) {
			elf64_phdr_t phdr;
			loff_t ph_pos = ehdr.e_phoff + i * ehdr.e_phentsize;
			file_lseek(f, ph_pos, SEEK_SET);
			if (file_read(f, &phdr, sizeof(phdr)) < (ssize_t)sizeof(phdr))
				continue;
			if (phdr.p_type == PT_TLS) {
				tls_memsz = phdr.p_memsz;
				tls_align = phdr.p_align ? phdr.p_align : 1;
				tls_filesz = phdr.p_filesz;
				if (tls_filesz > 0) {
					tls_file_data = kmalloc(tls_filesz);
					if (tls_file_data) {
						file_lseek(f, phdr.p_offset, SEEK_SET);
						file_read(f, tls_file_data, tls_filesz);
					}
				}
				break;
			}
		}
	}

	/* 释放旧的用户空间映射（保留 trampoline 和 trapframe） */
	uvm_free_user_pages(p->pagetable);
	sfence_vma();
	p->brk_end = 0;

	/* 加载主程序 */
	uintptr_t entry = 0, phdr_addr = 0, max_va = 0;
	int phdr_num = 0;
	int rc = load_elf(f, p, &ehdr, 0, &entry, &phdr_addr, &phdr_num, &max_va);
	if (rc != 0) {
		file_put(f);
		execve_die(127);
	}
	file_put(f);

	/* 加载动态链接解释器（如果 PT_INTERP 存在） */
	uintptr_t interp_entry = 0;
	uintptr_t interp_base = 0;
	if (interp_path[0] != '\0') {
		/* 打开解释器 - 先试原始路径，再试 musl 回退路径 */
		struct file *interp_f = vfs_open_cwd(interp_path, O_RDONLY, NULL);
		if (IS_ERR(interp_f)) {
			/* 回退：sdcard 布局将解释器放在 /glibc/lib/ 下 */
			const char *base = interp_path;
			const char *slash = strrchr(interp_path, '/');
			if (slash) base = slash + 1;
			char fallback[256];
			int n = snprintf(fallback, sizeof(fallback),
					 "/glibc/lib/%s", base);
			if (n > 0 && (size_t)n < sizeof(fallback))
				interp_f = vfs_open_cwd(fallback, O_RDONLY, NULL);
		}
		if (IS_ERR(interp_f)) {
			warnf("sys_execve: cannot open interpreter '%s'", interp_path);
			execve_die(127);
		}

		/* 读取解释器 ELF header */
		elf64_ehdr_t interp_ehdr;
		if (file_read(interp_f, &interp_ehdr, sizeof(interp_ehdr)) <
		    (ssize_t)sizeof(interp_ehdr) ||
		    interp_ehdr.e_ident[0] != 0x7f ||
		    interp_ehdr.e_ident[1] != 'E' ||
		    interp_ehdr.e_ident[2] != 'L' ||
		    interp_ehdr.e_ident[3] != 'F') {
			file_put(interp_f);
			warnf("sys_execve: interpreter not a valid ELF");
			execve_die(127);
		}

		uintptr_t interp_max_va = 0;
		rc = load_elf(interp_f, p, &interp_ehdr, INTERP_BASE,
			      &interp_entry, NULL, NULL, &interp_max_va);
		file_put(interp_f);
		if (rc != 0) {
			warnf("sys_execve: failed to load interpreter");
			execve_die(127);
		}

		interp_base = INTERP_BASE;
		if (interp_max_va > max_va)
			max_va = interp_max_va;
	}

	/* 设置 brk_end（主程序加载上限） */
	p->brk_end = max_va;

	/*
	 * 初始化 TLS（Thread Local Storage）— glibc 静态/动态链接程序
	 * 都需要 tp 指向有效的 TCB，否则 stack canary (tp+0x48) 会读到
	 * 垃圾值，触发 __stack_chk_fail → SIGABRT。
	 *
	 * RISC-V TLS ABI: tp → TCB, TLS 变量在 tp 的负方向。
	 * 布局: [TLS vars (memsz, aligned)] [TCB (TLS_TCB_SIZE)]
	 * tp 指向 TCB 起始。
	 */
	uintptr_t tls_tcb = 0;
	if (tls_memsz > 0) {
#define TLS_TCB_SIZE 0x70
		uintptr_t tls_total = PAGE_ALIGN_UP(tls_memsz + TLS_TCB_SIZE);
		uintptr_t tls_base = USER_TOP - STACK_PAGES * PAGE_SIZE - tls_total;
		if (map_zeroed_user_pages(p->pagetable, tls_base, tls_total,
					  PTE_R | PTE_W | PTE_U) == 0) {
			/* tdata 起始：TLS 区域中偏移 (tls_total - memsz - TCB_SIZE) */
			uintptr_t tdata_start = tls_base + tls_total - tls_memsz - TLS_TCB_SIZE;
			if (tls_file_data && tls_filesz > 0)
				copyout(p->pagetable, tdata_start,
					tls_file_data, tls_filesz);
			/* tp 指向 TCB 起始，对齐到 tls_align */
			uintptr_t tcb_addr = tls_base + tls_total - TLS_TCB_SIZE;
			tcb_addr &= ~(tls_align - 1);
			tls_tcb = tcb_addr;
		}
#undef TLS_TCB_SIZE
	}

	/* 分配用户栈（使用 walkaddr 检查是否已有映射） */
	uintptr_t stack_bot = USER_TOP - STACK_PAGES * PAGE_SIZE;
	if (walkaddr(p->pagetable, stack_bot) == 0) {
		if (map_zeroed_user_pages(p->pagetable, stack_bot,
					  STACK_PAGES * PAGE_SIZE,
					  PTE_R | PTE_W | PTE_U) != 0) {
			return -ENOMEM;
		}
	}

	/* 决定 argv：shebang 用解释器 argv，否则用用户 argv */
	int final_argc;
	uintptr_t *argv_strs = NULL;	/* 内核堆上存放 argv 字符串地址 */
	int max_argv = 0;
	{
		if (is_shebang) {
			int extra_argc = saved_argc > 1 ? saved_argc - 1 : 0;

			/* shebang argv = [interp, arg?, script, original argv[1..]] */
			final_argc = (shebang_arg[0] ? 3 : 2) + extra_argc;
			argv_strs = kmalloc(final_argc * sizeof(uintptr_t));
			if (!argv_strs) return -ENOMEM;
			argv_strs[0] = (uintptr_t)path;	      /* interpreter path */
			int argi = 1;
			if (shebang_arg[0])
				argv_strs[argi++] = (uintptr_t)shebang_arg;
			argv_strs[argi++] = (uintptr_t)shebang_script;
			for (int i = 1; i < saved_argc; i++)
				argv_strs[argi++] = (uintptr_t)saved_argv[i];
		} else {
			final_argc = saved_argc;
			argv_strs = kmalloc((final_argc + 1) * sizeof(uintptr_t));
			if (!argv_strs) return -ENOMEM;
			for (int i = 0; i < final_argc; i++)
				argv_strs[i] = (uintptr_t)saved_argv[i];
		}
	}

	/* 设置用户栈：
	 * 布局 [低 sp -> 高 address]：
	 *   argc | argv[]+NULL | envp[]+NULL | auxv[] | 随机16B | envp字符串 | argv字符串
	 */
	uintptr_t sp = USER_TOP;

	/* 1) argv 字符串 + envp 字符串（从高地址向下放置） */
	uintptr_t *arg_str_ptrs = kmalloc((final_argc + saved_envc) * sizeof(uintptr_t));
	if (!arg_str_ptrs) { kfree(argv_strs); return -ENOMEM; }

	for (int i = 0; i < final_argc; i++) {
		const char *s = (const char *)argv_strs[i];
		int len = 0; while (s[len]) len++;
		sp -= len + 1;
		if (copyout(p->pagetable, sp, (char *)s, len + 1) < 0)
			{ kfree(argv_strs); kfree(arg_str_ptrs); return -ENOMEM; }
		arg_str_ptrs[i] = sp;
	}
	for (int i = 0; i < saved_envc; i++) {
		const char *s = saved_envp[i];
		int len = 0; while (s[len]) len++;
		sp -= len + 1;
		if (copyout(p->pagetable, sp, (char *)s, len + 1) < 0)
			{ kfree(argv_strs); kfree(arg_str_ptrs); return -ENOMEM; }
		arg_str_ptrs[final_argc + i] = sp;
	}

	/* 2) 写入 16 字节随机数据（AT_RANDOM 指向此处） */
	sp -= 16;
	{
		char rand_buf[16] = {0}; /* canary=0 避免 glibc 内部 amoswap 覆盖检测 */
		if (copyout(p->pagetable, sp, rand_buf, 16) < 0)
			{ kfree(argv_strs); kfree(arg_str_ptrs); return -ENOMEM; }
	}
	uintptr_t rand_addr = sp;

	/* 3) 写入 auxv entries（每个 16 字节：type(u64) + value(u64)） */
	{
		uintptr_t aux_start;
		if (interp_path[0] != '\0') {
			/* 7 个 auxv + AT_NULL = 8 个 entry, 每个 16B */
			sp -= 8 * 16;
			aux_start = sp;
			uintptr_t apos = sp;
			/* AT_PHDR */
			u64 at_phdr_val[2] = { AT_PHDR, phdr_addr };
			copyout(p->pagetable, apos, (char *)at_phdr_val, 16); apos += 16;
			/* AT_PHNUM */
			u64 at_phnum_val[2] = { AT_PHNUM, (u64)phdr_num };
			copyout(p->pagetable, apos, (char *)at_phnum_val, 16); apos += 16;
			/* AT_PHENT */
			u64 at_phent_val[2] = { AT_PHENT, (u64)sizeof(elf64_phdr_t) };
			copyout(p->pagetable, apos, (char *)at_phent_val, 16); apos += 16;
			/* AT_PAGESZ */
			u64 at_pagesz_val[2] = { AT_PAGESZ, PAGE_SIZE };
			copyout(p->pagetable, apos, (char *)at_pagesz_val, 16); apos += 16;
			/* AT_BASE */
			u64 at_base_val[2] = { AT_BASE, interp_base };
			copyout(p->pagetable, apos, (char *)at_base_val, 16); apos += 16;
			/* AT_ENTRY */
			u64 at_entry_val[2] = { AT_ENTRY, entry };
			copyout(p->pagetable, apos, (char *)at_entry_val, 16); apos += 16;
			/* AT_RANDOM */
			u64 at_random_val[2] = { AT_RANDOM, rand_addr };
			copyout(p->pagetable, apos, (char *)at_random_val, 16); apos += 16;
			/* AT_NULL */
			u64 at_null_val[2] = { AT_NULL, 0 };
			copyout(p->pagetable, apos, (char *)at_null_val, 16);
		} else {
			/* 静态链接：AT_PHDR + AT_PHNUM + AT_PHENT + AT_PAGESZ + AT_RANDOM */
			sp -= 6 * 16;
			(void)aux_start;
			uintptr_t apos = sp;
			u64 at_phdr_val[2] = { AT_PHDR, phdr_addr };
			copyout(p->pagetable, apos, (char *)at_phdr_val, 16); apos += 16;
			u64 at_phnum_val[2] = { AT_PHNUM, (u64)phdr_num };
			copyout(p->pagetable, apos, (char *)at_phnum_val, 16); apos += 16;
			u64 at_phent_val[2] = { AT_PHENT, (u64)sizeof(elf64_phdr_t) };
			copyout(p->pagetable, apos, (char *)at_phent_val, 16); apos += 16;
			u64 at_pagesz_val[2] = { AT_PAGESZ, PAGE_SIZE };
			copyout(p->pagetable, apos, (char *)at_pagesz_val, 16); apos += 16;
			u64 at_random_val[2] = { AT_RANDOM, rand_addr };
			copyout(p->pagetable, apos, (char *)at_random_val, 16); apos += 16;
			u64 at_null_val[2] = { AT_NULL, 0 };
			copyout(p->pagetable, apos, (char *)at_null_val, 16);
		}
	}

	/* 4) envp 指针数组 + NULL */
	{
		int envc = saved_envc;
		sp -= (envc + 1) * 8;
		uintptr_t apos = sp;
		for (int i = 0; i < envc; i++) {
			uintptr_t p_str = arg_str_ptrs[final_argc + i];
			if (copyout(p->pagetable, apos, (char *)&p_str, 8) < 0)
				{ kfree(argv_strs); kfree(arg_str_ptrs); return -ENOMEM; }
			apos += 8;
		}
		{ u64 null_val = 0;
		  copyout(p->pagetable, apos, (char *)&null_val, 8); }
	}

	/* 5) argv 指针数组 + NULL */
	{
		sp -= (final_argc + 1) * 8;
		uintptr_t apos = sp;
		for (int i = 0; i < final_argc; i++) {
			if (copyout(p->pagetable, apos, (char *)&arg_str_ptrs[i], 8) < 0)
				{ kfree(argv_strs); kfree(arg_str_ptrs); return -ENOMEM; }
			apos += 8;
		}
		{ u64 null_val = 0;
		  copyout(p->pagetable, apos, (char *)&null_val, 8); }
	}

	/* 6) argc */
	sp -= 8;
	{
		u64 argc_val = final_argc;
		if (copyout(p->pagetable, sp, (char *)&argc_val, 8) < 0)
			{ kfree(argv_strs); kfree(arg_str_ptrs); return -ENOMEM; }
		p->tf->a0 = final_argc;
	}

	kfree(argv_strs);
	kfree(arg_str_ptrs);

	/* 设置 trapframe - 动态链接时跳转解释器，否则跳主程序 */
	if (interp_path[0] != '\0') {
		p->tf->epc = interp_entry; /* musl _dlstart */
	} else {
		p->tf->epc = entry;
	}
	p->tf->sp = sp; /* 指向栈上的 argc */
	p->tf->tp = tls_tcb; /* TLS TCB（如有 PT_TLS，否则 0） */

	kfree(tls_file_data);
	return 0;
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
					child->parent = NULL;
					list_del(&child->sibling);
					INIT_LIST_HEAD(&child->sibling);
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
	return curproc()->tgid;
}

uintptr_t sys_getppid(void)
{
	struct proc *p = curproc();
	return p->parent ? p->parent->pid : 0;
}

uintptr_t sys_statfs(uintptr_t path, uintptr_t buf)
{
	struct proc *p = curproc();
	char kpath[256];
	struct statfs st;

	if (copyinstr(p->pagetable, kpath, path, sizeof(kpath)) < 0)
		return -EFAULT;
	int ret = vfs_statfs(kpath, &st);
	if (ret < 0)
		return ret;
	if (copyout(p->pagetable, buf, (char *)&st, sizeof(st)) < 0)
		return -EFAULT;
	return 0;
}

uintptr_t sys_fstatfs(int fd, uintptr_t buf)
{
	struct proc *p = curproc();
	struct file *f = fd_get(p->fd_table, fd);
	struct statfs st;
	int ret;

	if (!f)
		return -EBADF;
	if (!f->f_dentry || !f->f_dentry->d_inode || !f->f_dentry->d_inode->i_sb) {
		file_put(f);
		return -EINVAL;
	}

	struct super_block *sb = f->f_dentry->d_inode->i_sb;
	if (!sb->s_op || !sb->s_op->statfs) {
		file_put(f);
		return -ENOSYS;
	}

	ret = sb->s_op->statfs(sb);
	if (ret < 0) {
		file_put(f);
		return ret;
	}

	st.f_type = 0;
	st.f_bsize = sb->s_blocksize;
	st.f_blocks = 0;
	st.f_bfree = 0;
	st.f_bavail = 0;
	st.f_files = 0;
	st.f_ffree = 0;
	st.f_fsid = sb->s_dev;
	st.f_namelen = NAME_MAX;
	st.f_frsize = sb->s_blocksize;
	st.f_flags = sb->s_flags;
	file_put(f);

	if (copyout(p->pagetable, buf, (char *)&st, sizeof(st)) < 0)
		return -EFAULT;
	return 0;
}

/* ============================================================
 * Memory: brk / mmap / munmap / mprotect
 * ============================================================ */

uintptr_t sys_brk(uintptr_t addr)
{
	struct proc *p = curproc();
	struct proc *brk_owner = shared_vm_owner(p);
	uintptr_t cur_brk;

	shared_vm_lock(brk_owner);
	cur_brk = brk_owner->brk_end;
	if (brk_debug_budget > 0 && p->tgid >= 7700) {
		infof("brk-debug: pid=%d tgid=%d req=%p cur=%p shared=%d",
		      p->pid, p->tgid, addr, cur_brk, p->vm_shared ? 1 : 0);
		brk_debug_budget--;
	}

	if (addr == 0) {
		/* CLONE_VM 线程必须观察共享地址空间拥有者的 program break。 */
		uintptr_t ret = cur_brk == 0 ? (1ULL << 20) : cur_brk;
		shared_vm_unlock(brk_owner);
		return ret; /* 默认起始 1MB */
	}

	/* 首次设置 brk，从默认起始地址开始 */
	if (cur_brk == 0) {
		brk_owner->brk_end = (1ULL << 20);
		cur_brk = brk_owner->brk_end;
	}

	if (addr < cur_brk) {
		/* 缩小堆 */
		uintptr_t old = PAGE_ALIGN_UP(cur_brk);
		uintptr_t new = PAGE_ALIGN_UP(addr);
		/* 释放多余页 */
		for (uintptr_t a = new; a < old; a += PAGE_SIZE) {
			uintptr_t pa = walkaddr(p->pagetable, a);
			if (pa) {
				shared_unmap_page(brk_owner, a);
				kfree((void *)pa);
			}
		}
		sync_shared_brk(brk_owner, addr);
		shared_vm_unlock(brk_owner);
		return addr;
	}

	if (addr > cur_brk) {
		/* 扩大堆 */
		uintptr_t old = PAGE_ALIGN_UP(cur_brk);
		uintptr_t new_end = PAGE_ALIGN_UP(addr);
		for (uintptr_t a = old; a < new_end; a += PAGE_SIZE) {
			if (walkaddr(p->pagetable, a) != 0)
				continue;
			uintptr_t pa = (uintptr_t)kzalloc(PAGE_SIZE);
			if (!pa)
				goto out;
			if (shared_map_page(brk_owner, a, pa,
					    PTE_R | PTE_W | PTE_U) != 0) {
				kfree((void *)pa);
				goto out;
			}
		}
		sync_shared_brk(brk_owner, addr);
	}

out:
	cur_brk = brk_owner->brk_end;
	if (brk_debug_budget > 0 && p->tgid >= 7700) {
		infof("brk-debug-ret: pid=%d tgid=%d ret=%p", p->pid, p->tgid,
		      cur_brk);
		brk_debug_budget--;
	}
	shared_vm_unlock(brk_owner);
	return cur_brk;
}

uintptr_t sys_mmap(uintptr_t addr, size_t len, int prot, int flags, int fd, loff_t offset)
{
	struct proc *p = curproc();
	struct proc *vm_owner = shared_vm_owner(p);
	bool prot_none = (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;

	if (len == 0) {
		goto fail;
	}
	if (mmap_debug_budget > 0 && p->tgid >= 7700) {
		infof("mmap-debug: pid=%d tgid=%d addr=%p len=%lx prot=%x flags=%x fd=%d off=%lx brk=%p",
		      p->pid, p->tgid, addr, len, prot, flags, fd, offset,
		      vm_owner->brk_end);
		mmap_debug_budget--;
	}

	shared_vm_lock(vm_owner);

	int pte_flags = prot_none ? PTE_R : PTE_U;
	if (prot & PROT_READ) pte_flags |= PTE_R;
	if (prot & PROT_WRITE) pte_flags |= PTE_R | PTE_W;
	if (prot & PROT_EXEC) pte_flags |= PTE_X;
	size_t sz = PAGE_ALIGN_UP(len);
	uintptr_t start;
	uintptr_t limit = USER_TOP - STACK_PAGES * PAGE_SIZE;

	if (flags & 0x10 /* MAP_FIXED */) {
		start = addr;
	} else {
		uintptr_t hint = 0;
		uintptr_t base = PAGE_ALIGN_UP(vm_owner->brk_end ? vm_owner->brk_end : 0x10000);

		if (addr != 0 && addr < limit)
			hint = find_free_user_range(p->pagetable, addr, sz, limit);
		start = hint ? hint :
			       find_free_user_range(p->pagetable, base, sz, limit);
		if (start == 0)
			goto fail_unlock;
	}

	/* File-backed mmap */
	if (fd >= 0) {
		struct file *f = fd_get(p->fd_table, fd);
		if (!f)
			goto fail;

		loff_t saved_pos = f->f_pos;
		f->f_pos = offset;

		for (uintptr_t a = start; a < start + sz; a += PAGE_SIZE) {
			uintptr_t pa = (uintptr_t)kzalloc(PAGE_SIZE);
			if (!pa) {
				f->f_pos = saved_pos;
				file_put(f);
				shared_vm_unlock(vm_owner);
				return -ENOMEM;
			}
			if (shared_map_page(vm_owner, a, pa, pte_flags) != 0) {
				kfree((void *)pa);
				f->f_pos = saved_pos;
				file_put(f);
				shared_vm_unlock(vm_owner);
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

		if (start + sz > vm_owner->brk_end)
			sync_shared_brk(vm_owner, start + sz);
		if (mmap_debug_budget > 0 && p->tgid >= 7700) {
			infof("mmap-debug-ret: pid=%d tgid=%d start=%p sz=%lx file=1",
			      p->pid, p->tgid, start, sz);
			mmap_debug_budget--;
		}
		shared_vm_unlock(vm_owner);
		return start;
	}

	/* Anonymous mmap */
	for (uintptr_t a = start; a < start + sz; a += PAGE_SIZE) {
		pte_t *pte;
		uintptr_t old_pa = walkaddr(p->pagetable, a);
		if (old_pa != 0) {
			if (flags & 0x10 /* MAP_FIXED */) {
				shared_unmap_page(vm_owner, a);
				kfree((void *)old_pa);
			} else {
				continue;
			}
		} else {
			pte = va2pte(p->pagetable, a, false);
			if (pte && (*pte & PTE_V)) {
				if (flags & 0x10) {
					shared_unmap_page(vm_owner, a);
				} else {
					continue;
				}
			}
		}
		if (prot_none) {
			if (shared_reserve_page(vm_owner, a) != 0)
				goto fail_unlock;
		} else {
			uintptr_t pa = (uintptr_t)kzalloc(PAGE_SIZE);
			if (!pa)
				goto fail_unlock;
			if (shared_map_page(vm_owner, a, pa, pte_flags) != 0) {
				kfree((void *)pa);
				goto fail_unlock;
			}
		}
	}

	if (start + sz > vm_owner->brk_end)
		sync_shared_brk(vm_owner, start + sz);
	if (mmap_debug_budget > 0 && p->tgid >= 7700) {
		infof("mmap-debug-ret: pid=%d tgid=%d start=%p sz=%lx file=0",
		      p->pid, p->tgid, start, sz);
		mmap_debug_budget--;
	}
	shared_vm_unlock(vm_owner);
	return start;

fail_unlock:
	shared_vm_unlock(vm_owner);
	return -ENOMEM;
fail:
	return (uintptr_t)-1; /* MAP_FAILED */
}

uintptr_t sys_munmap(uintptr_t addr, size_t len)
{
	struct proc *vm_owner = shared_vm_owner(curproc());
	struct proc *p = curproc();
	if (munmap_debug_budget > 0 && addr >= 0x100000) {
		infof("munmap-debug: pid=%d tgid=%d shared=%d addr=%p len=%lx sp=%p tp=%p",
		      p->pid, p->tgid, p->vm_shared ? 1 : 0, addr, len,
		      p->tf ? p->tf->sp : 0, p->tf ? p->tf->tp : 0);
		munmap_debug_budget--;
	}
	if (addr == 0 || len == 0)
		return 0;
	shared_vm_lock(vm_owner);
	size_t sz = PAGE_ALIGN_UP(len);
	uintptr_t range_end = addr + sz;
	/* 不允许 unmap TRAMPOLINE/TRAPFRAME/栈区域 */
	if ((addr >= TRAMPOLINE && addr < TRAMPOLINE + PAGE_SIZE) ||
	    (addr >= TRAPFRAME && addr < TRAPFRAME + PAGE_SIZE) ||
	    (range_end > USER_TOP - STACK_PAGES * PAGE_SIZE && addr < USER_TOP)) {
		shared_vm_unlock(vm_owner);
		return -EINVAL;
	}
	for (uintptr_t a = addr; a < range_end; a += PAGE_SIZE) {
		uintptr_t pa = walkaddr(curproc()->pagetable, a);
		pte_t *pte = va2pte(curproc()->pagetable, a, false);
		if (pa) {
			shared_unmap_page(vm_owner, a);
			kfree((void *)pa);
		} else if (pte && (*pte & PTE_MMAP_RESERVED)) {
			shared_unmap_page(vm_owner, a);
		}
	}
	shared_vm_unlock(vm_owner);
	return 0;
}

uintptr_t sys_mprotect(uintptr_t addr, size_t len, int prot)
{
	struct proc *p = curproc();
	struct proc *vm_owner = shared_vm_owner(p);
	uintptr_t start = PAGE_ALIGN_DOWN(addr);
	uintptr_t end;
	bool prot_none = (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;
	int pte_flags = prot_none ? PTE_R : PTE_U;

	if (len == 0)
		return 0;
	if (addr >= USER_TOP)
		return -EINVAL;

	if (prot & PROT_READ)
		pte_flags |= PTE_R;
	if (prot & PROT_WRITE)
		pte_flags |= PTE_R | PTE_W;
	if (prot & PROT_EXEC)
		pte_flags |= PTE_X;
	end = PAGE_ALIGN_UP(addr + len);
	shared_vm_lock(vm_owner);
	for (uintptr_t a = start; a < end; a += PAGE_SIZE) {
		if (shared_protect_page(vm_owner, a, pte_flags) != 0) {
			shared_vm_unlock(vm_owner);
			return -ENOMEM;
		}
	}
	shared_vm_unlock(vm_owner);
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

	/* 为新的 fd 表项增加引用 */
	file_get(f);

	if (fd_install(p->fd_table, newfd, f) < 0) {
		file_put(f);
		file_put(f);
		return -EBADF;
	}

	/* 释放 fd_get 的临时引用；fd_install 持有的那一次由 fd_table_free 释放 */
	file_put(f);

	return newfd;
}

uintptr_t sys_dup3(uintptr_t oldfd, uintptr_t newfd, uintptr_t flags)
{
	(void)flags;
	struct proc *p = curproc();

	if ((int)newfd < 0)
		return -EBADF;

	/* 若 newfd 超过当前表大小则扩展 */
	struct fd_table *fdt = p->fd_table;
	if ((u32)newfd >= fdt->max_fds) {
		u32 new_max = fdt->max_fds * 2;
		while (new_max <= (u32)newfd && new_max < NR_OPEN_MAX)
			new_max *= 2;
		if (new_max > NR_OPEN_MAX)
			new_max = NR_OPEN_MAX;
		if ((u32)newfd >= new_max)
			return -EBADF;

		spinlock_acquire(&fdt->lock);
		struct file **new_fds = kmalloc(sizeof(struct file *) * new_max);
		if (!new_fds) {
			spinlock_release(&fdt->lock);
			return -ENOMEM;
		}
		for (u32 i = 0; i < fdt->max_fds; i++)
			new_fds[i] = fdt->fds[i];
		for (u32 i = fdt->max_fds; i < new_max; i++)
			new_fds[i] = NULL;
		kfree(fdt->fds);
		fdt->fds = new_fds;
		fdt->max_fds = new_max;
		spinlock_release(&fdt->lock);
	}

	if ((int)oldfd == (int)newfd)
		return -EINVAL;

	struct file *f = fd_get(p->fd_table, (int)oldfd);
	if (!f)
		return -EBADF;

	/* 关闭 newfd 上已有的文件 */
	fd_free(p->fd_table, (int)newfd);
	file_get(f);

	if (fd_install(p->fd_table, (int)newfd, f) < 0) {
		file_put(f);
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
		file_put(rf);
		file_put(wf);
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
	struct proc *p = curproc();
	struct file *f;

	/* Linux fcntl commands used by busybox/stdio */
	enum {
		F_DUPFD = 0,
		F_GETFD = 1,
		F_SETFD = 2,
		F_GETFL = 3,
		F_SETFL = 4,
	};

	if (cmd == F_DUPFD || cmd == 1030) {
		/* F_DUPFD (0) / F_DUPFD_CLOEXEC (1030): dup to lowest >= arg */
		int minfd = (int)arg;
		if (minfd < 0) minfd = 0;

		f = fd_get(p->fd_table, (int)fd);
		if (!f) return -EBADF;

		/* 找到 >= minfd 的最小空闲 fd */
		struct fd_table *fdt = p->fd_table;
		int newfd = -EMFILE;
		spinlock_acquire(&fdt->lock);
		for (u32 i = (u32)minfd; i < fdt->max_fds; i++) {
			if (fdt->fds[i] == NULL) {
				newfd = (int)i;
				fdt->fds[i] = FD_RESERVED;
				break;
			}
		}
		spinlock_release(&fdt->lock);

		if (newfd < 0) {
			file_put(f);
			return -EMFILE;
		}

		file_get(f);
		if (fd_install(p->fd_table, newfd, f) < 0) {
			file_put(f);
			file_put(f);
			return -EBADF;
		}
		file_put(f);
		return newfd;
	}

	f = fd_get(p->fd_table, (int)fd);
	if (!f)
		return -EBADF;

	switch (cmd) {
	case F_GETFD:
		file_put(f);
		return 0;
	case F_SETFD:
		file_put(f);
		return 0;
	case F_GETFL:
	{
		uintptr_t flags = f->f_flags | O_LARGEFILE;
		file_put(f);
		return flags;
	}
	case F_SETFL: {
		u32 keep = f->f_flags & ~(O_APPEND | O_NONBLOCK);
		u32 set = (u32)arg & (O_APPEND | O_NONBLOCK);
		f->f_flags = keep | set;
		file_put(f);
		return 0;
	}
	default:
		file_put(f);
		return 0;
	}
}

uintptr_t sys_ioctl(uintptr_t fd, uintptr_t request, uintptr_t arg)
{
	struct proc *p = curproc();
	u32 req = (u32)request;

	if (fd < 0)
		return -EBADF;

	switch (req) {
	case 0x5401: /* TIOCSWINSZ: set window size — ignore */
	case 0x5402: /* TIOCGWINSZ: get window size — stub */
		return 0;
	case 0x540f: /* TCGETS: get terminal attrs — not a terminal */
	case 0x5413: /* TIOCGPGRP: get pgrp — not a terminal */
		return -ENOTTY;
	case (u32)RTC_RD_TIME:
	{
		struct kernel_rtc_time tm = {
			.tm_sec = 0,
			.tm_min = 0,
			.tm_hour = 0,
			.tm_mday = 1,
			.tm_mon = 0,
			.tm_year = 70,
			.tm_wday = 4,
			.tm_yday = 0,
			.tm_isdst = 0,
		};
		if (copyout(p->pagetable, arg, (char *)&tm, sizeof(tm)) < 0)
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

/* ============================================================
 * FS: mkdirat / unlinkat
 * ============================================================ */

uintptr_t sys_mkdirat(uintptr_t dirfd, uintptr_t pathname, uintptr_t mode)
{
	struct proc *p = curproc();
	char path[256];
	struct file *dir_file = NULL;

	if (copyinstr(p->pagetable, path, pathname, sizeof(path)) < 0)
		return -EFAULT;

	struct dentry *base =
		resolve_dirfd_base(p, (long)dirfd, path, &dir_file);
	int ret = vfs_mkdir_cwd(path, (umode_t)mode, base);
	if (dir_file)
		file_put(dir_file);
	return ret;
}

uintptr_t sys_unlinkat(uintptr_t dirfd, uintptr_t pathname, uintptr_t flags)
{
	struct proc *p = curproc();
	char path[256];
	struct file *dir_file = NULL;

	if (copyinstr(p->pagetable, path, pathname, sizeof(path)) < 0)
		return -EFAULT;

	struct dentry *base =
		resolve_dirfd_base(p, (long)dirfd, path, &dir_file);
	int ret;
	if (flags & AT_REMOVEDIR)
		ret = vfs_rmdir_cwd(path, base);
	else
		ret = vfs_unlink_cwd(path, base);
	if (dir_file)
		file_put(dir_file);
	return ret;
}

uintptr_t sys_utimensat(uintptr_t dirfd, uintptr_t pathname, uintptr_t times,
			uintptr_t flags)
{
	struct proc *p = curproc();
	char path[256];
	struct file *dir_file = NULL;
	struct dentry *d;
	struct inode *inode;
	u64 now_sec;

	(void)times;
	(void)flags;

	if (copyinstr(p->pagetable, path, pathname, sizeof(path)) < 0)
		return -EFAULT;

	d = vfs_path_lookup(resolve_dirfd_base(p, (long)dirfd, path, &dir_file),
			   path, LOOKUP_FOLLOW);
	if (dir_file)
		file_put(dir_file);
	if (IS_ERR(d))
		return PTR_ERR(d);

	inode = d->d_inode;
	if (!inode) {
		dentry_put(d);
		return -ENOENT;
	}

	now_sec = cputime_to_ns(r_time()) / 1000000000ULL;
	inode->i_atime = now_sec;
	inode->i_mtime = now_sec;
	inode->i_ctime = now_sec;
	inode_dirty(inode);
	dentry_put(d);
	return 0;
}

uintptr_t sys_renameat(uintptr_t olddirfd, uintptr_t oldpath, uintptr_t newdirfd,
		       uintptr_t newpath)
{
	struct proc *p = curproc();
	char old_kpath[256];
	char new_kpath[256];
	struct file *old_dir_file = NULL;
	struct file *new_dir_file = NULL;
	struct dentry *old_base;
	struct dentry *new_base;
	int ret;

	if (copyinstr(p->pagetable, old_kpath, oldpath, sizeof(old_kpath)) < 0)
		return -EFAULT;
	if (copyinstr(p->pagetable, new_kpath, newpath, sizeof(new_kpath)) < 0)
		return -EFAULT;

	old_base = resolve_dirfd_base(p, (long)olddirfd, old_kpath, &old_dir_file);
	new_base = resolve_dirfd_base(p, (long)newdirfd, new_kpath, &new_dir_file);
	ret = vfs_rename_cwd(old_kpath, new_kpath, old_base, new_base);

	if (old_dir_file)
		file_put(old_dir_file);
	if (new_dir_file)
		file_put(new_dir_file);

	return ret;
}

struct kernel_statx {
	u32 stx_mask;
	u32 stx_blksize;
	u64 stx_attributes;
	u32 stx_nlink;
	u32 stx_uid;
	u32 stx_gid;
	u16 stx_mode;
	u16 __spare0[1];
	u64 stx_ino;
	u64 stx_size;
	u64 stx_blocks;
	u64 stx_attributes_mask;
	struct {
		s64 tv_sec;
		u32 tv_nsec;
		s32 __reserved;
	} stx_atime, stx_btime, stx_ctime, stx_mtime;
	u32 stx_rdev_major;
	u32 stx_rdev_minor;
	u32 stx_dev_major;
	u32 stx_dev_minor;
	u64 stx_mnt_id;
	u32 stx_dio_mem_align;
	u32 stx_dio_offset_align;
	u64 __spare3[12];
};

uintptr_t sys_statx(int dirfd, uintptr_t pathname, uintptr_t flags,
		    uintptr_t mask, uintptr_t statxbuf)
{
	struct proc *p = curproc();
	char path[256];
	struct file *dir_file = NULL;
	struct dentry *base;
	struct dentry *d;
	struct inode *inode;
	struct kernel_statx stx;

	(void)flags;
	(void)mask;

	if (copyinstr(p->pagetable, path, pathname, sizeof(path)) < 0)
		return -EFAULT;

	base = resolve_dirfd_base(p, dirfd, path, &dir_file);
	d = vfs_path_lookup(base, path, 0);
	if (dir_file)
		file_put(dir_file);
	if (IS_ERR(d))
		return PTR_ERR(d);

	inode = d->d_inode;
	if (!inode) {
		dentry_put(d);
		return -ENOENT;
	}

	memset(&stx, 0, sizeof(stx));
	stx.stx_mask = 0x00000fff;
	stx.stx_blksize = 1024;
	stx.stx_nlink = inode->i_nlink;
	stx.stx_uid = inode->i_uid;
	stx.stx_gid = inode->i_gid;
	stx.stx_mode = inode->i_mode;
	stx.stx_ino = inode->i_ino;
	stx.stx_size = inode->i_size;
	stx.stx_blocks = inode->i_blocks;
	stx.stx_atime.tv_sec = inode->i_atime;
	stx.stx_mtime.tv_sec = inode->i_mtime;
	stx.stx_ctime.tv_sec = inode->i_ctime;
	stx.stx_dev_major = MAJOR(inode->i_sb ? inode->i_sb->s_dev : 0);
	stx.stx_dev_minor = MINOR(inode->i_sb ? inode->i_sb->s_dev : 0);
	stx.stx_rdev_major = MAJOR(inode->i_rdev);
	stx.stx_rdev_minor = MINOR(inode->i_rdev);

	dentry_put(d);

	if (copyout(p->pagetable, statxbuf, (char *)&stx, sizeof(stx)) < 0)
		return -EFAULT;

	return 0;
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

/* symlinkat stub - 返回 0 让 busybox 可继续运行 */
uintptr_t sys_symlinkat(uintptr_t target, int dirfd, uintptr_t linkpath)
{
	(void)target;
	(void)dirfd;
	(void)linkpath;
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
	strcpy(u.release, "6.1.0");
	strcpy(u.version, "Yu-NoobKernel riscv64");
	strcpy(u.machine, "riscv64");
	strcpy(u.domainname, "");

	if (copyout(curproc()->pagetable, buf, (char *)&u, sizeof(u)) < 0)
		return -EFAULT;

	return 0;
}

struct kernel_sysinfo {
	long uptime;
	unsigned long loads[3];
	unsigned long totalram;
	unsigned long freeram;
	unsigned long sharedram;
	unsigned long bufferram;
	unsigned long totalswap;
	unsigned long freeswap;
	unsigned short procs;
	unsigned long totalhigh;
	unsigned long freehigh;
	unsigned int mem_unit;
	char _f[20 - 2 * sizeof(long) - sizeof(int)];
};

uintptr_t sys_sysinfo(uintptr_t info)
{
	struct proc *p = curproc();
	struct kernel_sysinfo si;
	u64 sec = cputime_to_ns(r_time()) / 1000000000ULL;

	memset(&si, 0, sizeof(si));
	si.uptime = (long)sec;
	si.totalram = 128UL * 1024UL * 1024UL;
	si.freeram = 64UL * 1024UL * 1024UL;
	si.mem_unit = 1;
	si.procs = 1;

	if (copyout(p->pagetable, info, (char *)&si, sizeof(si)) < 0)
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
	struct file *dir_file = NULL;

	if (copyinstr(p->pagetable, path, pathname, sizeof(path)) < 0)
		return -EFAULT;

	(void)flags;

	/* 通过路径查找 inode */
	struct dentry *base =
		resolve_dirfd_base(p, (long)dirfd, path, &dir_file);
	if (!base && path[0] != '/')
		base = p->pwd ? p->pwd : vfs_get_root();
	struct dentry *d = vfs_path_lookup(base, path, 0);
	if (dir_file)
		file_put(dir_file);
	if (IS_ERR(d))
		return PTR_ERR(d);

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
		/* busy-wait */
	}

	if (rem)
		memset((void *)rem, 0, sizeof(req_ts));

	return 0;
}

uintptr_t sys_clock_nanosleep(int clockid, int flags, uintptr_t req, uintptr_t rem)
{
	(void)clockid;
	(void)flags;
	return sys_nanosleep(req, rem);
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

/* poll/ppoll constants */
#define POLLIN    0x001
#define POLLOUT   0x004
#define POLLERR   0x008
#define POLLHUP   0x010
#define POLLNVAL  0x020
struct pollfd { int fd; short events; short revents; };

/* ppoll — 轮询文件描述符事件（简单实现，支持 POLLIN/POLLOUT） */
uintptr_t sys_ppoll(uintptr_t fds, uintptr_t nfds, uintptr_t tsp, uintptr_t sigmask)
{
	(void)tsp;
	(void)sigmask;
	struct proc *p = curproc();
	if (nfds > 16) nfds = 16; /* safety limit */
	struct pollfd kfds[16];
	if (copyin(p->pagetable, (char *)kfds, fds, sizeof(struct pollfd) * nfds) < 0)
		return -EFAULT;
	int nready = 0;
	for (uintptr_t i = 0; i < nfds; i++) {
		kfds[i].revents = 0;
		if (kfds[i].fd < 0) {
			nready++;
			continue;
		}
		struct file *f = fd_get(p->fd_table, kfds[i].fd);
		if (!f) {
			kfds[i].revents = POLLNVAL;
			nready++;
			continue;
		}
		short events = kfds[i].events;
		if (events & POLLIN)
			kfds[i].revents |= POLLIN;
		if (events & POLLOUT)
			kfds[i].revents |= POLLOUT;
		file_put(f);
		nready++;
	}
	if (copyout(p->pagetable, fds, (char *)kfds, sizeof(struct pollfd) * nfds) < 0)
		return -EFAULT;
	return nready;
}

/* ============================================================
 * 进程/线程信息：gettid, geteuid, tgkill, kill
 * ============================================================ */

uintptr_t sys_gettid(void)
{
	return curproc()->pid;
}

uintptr_t sys_geteuid(void)
{
	return 0; /* root */
}

uintptr_t sys_getegid(void)
{
	return 0;
}

uintptr_t sys_getgid(void)
{
	return 0;
}

uintptr_t sys_getpgid(int pid)
{
	if (pid == 0)
		return curproc()->pid;
	return 0;
}

uintptr_t sys_getsid(int pid)
{
	if (pid == 0)
		return curproc()->pid;
	return 0;
}

uintptr_t sys_tgkill(int tgid, int tid, int sig)
{
	(void)tgid;
	(void)tid;
	/* 如果目标是当前进程且信号是 SIGABRT(6)，直接 exit */
	if (sig == 6) {
		struct proc *p = curproc();
		p->exit_code = -sig;
		p->state = PROC_ZOMBIE;
		if (p->fd_table) {
			fd_table_free(p->fd_table);
			p->fd_table = NULL;
		}
		if (p->parent)
			wait_queue_wakeup_one(&p->parent->child_wait);
		sched_yield();
		panic("unreachable");
	}
	return 0;
}

uintptr_t sys_kill(int pid, int sig)
{
	struct proc *self = curproc();
	struct proc *target;

	if (sig == 0)
		return 0;

	if (pid <= 0)
		return -EINVAL;

	if (pid == self->pid)
		target = self;
	else {
		target = find_child_by_pid(self, pid);
		if (!target && self->parent)
			target = find_child_by_pid(self->parent, pid);
	}

	if (!target)
		return -ESRCH;

	switch (sig) {
	case 9:
	case 15:
		if (!list_empty(&target->runq)) {
			list_del(&target->runq);
			INIT_LIST_HEAD(&target->runq);
		}
		target->exit_code = -sig;
		target->state = PROC_ZOMBIE;
		if (target->fd_table) {
			fd_table_free(target->fd_table);
			target->fd_table = NULL;
		}
		if (target->parent)
			wait_queue_wakeup_one(&target->parent->child_wait);
		if (target == self) {
			sched_yield();
			panic("unreachable");
		}
		return 0;
	default:
		return 0;
	}
}

/* ============================================================
 * FS 辅助：faccessat, readlinkat, sendfile
 * ============================================================ */

uintptr_t sys_faccessat(int dirfd, uintptr_t pathname, int mode, uintptr_t flags)
{
	(void)dirfd;
	(void)pathname;
	(void)mode;
	(void)flags;
	return 0; /* 允许所有访问 */
}

uintptr_t sys_readlinkat(int dirfd, uintptr_t pathname, uintptr_t buf, uintptr_t bufsiz)
{
	(void)dirfd;
	(void)pathname;
	(void)buf;
	(void)bufsiz;
	return -ENOENT; /* 没有符号链接 */
}

uintptr_t sys_sendfile(int out_fd, int in_fd, uintptr_t offset, uintptr_t count)
{
	struct proc *p = curproc();
	struct file *in;
	struct file *out;
	loff_t pos;
	loff_t off_val = 0;
	ssize_t total = 0;
	char buf[512];

	in = fd_get(p->fd_table, in_fd);
	if (!in)
		return -EBADF;
	out = fd_get(p->fd_table, out_fd);
	if (!out) {
		file_put(in);
		return -EBADF;
	}

	if (offset) {
		if (copyin(p->pagetable, (char *)&off_val, offset, sizeof(off_val)) < 0) {
			file_put(out);
			file_put(in);
			return -EFAULT;
		}
		pos = off_val;
	} else {
		pos = in->f_pos;
	}
	while (total < count) {
		size_t chunk = count - total;
		ssize_t nr;
		ssize_t nw;
		if (chunk > sizeof(buf))
			chunk = sizeof(buf);

		nr = in->f_op && in->f_op->read ? in->f_op->read(in, buf, chunk, &pos) :
						  file_read(in, buf, chunk);
		if (nr <= 0)
			break;
		nw = file_write(out, buf, nr);
		if (nw < 0) {
			total = total ? total : nw;
			break;
		}
		total += nw;
		if (nw < nr)
			break;
	}

	if (offset) {
		if (copyout(p->pagetable, offset, (char *)&pos, sizeof(pos)) < 0) {
			file_put(out);
			file_put(in);
			return total > 0 ? (uintptr_t)total : (uintptr_t)-EFAULT;
		}
	} else {
		in->f_pos = pos;
	}

	file_put(out);
	file_put(in);
	return total;
}

uintptr_t sys_syslog(int type, uintptr_t buf, int len)
{
	struct proc *p = curproc();
	static const char kmsg[] = "NoobKernel log buffer\n";
	int n = len;

	if (type != 3 && type != 10)
		return 0;

	if (n > (int)(sizeof(kmsg) - 1))
		n = (int)(sizeof(kmsg) - 1);
	if (n < 0)
		n = 0;
	if (n > 0 && copyout(p->pagetable, buf, (char *)kmsg, n) < 0)
		return -EFAULT;
	return n;
}

/* ============================================================
 * 资源管理：set_robust_list, prlimit64
 * ============================================================ */

uintptr_t sys_set_robust_list(uintptr_t head, uintptr_t len)
{
	(void)head;
	(void)len;
	return 0;
}

uintptr_t sys_futex(uintptr_t uaddr, int futex_op, int val, uintptr_t timeout,
		    uintptr_t uaddr2, int val3)
{
	struct proc *p = curproc();
	struct wait_queue *wq;
	int cur;
	int op = futex_op & FUTEX_CMD_MASK;

	(void)timeout;
	(void)uaddr2;
	(void)val3;

	if ((uaddr & (sizeof(int) - 1)) != 0)
		return -EINVAL;

	futex_init_once();
	wq = futex_bucket(uaddr);
	if (p->tgid >= 7700 && futex_debug_budget > 0) {
		infof("futex-debug: pid=%d tgid=%d op=%x cmd=%d val=%d uaddr=%p clear_child_tid=%p timeout=%p uaddr2=%p val3=%x",
		      p->pid, p->tgid, futex_op, op, val, uaddr,
		      p->clear_child_tid, timeout, uaddr2, val3);
		futex_debug_budget--;
	}

	switch (op) {
	case FUTEX_WAIT:
	case FUTEX_WAIT_BITSET:
		for (;;) {
			if (p->tgid >= 7700 && futex_debug_budget > 0) {
				infof("futex-wait-enter: pid=%d tgid=%d uaddr=%p val=%d clear_child_tid=%p",
				      p->pid, p->tgid, uaddr, val, p->clear_child_tid);
				futex_debug_budget--;
			}
			spinlock_acquire(&wq->lock);
			if (futex_read_u32(p, uaddr, &cur) < 0) {
				spinlock_release(&wq->lock);
				return -EFAULT;
			}
			if (cur != val) {
				spinlock_release(&wq->lock);
				return -EAGAIN;
			}
			p->futex_uaddr = uaddr;
			list_add_tail(&p->runq, &wq->list);
			p->state = PROC_SLEEPING;
			spinlock_release(&wq->lock);
			sched_yield();
			p->futex_uaddr = 0;
			if (futex_read_u32(p, uaddr, &cur) < 0)
				return -EFAULT;
			if (cur != val) {
				if (p->tgid >= 7700 && futex_debug_budget > 0) {
					infof("futex-wait-leave: pid=%d tgid=%d uaddr=%p old=%d new=%d",
					      p->pid, p->tgid, uaddr, val, cur);
					futex_debug_budget--;
				}
				return 0;
			}
		}
	case FUTEX_WAKE:
	case FUTEX_WAKE_BITSET:
		if (val <= 0)
			return 0;
		cur = wait_queue_wakeup_addr(wq, uaddr, val);
		if (p->tgid >= 7700 && futex_debug_budget > 0) {
			infof("futex-wake-done: pid=%d tgid=%d uaddr=%p nr=%d woke=%d waiters=%d val3=%x",
			      p->pid, p->tgid, uaddr, val, cur,
			      wait_queue_count_addr(wq, uaddr), val3);
			futex_debug_budget--;
		}
		return cur;
	default:
		return -ENOSYS;
	}
}

uintptr_t sys_madvise(uintptr_t addr, size_t length, int advice)
{
	(void)addr;
	(void)length;
	(void)advice;
	return 0;
}

uintptr_t sys_prlimit64(int pid, int resource, uintptr_t new_limit, uintptr_t old_limit)
{
	(void)pid;
	(void)resource;
	(void)new_limit;
	(void)old_limit;
	return -ENOSYS;
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

	if (p->tgid >= 7700 && p->pid != p->tgid &&
	    child_syscall_debug_budget > 0) {
		infof("child-syscall: pid=%d tgid=%d nr=%ld epc=%p sp=%p tp=%p a0=%p a1=%p a2=%p",
		      p->pid, p->tgid, n, p->tf->epc, p->tf->sp, p->tf->tp,
		      a0, a1, a2);
		child_syscall_debug_budget--;
	}

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
	syscall_register(SYS_vfork,       (syscall_fn_t)sys_fork);
	syscall_register(SYS_getdents64,  (syscall_fn_t)sys_getdents);
	syscall_register(SYS_statfs,      (syscall_fn_t)sys_statfs);
	syscall_register(SYS_fstatfs,     (syscall_fn_t)sys_fstatfs);

	/* 进程管理 - fork/clone 共用 SYS_clone (220)，由 child_stack==0 区分 */
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
	syscall_register(SYS_madvise,     (syscall_fn_t)sys_madvise);

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
	syscall_register(SYS_renameat,    (syscall_fn_t)sys_renameat);
	syscall_register(SYS_renameat2,   (syscall_fn_t)sys_renameat);
	syscall_register(SYS_utimensat,   (syscall_fn_t)sys_utimensat);
	syscall_register(SYS_symlinkat,   (syscall_fn_t)sys_symlinkat);
	syscall_register(SYS_getcwd,      (syscall_fn_t)sys_getcwd);
	syscall_register(SYS_chdir,       (syscall_fn_t)sys_chdir);
	syscall_register(SYS_fstat,       (syscall_fn_t)sys_fstat);
	syscall_register(SYS_fstatat,     (syscall_fn_t)sys_fstatat);
	syscall_register(SYS_statx,       (syscall_fn_t)sys_statx);
	syscall_register(SYS_mount,       (syscall_fn_t)sys_mount);
	syscall_register(SYS_umount,      (syscall_fn_t)sys_umount);

	/* 信号 (stub) */
	syscall_register(SYS_rt_sigaction, (syscall_fn_t)sys_rt_sigaction);
	syscall_register(SYS_rt_sigprocmask, (syscall_fn_t)sys_rt_sigprocmask);

	/* 杂项 */
	syscall_register(SYS_uname,       (syscall_fn_t)sys_uname);
	syscall_register(SYS_sysinfo,     (syscall_fn_t)sys_sysinfo);
	syscall_register(SYS_gettimeofday, (syscall_fn_t)sys_gettimeofday);
	syscall_register(SYS_clock_gettime, (syscall_fn_t)sys_clock_gettime);
	syscall_register(SYS_clock_nanosleep, (syscall_fn_t)sys_clock_nanosleep);
	syscall_register(SYS_nanosleep,    (syscall_fn_t)sys_nanosleep);
	syscall_register(SYS_times,        (syscall_fn_t)sys_times);
	syscall_register(SYS_sched_yield, (syscall_fn_t)sys_sched_yield);
	syscall_register(SYS_getrandom, (syscall_fn_t)sys_getrandom);
	syscall_register(SYS_ppoll, (syscall_fn_t)sys_ppoll);
	syscall_register(SYS_futex,       (syscall_fn_t)sys_futex);

	/* 新增的 glibc/compat stub */
	syscall_register(SYS_gettid,       (syscall_fn_t)sys_gettid);
	syscall_register(SYS_geteuid,      (syscall_fn_t)sys_geteuid);
	syscall_register(SYS_getegid,      (syscall_fn_t)sys_getegid);
	syscall_register(SYS_getgid,       (syscall_fn_t)sys_getgid);
	syscall_register(SYS_getpgid,      (syscall_fn_t)sys_getpgid);
	syscall_register(SYS_getsid,       (syscall_fn_t)sys_getsid);
	syscall_register(SYS_tgkill,       (syscall_fn_t)sys_tgkill);
	syscall_register(SYS_kill,         (syscall_fn_t)sys_kill);
	syscall_register(SYS_faccessat,    (syscall_fn_t)sys_faccessat);
	syscall_register(SYS_readlinkat,   (syscall_fn_t)sys_readlinkat);
	syscall_register(SYS_sendfile,     (syscall_fn_t)sys_sendfile);
	syscall_register(SYS_syslog,       (syscall_fn_t)sys_syslog);
	syscall_register(SYS_set_robust_list, (syscall_fn_t)sys_set_robust_list);
	syscall_register(SYS_prlimit64,    (syscall_fn_t)sys_prlimit64);

	/* 自定义 */
	syscall_register(SYS_shutdown,    (syscall_fn_t)sys_shutdown);
	syscall_register(SYS_reboot,      (syscall_fn_t)sys_reboot);
}
