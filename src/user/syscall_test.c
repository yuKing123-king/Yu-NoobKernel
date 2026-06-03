/*
 * syscall_test.c — 验证所有比赛需要的 syscall
 */

#define SYS_read        63
#define SYS_write       64
#define SYS_close       57
#define SYS_openat      56
#define SYS_getdents64  61
#define SYS_fork        220
#define SYS_execve      221
#define SYS_exit        93
#define SYS_brk         214
#define SYS_wait4       260

#define SYS_getcwd      17
#define SYS_chdir       49
#define SYS_fstat       80
#define SYS_mkdirat     34
#define SYS_unlinkat    35
#define SYS_mount       40
#define SYS_umount      39
#define SYS_getpid      172
#define SYS_getppid     173
#define SYS_times       153
#define SYS_sched_yield 124
#define SYS_gettimeofday 169
#define SYS_nanosleep   101
#define SYS_uname       160

#define O_RDONLY   0
#define O_RDWR     2
#define O_CREAT    0100
#define O_WRONLY   1

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
	asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1) : "memory");
	return _a0;
}

static long syscall3(long n, long a0, long a1, long a2)
{
	register long a7 asm("a7") = n;
	register long _a0 asm("a0") = a0;
	register long _a1 asm("a1") = a1;
	register long _a2 asm("a2") = a2;
	asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1), "r"(_a2) : "memory");
	return _a0;
}

static long syscall4(long n, long a0, long a1, long a2, long a3)
{
	register long a7 asm("a7") = n;
	register long _a0 asm("a0") = a0;
	register long _a1 asm("a1") = a1;
	register long _a2 asm("a2") = a2;
	register long _a3 asm("a3") = a3;
	asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3) : "memory");
	return _a0;
}

static long syscall5(long n, long a0, long a1, long a2, long a3, long a4)
{
	register long a7 asm("a7") = n;
	register long _a0 asm("a0") = a0;
	register long _a1 asm("a1") = a1;
	register long _a2 asm("a2") = a2;
	register long _a3 asm("a3") = a3;
	register long _a4 asm("a4") = a4;
	asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3), "r"(_a4) : "memory");
	return _a0;
}

static int my_strlen(const char *s) { int n = 0; while (*s++) n++; return n; }

static void prints(const char *s) { syscall3(SYS_write, 1, (long)s, my_strlen(s)); }

static void printn(long n)
{
	char buf[20];
	int i = 0;
	if (n < 0) { prints("-"); n = -n; }
	if (n == 0) { prints("0"); return; }
	while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
	while (i > 0) { char c = buf[--i]; syscall3(SYS_write, 1, (long)&c, 1); }
}

static void println(void) { syscall3(SYS_write, 1, (long)"\n", 1); }

static void test_getcwd(void)
{
	prints("[TEST] getcwd: ");
	char buf[128];
	long ret = syscall2(SYS_getcwd, (long)buf, 128);
	if (ret > 0) {
		prints(buf);
		println();
	} else {
		prints("FAILED ret=");
		printn(ret);
		println();
	}
}

static void test_chdir(void)
{
	prints("[TEST] chdir: ");
	long ret = syscall1(SYS_mkdirat, (long)"test_dir");
	if (ret < 0 && ret != -17) { /* -17 = EEXIST */
		prints("mkdir failed ret=");
		printn(ret);
		println();
		return;
	}
	ret = syscall1(SYS_chdir, (long)"test_dir");
	if (ret == 0) {
		prints("OK, cwd=");
		char buf[128];
		syscall2(SYS_getcwd, (long)buf, 128);
		prints(buf);
		println();
	} else {
		prints("FAILED ret=");
		printn(ret);
		println();
	}
	/* 切回根目录 */
	syscall1(SYS_chdir, (long)"/");
}

static void test_fstat(void)
{
	prints("[TEST] fstat: ");
	long fd = syscall3(SYS_openat, -100, (long)"/", 0);
	if (fd < 0) {
		prints("open / failed\n");
		return;
	}

	struct {
		long long st_dev;
		long long st_ino;
		unsigned int st_mode;
		unsigned int st_nlink;
		long long st_size;
		long long st_atime_sec;
		long long st_mtime_sec;
		long long st_ctime_sec;
	} kst;

	long ret = syscall2(SYS_fstat, fd, (long)&kst);
	if (ret == 0) {
		prints("OK size=");
		printn(kst.st_size);
		prints(" ino=");
		printn(kst.st_ino);
		prints(" nlink=");
		printn(kst.st_nlink);
		println();
	} else {
		prints("FAILED ret=");
		printn(ret);
		println();
	}
	syscall1(SYS_close, fd);
}

static void test_gettimeofday(void)
{
	prints("[TEST] gettimeofday: ");
	struct { long long sec; long long usec; } tv;
	long ret = syscall2(SYS_gettimeofday, (long)&tv, 0);
	if (ret == 0) {
		prints("sec=");
		printn(tv.sec);
		prints(" usec=");
		printn(tv.usec);
		println();
	} else {
		prints("FAILED ret=");
		printn(ret);
		println();
	}
}

static void test_nanosleep(void)
{
	prints("[TEST] nanosleep: ");
	struct { long long tv_sec; long long tv_nsec; } req;
	req.tv_sec = 0;
	req.tv_nsec = 100000000; /* 100ms */
	long ret = syscall2(SYS_nanosleep, (long)&req, 0);
	if (ret == 0) {
		prints("OK (100ms slept)");
		println();
	} else {
		prints("FAILED ret=");
		printn(ret);
		println();
	}
}

static void test_times(void)
{
	prints("[TEST] times: ");
	struct { long long tms_utime; long long tms_stime; long long tms_cutime; long long tms_cstime; } tms;
	long ret = syscall1(SYS_times, (long)&tms);
	if (ret >= 0) {
		prints("OK ticks=");
		printn(ret);
		println();
	} else {
		prints("FAILED ret=");
		printn(ret);
		println();
	}
}

static void test_sched_yield(void)
{
	prints("[TEST] sched_yield: ");
	long ret = syscall1(SYS_sched_yield, 0);
	if (ret == 0) {
		prints("OK");
		println();
	} else {
		prints("FAILED ret=");
		printn(ret);
		println();
	}
}

static void test_uname(void)
{
	prints("[TEST] uname: ");
	struct { char s[65]; char n[65]; char r[65]; char v[65]; char m[65]; char d[65]; } un;
	long ret = syscall1(SYS_uname, (long)&un);
	if (ret == 0) {
		prints(un.s);
		prints(" ");
		prints(un.m);
		println();
	} else {
		prints("FAILED ret=");
		printn(ret);
		println();
	}
}

__attribute__((section(".text.entry"), noinline, noreturn))
void _start(void)
{
	prints("========== syscall test ==========");
	println();

	test_getcwd();
	test_chdir();
	test_fstat();
	test_gettimeofday();
	test_nanosleep();
	test_times();
	test_sched_yield();
	test_uname();

	prints("========== all tests done ==========");
	println();

	syscall1(SYS_exit, 0);
	while (1) {}
}
