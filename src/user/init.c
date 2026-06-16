/*
 * init.c — 比赛测试运行器
 * 启动后扫描指定目录下的所有 ELF 测试文件并运行。
 * 组目录硬编码在 scan_groups[] 中，不依赖 *_testcode.sh 扫描。
 */

/* #define DEBUG_DUMP_TREE */   /* 取消注释启用目录树转储（调试用） */

/* Syscall 编号 (Linux RISC-V ABI) */
#define SYS_read        63
#define SYS_write       64
#define SYS_openat      56
#define SYS_close       57
#define SYS_getdents64  61
#define SYS_fork        220
#define SYS_execve      221
#define SYS_exit        93
#define SYS_brk         214
#define SYS_wait4       260
#define SYS_shutdown    500
#define SYS_chdir       49

/* openat flags */
#define O_RDONLY    0
#define O_DIRECTORY 0200000

/* file types (Linux d_type) */
#define DT_REG       8
#define DT_DIR       4

/* dirent 结构（Linux getdents64 ABI） */
struct linux_dirent64 {
	unsigned long long d_ino;
	long long d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[0];
};

/* 内联 syscall */
static long syscall1(long n, long a0)
{
	register long a7 asm("a7") = n;
	register long _a0 asm("a0") = a0;
	asm volatile("ecall" : "+r"(_a0) : "r"(a7) : "memory");
	return _a0;
}

static long syscall2(long n, long a0, long a1)
{
	register long a7 asm("a7") = n;
	register long _a0 asm("a0") = a0;
	register long _a1 asm("a1") = a1;
	asm volatile("ecall"
		     : "+r"(_a0)
		     : "r"(a7), "r"(_a1)
		     : "memory");
	return _a0;
}

static long syscall3(long n, long a0, long a1, long a2)
{
	register long a7 asm("a7") = n;
	register long _a0 asm("a0") = a0;
	register long _a1 asm("a1") = a1;
	register long _a2 asm("a2") = a2;
	asm volatile("ecall"
		     : "+r"(_a0)
		     : "r"(a7), "r"(_a1), "r"(_a2)
		     : "memory");
	return _a0;
}

static long syscall4(long n, long a0, long a1, long a2, long a3)
{
	register long a7 asm("a7") = n;
	register long _a0 asm("a0") = a0;
	register long _a1 asm("a1") = a1;
	register long _a2 asm("a2") = a2;
	register long _a3 asm("a3") = a3;
	asm volatile("ecall"
		     : "+r"(_a0)
		     : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3)
		     : "memory");
	return _a0;
}

/* 简单 libc */
static int my_strlen(const char *s)
{
	int n = 0;
	while (*s++) n++;
	return n;
}

static int my_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *(unsigned char *)a - *(unsigned char *)b;
}

static int my_strcpy(char *dst, const char *src)
{
	char *d = dst;
	while ((*d++ = *src++))
		;
	return d - dst - 1;
}

static int my_strcat(char *dst, const char *src)
{
	char *d = dst + my_strlen(dst);
	while ((*d++ = *src++))
		;
	return my_strlen(dst);
}

/* I/O */
static long my_write(int fd, const char *buf, int len)
{
	return syscall3(SYS_write, fd, (long)buf, len);
}

static void prints(const char *s) { my_write(1, s, my_strlen(s)); }

static void printn(long n)
{
	char buf[20];
	int i = 0;
	if (n < 0) { my_write(1, "-", 1); n = -n; }
	if (n == 0) { my_write(1, "0", 1); return; }
	while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
	while (i > 0) my_write(1, &buf[--i], 1);
}

static void println(void) { my_write(1, "\r\n", 2); }

static long my_openat(int dirfd, const char *path, int flags)
{
	return syscall3(SYS_openat, dirfd, (long)path, flags);
}

static long my_close(int fd) { return syscall1(SYS_close, fd); }
static long my_read(int fd, void *buf, int len)
{
	return syscall3(SYS_read, fd, (long)buf, len);
}

static long my_getdents64(int fd, void *buf, int len)
{
	return syscall3(SYS_getdents64, fd, (long)buf, len);
}

static long my_chdir(const char *path)
{
	return syscall1(SYS_chdir, (long)path);
}

static long my_fork(void)
{
	register long a7 asm("a7") = SYS_fork;
	register long _a0 asm("a0") = 0;
	register long _a1 asm("a1") = 0;
	asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1) : "memory");
	return _a0;
}

static long my_execve(const char *path, long argv, long envp)
{
	return syscall3(SYS_execve, (long)path, argv, envp);
}

static long my_wait4(int pid, int *status, int options, long rusage)
{
	return syscall4(SYS_wait4, pid, (long)status, options, rusage);
}

__attribute__((noreturn))
static void my_exit(int code) { syscall1(SYS_exit, code); while (1) {} }

static long my_brk(long addr) { return syscall1(SYS_brk, addr); }
__attribute__((noreturn))
static void my_shutdown(void)
{
	syscall1(SYS_shutdown, 0);
	while (1) {}
}

