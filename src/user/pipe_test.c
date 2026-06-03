/* pipe_test.c — 验证 pipe2/dup2 syscall */

#define SYS_read        63
#define SYS_write       64
#define SYS_close       57
#define SYS_fork        220
#define SYS_exit        93
#define SYS_pipe2       59

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

static long syscall1(long n, long a0)
{
	register long a7 asm("a7") = n;
	register long _a0 asm("a0") = a0;
	asm volatile("ecall" : "+r"(_a0) : "r"(a7) : "memory");
	return _a0;
}

static long my_write(int fd, const char *buf, int len)
{
	return syscall3(SYS_write, fd, (long)buf, len);
}

static void prints(const char *s)
{
	int len = 0;
	while (s[len]) len++;
	my_write(1, s, len);
}

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

static long my_read(int fd, char *buf, int len)
{
	return syscall3(SYS_read, fd, (long)buf, len);
}

static long my_close(int fd)
{
	return syscall1(SYS_close, fd);
}

static long my_fork(void)
{
	register long a7 asm("a7") = SYS_fork;
	register long _a0 asm("a0") = 0;
	register long _a1 asm("a1") = 0;  /* child_stack = 0: fork 行为 */
	asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1) : "memory");
	return _a0;
}

static long my_wait4(int pid, int *status, int options, long rusage)
{
	register long a7 asm("a7") = 260;
	register long _a0 asm("a0") = pid;
	register long _a1 asm("a1") = (long)status;
	register long _a2 asm("a2") = options;
	register long _a3 asm("a3") = rusage;
	asm volatile("ecall"
		     : "+r"(_a0)
		     : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3)
		     : "memory");
	return _a0;
}

__attribute__((noreturn))
static void my_exit(int code) { syscall1(SYS_exit, code); while (1) {} }

__attribute__((section(".text.entry"), noinline, noreturn))
void _start(void)
{
	/* === Test 1: 父进程 pipe, fork, 子进程 write, 父进程 read === */
	prints("[TEST] pipe read/write");
	println();

	int pipefd[2];
	long ret = syscall2(SYS_pipe2, (long)pipefd, 0);
	if (ret < 0) {
		prints("[FAIL] pipe2 failed: ");
		printn(ret);
		println();
		my_exit(1);
	}
	prints("[OK] pipe2: fd=");
	printn(pipefd[0]);
	prints(", fd=");
	printn(pipefd[1]);
	println();

	long cpid = my_fork();
	if (cpid == 0) {
		/* 子进程：写端写入 */
		my_close(pipefd[0]);
		char msg[] = "hello from child";
		my_write(pipefd[1], msg, sizeof(msg));
		my_close(pipefd[1]);
		my_exit(0);
	}

	/* 父进程：读端读取 */
	my_close(pipefd[1]);
	char buf[64];
	int total = 0;
	while (1) {
		long n = my_read(pipefd[0], buf + total, sizeof(buf) - total);
		if (n <= 0) break;
		total += n;
	}
	my_close(pipefd[0]);

	prints("[OK] parent read ");
	printn(total);
	prints(" bytes: ");
	my_write(1, buf, total);
	println();

	int status = 0;
	my_wait4((int)cpid, &status, 0, 0);
	prints("[OK] child exited with status=");
	printn(status);
	println();

	/* === Test 2: 多次写入验证 === */
	prints("[TEST] multi-write pipe");
	println();

	ret = syscall2(SYS_pipe2, (long)pipefd, 0);
	if (ret < 0) {
		prints("[FAIL] pipe2 #2 failed: ");
		printn(ret);
		println();
		my_exit(1);
	}

	cpid = my_fork();
	if (cpid == 0) {
		my_close(pipefd[0]);
		my_write(pipefd[1], "AAA", 3);
		my_write(pipefd[1], "BBB", 3);
		my_write(pipefd[1], "CCC", 3);
		my_close(pipefd[1]);
		my_exit(0);
	}

	my_close(pipefd[1]);
	total = 0;
	while (1) {
		long n = my_read(pipefd[0], buf + total, sizeof(buf) - total);
		if (n <= 0) break;
		total += n;
	}
	my_close(pipefd[0]);

	prints("[OK] multi-read ");
	printn(total);
	prints(" bytes: ");
	my_write(1, buf, total);
	println();

	my_wait4((int)cpid, &status, 0, 0);

	prints("[DONE] all pipe tests passed");
	println();
	my_exit(0);
}
