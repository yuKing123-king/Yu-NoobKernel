/*
 * hello.c — 极简测试 ELF，仅输出 "Hello from test ELF" 然后退出
 * 用于验证 fork/execve/wait4 路径
 */

#define SYS_write 64
#define SYS_exit  93

static long syscall2(long n, long a0, long a1)
{
	register long a7 asm("a7") = n;
	register long _a0 asm("a0") = a0;
	register long _a1 asm("a1") = a1;
	asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1) : "memory");
	return _a0;
}

static long syscall1(long n, long a0)
{
	register long a7 asm("a7") = n;
	register long _a0 asm("a0") = a0;
	asm volatile("ecall" : "+r"(_a0) : "r"(a7) : "memory");
	return _a0;
}

__attribute__((section(".text.entry"), noreturn))
void _start(void)
{
	const char msg[] = "Hello from test ELF\r\n";
	syscall2(SYS_write, 1, (long)msg);
	syscall1(SYS_exit, 42);
	while (1) {}
}