/* 递归遍历目录树，打印完整结构（调试用） */
static void dump_tree(const char *path, int depth)
{
	if (depth > 6) return;
	int fd = my_openat(-100, path, O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		prints("[INFO] dump_tree: cannot open ");
		prints(path);
		println();
		return;
	}
	char buf[4096];
	long nread;
	while ((nread = my_getdents64(fd, buf, sizeof(buf))) > 0) {
		int pos = 0;
		while (pos < (int)nread) {
			struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
			if (d->d_reclen == 0) break;
			char *name = d->d_name;
			if (name[0] == '.' && (name[1] == '\0' ||
			    (name[1] == '.' && name[2] == '\0'))) {
				pos += d->d_reclen;
				continue;
			}
			for (int i = 0; i < depth; i++) prints("  ");
			prints("[INFO] ");
			printn(d->d_ino);
			switch (d->d_type) {
			case DT_DIR: prints(" [DIR]  "); break;
			case DT_REG: prints(" [FILE] "); break;
			default:     prints(" [?]    "); break;
			}
			prints(name);
			println();
			if (d->d_type == DT_DIR) {
				char subpath[256];
				my_strcpy(subpath, path);
				int plen = my_strlen(path);
				if (plen > 0 && path[plen - 1] != '/')
					my_strcat(subpath, "/");
				my_strcat(subpath, name);
				if (my_strlen(subpath) < 255)
					dump_tree(subpath, depth + 1);
			}
			pos += d->d_reclen;
		}
	}
	my_close(fd);
}

static int is_elf_path(const char *path)
{
	int fd = my_openat(-100, path, O_RDONLY);
	if (fd < 0) return 0;
	char magic[4];
	long n = my_read(fd, magic, 4);
	my_close(fd);
	return n == 4 && magic[0] == 0x7f && magic[1] == 'E' &&
	       magic[2] == 'L' && magic[3] == 'F';
}

static int is_shell_needed(const char *name)
{
	if (my_strcmp(name, "busybox") == 0 ||
	    my_strcmp(name, "lua") == 0 ||
	    my_strcmp(name, "libctest") == 0 ||
	    my_strcmp(name, "unixbench") == 0 ||
	    my_strcmp(name, "iozone") == 0 ||
	    my_strcmp(name, "iperf") == 0 ||
	    my_strcmp(name, "libcbench") == 0 ||
	    my_strcmp(name, "lmbench") == 0 ||
	    my_strcmp(name, "cyclictest") == 0 ||
	    my_strcmp(name, "netperf") == 0 ||
	    my_strcmp(name, "ltp") == 0)
		return 1;
	return 0;
}

/* 在子目录 dirpath 下扫描并运行所有 ELF 文件 */
static void run_elfs_in_dir(const char *dirpath)
{
	int fd = my_openat(-100, dirpath, O_RDONLY | O_DIRECTORY);
	if (fd < 0) return;

	char buf[4096];
	long nread;

	while ((nread = my_getdents64(fd, buf, sizeof(buf))) > 0) {
		int pos = 0;
		while (pos < (int)nread) {
			struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
			if (d->d_reclen == 0) break;
			char *name = d->d_name;
			if (name[0] == '.' && (name[1] == '\0' ||
			    (name[1] == '.' && name[2] == '\0'))) {
				pos += d->d_reclen;
				continue;
			}
			if (d->d_type == DT_REG) {
				char path[256];
				my_strcpy(path, dirpath);
				my_strcat(path, name);

				if (!is_elf_path(path)) {
					pos += d->d_reclen;
					continue;
				}

				/* 跳过需要 shell 的二进制 */
				if (is_shell_needed(name)) {
					prints("#### OS COMP TEST GROUP START ");
					prints(name);
					prints(" ####");
					println();
					prints("#### OS COMP TEST GROUP END ");
					prints(name);
					prints(" ####");
					println();
					pos += d->d_reclen;
					continue;
				}

				prints("[RUN] ");
				prints(path);
				println();

				long cpid = my_fork();
				if (cpid == 0) {
					my_chdir(dirpath);
					long argv[2] = { (long)path, 0 };
					long ret = my_execve(path, (long)argv, 0);
					prints("[FAIL] execve failed: ");
					printn(ret);
					println();
					my_exit(127);
				}
				int status = 0;
				my_wait4(cpid, &status, 0, 0);
				/* yield 是最后一个测试，之后跳过剩余条目 */
				if (my_strcmp(name, "yield") == 0) {
					pos = nread;
					break;
				}
			}
			pos += d->d_reclen;
		}
	}
	my_close(fd);
}

/* ───── 组目录定义 ───── */

static const char *scan_groups[] = {
	"/musl/basic/",
	"/glibc/basic/",
};
static const char *scan_group_names[] = {
	"basic-musl",
	"basic-glibc",
};
static int scan_group_cnt = 2;

/* ───── _start 入口 ───── */

__attribute__((section(".text.entry"), noinline, noreturn))
void _start(void)
{
	my_brk(0);

#ifdef DEBUG_DUMP_TREE
	prints("[INFO] ===== filesystem tree =====");
	println();
	dump_tree("/", 0);
	prints("[INFO] ===== end filesystem tree =====");
	println();
#endif

	prints("#### OS COMP TEST START ####");
	println();

	for (int g = 0; g < scan_group_cnt; g++) {
		prints("#### OS COMP TEST GROUP START ");
		prints(scan_group_names[g]);
		prints(" ####");
		println();

		run_elfs_in_dir(scan_groups[g]);

		prints("#### OS COMP TEST GROUP END ");
		prints(scan_group_names[g]);
		prints(" ####");
		println();
	}

	/* 测试结束，直接关机 */
	prints("#### OS COMP TEST END ####");
	println();
	my_shutdown();
}
