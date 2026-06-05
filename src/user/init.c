/*
 * init.c — 比赛测试运行器
 * 启动后扫描根目录找 *_testcode.sh，提取组名，
 * 然后进入对应子目录 (如 /basic/) 扫描并运行所有 ELF 测试
 */

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
static long heap_end = 0;

#define MAX_GROUPS 32
#define MAX_NAME    64

static char groups[MAX_GROUPS][MAX_NAME];
static int group_count = 0;

static int is_test_script(const char *name)
{
	int len = my_strlen(name);
	if (len <= 12) return 0;
	return my_strcmp(name + len - 12, "_testcode.sh") == 0;
}

static int extract_group(const char *name, char *out, int maxlen)
{
	int len = my_strlen(name);
	int glen = len - 12;
	if (glen <= 0 || glen >= maxlen) return -1;
	for (int i = 0; i < glen; i++) out[i] = name[i];
	out[glen] = '\0';
	return 0;
}

/* scan_dir_for_testscripts: scan a directory for *_testcode.sh,
 * add each parent dir to groups[] as the group name */
static void scan_dir_for_scripts(const char *dirpath)
{
	int fd = my_openat(-100, dirpath, O_RDONLY | O_DIRECTORY);
	if (fd < 0) return;

	char buf[4096];
	long nread;
	while ((nread = my_getdents64(fd, buf, sizeof(buf))) > 0) {
		int pos = 0;
		while (pos < (int)nread) {
			struct linux_dirent64 *d =
			    (struct linux_dirent64 *)(buf + pos);
			if (d->d_reclen == 0) break;
			char *name = d->d_name;
			if (name[0] == '.' && (name[1] == '\0' ||
			    (name[1] == '.' && name[2] == '\0'))) {
				pos += d->d_reclen;
				continue;
			}
			if (is_test_script(name) && group_count < MAX_GROUPS) {
				my_strcpy(groups[group_count], dirpath);
				group_count++;
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

/* 在子目录 dirpath 下运行所有 ELF 文件 */
static void run_elfs_in_dir(const char *dirpath)
{
	int fd = my_openat(-100, dirpath, O_RDONLY | O_DIRECTORY);
	if (fd < 0) return;

	char buf[4096];
	long nread;

	/* 循环调用 getdents64 直到读完所有条目 */
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

				/* Skip binaries that need a shell */
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
					/* chdir 使 ./text.txt 等相对路径正确解析 */
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
			}
			pos += d->d_reclen;
		}
	}
	my_close(fd);
}

__attribute__((section(".text.entry"), noinline, noreturn))
void _start(void)
{
	heap_end = my_brk(0);

	prints("#### OS COMP TEST START ####");
	println();

	/* 第1遍：扫描根目录下的 *_testcode.sh（兼容旧格式 fs.img）*/
	{
		int fd = my_openat(-100, "/", O_RDONLY | O_DIRECTORY);
		if (fd < 0) { my_exit(1); }

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
				if (is_test_script(name) && group_count < MAX_GROUPS) {
					extract_group(name, groups[group_count], MAX_NAME);
					group_count++;
				}
				pos += d->d_reclen;
			}
		}
		my_close(fd);
	}

	/* 第2遍：扫描根下一级子目录里的 *_testcode.sh（评测机 sdcard-rv.img 格式）
	   例如 /musl/basic_testcode.sh → group = "/musl/" */
	{
		int fd = my_openat(-100, "/", O_RDONLY | O_DIRECTORY);
		if (fd >= 0) {
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
					if (d->d_type == DT_DIR  /* 4 = DT_DIR */) {
						char subpath[256];
						my_strcpy(subpath, "/");
						my_strcat(subpath, name);
						my_strcat(subpath, "/");
						scan_dir_for_scripts(subpath);
					}
					pos += d->d_reclen;
				}
			}
			my_close(fd);
		}
	}

	/* 处理所有找到的组 */
	for (int g = 0; g < group_count; g++) {
		char *group = groups[g];
		int is_subdir = (group[0] == '/');

		/* TODO: 临时只跑 basic 组 */
		if (my_strcmp(group, "/musl/basic/") != 0 && my_strcmp(group, "basic") != 0)
			continue;

		if (!is_subdir && is_shell_needed(group)) {
			/* 跳过需要 shell 的组，仅输出 markers */
			prints("#### OS COMP TEST GROUP START ");
			prints(group);
			prints(" ####");
			println();
			prints("#### OS COMP TEST GROUP END ");
			prints(group);
			prints(" ####");
			println();
			continue;
		}

		char dirpath[256];
		if (is_subdir) {
			my_strcpy(dirpath, group);
		} else {
			my_strcpy(dirpath, "/");
			my_strcat(dirpath, group);
			my_strcat(dirpath, "/");
		}

		/* 输出 group marker */
		prints("#### OS COMP TEST GROUP START ");
		if (is_subdir) {
			/* extract: "/musl/basic/" -> "basic-musl" */
			char disp[64];
			int di = 0, prev = 0, last = 0;
			for (int si = 0; group[si]; si++)
				if (group[si] == '/') { prev = last; last = si; }
			for (int si = prev + 1; si < last; si++)
				disp[di++] = group[si];
			/* append -musl / -glibc suffix */
			{
				int has_musl = 0, has_glibc = 0;
				for (int si = 0; group[si]; si++) {
					if (group[si] == '/' && group[si+1] == 'm' && group[si+2] == 'u' && group[si+3] == 's' && group[si+4] == 'l' && group[si+5] == '/')
						has_musl = 1;
					if (group[si] == '/' && group[si+1] == 'g' && group[si+2] == 'l' && group[si+3] == 'i' && group[si+4] == 'b' && group[si+5] == 'c' && group[si+6] == '/')
						has_glibc = 1;
				}
				if (has_musl) {
					disp[di++] = '-'; disp[di++] = 'm'; disp[di++] = 'u'; disp[di++] = 's'; disp[di++] = 'l';
				} else if (has_glibc) {
					disp[di++] = '-'; disp[di++] = 'g'; disp[di++] = 'l'; disp[di++] = 'i'; disp[di++] = 'b'; disp[di++] = 'c';
				}
			}
			disp[di] = '\0';
			prints(disp);
		} else {
			prints(group);
		}
		prints(" ####");
		println();

		run_elfs_in_dir(dirpath);

		prints("#### OS COMP TEST GROUP END ");
		if (is_subdir) {
			char disp[64];
			int di = 0, prev = 0, last = 0;
			for (int si = 0; group[si]; si++)
				if (group[si] == '/') { prev = last; last = si; }
			for (int si = prev + 1; si < last; si++)
				disp[di++] = group[si];
			/* append -musl / -glibc suffix */
			{
				int has_musl = 0, has_glibc = 0;
				for (int si = 0; group[si]; si++) {
					if (group[si] == '/' && group[si+1] == 'm' && group[si+2] == 'u' && group[si+3] == 's' && group[si+4] == 'l' && group[si+5] == '/')
						has_musl = 1;
					if (group[si] == '/' && group[si+1] == 'g' && group[si+2] == 'l' && group[si+3] == 'i' && group[si+4] == 'b' && group[si+5] == 'c' && group[si+6] == '/')
						has_glibc = 1;
				}
				if (has_musl) {
					disp[di++] = '-'; disp[di++] = 'm'; disp[di++] = 'u'; disp[di++] = 's'; disp[di++] = 'l';
				} else if (has_glibc) {
					disp[di++] = '-'; disp[di++] = 'g'; disp[di++] = 'l'; disp[di++] = 'i'; disp[di++] = 'b'; disp[di++] = 'c';
				}
			}
			disp[di] = '\0';
			prints(disp);
		} else {
			prints(group);
		}
		prints(" ####");
		println();
	}

	/* 根目录下的 ELF（兼容无 testcode 脚本的旧格式）*/
	run_elfs_in_dir("/");

	prints("#### OS COMP TEST END ####");
	println();
	my_shutdown();
}