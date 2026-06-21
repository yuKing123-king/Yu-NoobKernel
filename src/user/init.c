/*
 * init.c — 比赛测试运行器
 */
/* #define DEBUG_DUMP_TREE */

/* 空 envp 数组，供 execve 使用（glibc 需要 envp 非 NULL） */
static long empty_envp[1] = { 0 };

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
#define SYS_dup3        24

#define O_RDONLY    0
#define O_DIRECTORY 0200000
#define DT_REG       8
#define DT_DIR       4

struct linux_dirent64 {
	unsigned long long d_ino;
	long long d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[0];
};

static long syscall1(long n, long a0) {
	register long a7 asm("a7") = n;
	register long _a0 asm("a0") = a0;
	asm volatile("ecall" : "+r"(_a0) : "r"(a7) : "memory");
	return _a0;
}
static long syscall2(long n, long a0, long a1) {
	register long a7 asm("a7") = n; register long _a0 asm("a0") = a0;
	register long _a1 asm("a1") = a1;
	asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1) : "memory");
	return _a0;
}
static long syscall3(long n, long a0, long a1, long a2) {
	register long a7 asm("a7") = n; register long _a0 asm("a0") = a0;
	register long _a1 asm("a1") = a1; register long _a2 asm("a2") = a2;
	asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1), "r"(_a2) : "memory");
	return _a0;
}
static long syscall4(long n, long a0, long a1, long a2, long a3) {
	register long a7 asm("a7") = n; register long _a0 asm("a0") = a0;
	register long _a1 asm("a1") = a1; register long _a2 asm("a2") = a2;
	register long _a3 asm("a3") = a3;
	asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3) : "memory");
	return _a0;
}

static int my_strlen(const char *s) { int n=0; while(*s++)n++; return n; }
static int my_strcmp(const char *a, const char *b) { while(*a&&*a==*b){a++;b++;} return *(unsigned char*)a-*(unsigned char*)b; }
static int my_strcpy(char *d, const char *s) { char *dd=d; while((*d++=*s++)); return d-dd-1; }
static int my_strcat(char *d, const char *s) { char *dd=d+my_strlen(d); while((*dd++=*s++)); return my_strlen(d); }
static long my_write(int fd,const char*buf,int len) { return syscall3(SYS_write,fd,(long)buf,len); }
static void prints(const char*s){my_write(1,s,my_strlen(s));}
static void printn(long n){char b[20];int i=0;if(n<0){my_write(1,"-",1);n=-n;}if(n==0){my_write(1,"0",1);return;}while(n>0){b[i++]='0'+(n%10);n/=10;}while(i>0)my_write(1,&b[--i],1);}
static void println(void){my_write(1,"\r\n",2);}
static long my_openat(int df,const char*p,int f){return syscall3(SYS_openat,df,(long)p,f);}
static long my_close(int fd){return syscall1(SYS_close,fd);}
static long my_read(int fd,void*buf,int len){return syscall3(SYS_read,fd,(long)buf,len);}
static long my_chdir(const char*p){return syscall1(SYS_chdir,(long)p);}
static long my_fork(void){register long a7 asm("a7")=SYS_fork;register long _a0 asm("a0")=0;register long _a1 asm("a1")=0;asm volatile("ecall" : "+r"(_a0) : "r"(a7),"r"(_a1) : "memory");return _a0;}
static long my_execve(const char*p,long a,long e){return syscall3(SYS_execve,(long)p,a,e);}
static long my_wait4(int p,int*s,int o,long r){return syscall4(SYS_wait4,p,(long)s,o,r);}
__attribute__((noreturn)) static void my_exit(int c){syscall1(SYS_exit,c);while(1){}}
static long my_brk(long a){return syscall1(SYS_brk,a);}
__attribute__((noreturn)) static void my_shutdown(void){syscall1(SYS_shutdown,0);while(1){}}
static long my_getdents64(int fd,void*buf,int len){return syscall3(SYS_getdents64,fd,(long)buf,len);}
static long my_dup3(int oldfd, int newfd, int flags) { return syscall3(SYS_dup3, oldfd, newfd, flags); }

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
	if (my_strcmp(name, "lua") == 0 ||
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
			if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) { pos += d->d_reclen; continue; }
			if (d->d_type == DT_REG) {
				char path[256];
				my_strcpy(path, dirpath); my_strcat(path, name);
				if (!is_elf_path(path)) { pos += d->d_reclen; continue; }
				if (is_shell_needed(name)) {
					prints("#### OS COMP TEST GROUP START "); prints(name); prints(" ####"); println();
					prints("#### OS COMP TEST GROUP END "); prints(name); prints(" ####"); println();
					pos += d->d_reclen; continue;
				}
				prints("[RUN] "); prints(path); println();
				long cpid = my_fork();
				if (cpid == 0) { my_chdir(dirpath); long argv[2]={(long)path,0}; long ret=my_execve(path,(long)argv,(long)empty_envp);
					prints("[FAIL] execve failed: "); printn(ret); println(); my_exit(127); }
				int status = 0; my_wait4(cpid, &status, 0, 0);
				if (my_strcmp(name, "yield") == 0) { pos = nread; break; }
			}
			pos += d->d_reclen;
		}
	}
	my_close(fd);
}

static const char *scan_groups[] = { "/musl/basic/", "/glibc/basic/" };
static const char *scan_group_names[] = { "basic-musl", "basic-glibc" };
static int scan_group_cnt = 2;

static void run_busybox(const char *root, const char *group_name)
{
	char script[256];
	my_strcpy(script, root); my_strcat(script, "busybox_testcode.sh");

	prints("#### OS COMP TEST GROUP START "); prints(group_name); prints(" ####"); println();

	char bbpath[256];
	my_strcpy(bbpath, root); my_strcat(bbpath, "busybox");

	/* 1) busybox true */
	{ long cpid=my_fork(); if(cpid==0){ long a[3]={(long)bbpath,(long)"true",0}; long r=my_execve(bbpath,(long)a,(long)empty_envp);
		prints("[FAIL] true: ");printn(r);println();my_exit(127);} int st=0;my_wait4(cpid,&st,0,0); }

	/* 2) busybox pwd */
	{ long cpid=my_fork(); if(cpid==0){ my_chdir(root); long a[3]={(long)bbpath,(long)"pwd",0}; long r=my_execve(bbpath,(long)a,(long)empty_envp);
		prints("[FAIL] pwd: ");printn(r);println();my_exit(127);} int st=0;my_wait4(cpid,&st,0,0); }

	/* 3) sh -c 'echo HELLO' (builtin echo) */
	{ long cpid=my_fork(); if(cpid==0){ my_chdir(root); long a[5]={(long)bbpath,(long)"sh",(long)"-c",(long)"echo HELLO",0};
		long r=my_execve(bbpath,(long)a,(long)empty_envp); prints("[FAIL] sh -c echo: ");printn(r);println();my_exit(127);} int st=0;my_wait4(cpid,&st,0,0); }

	/* 4) sh -c 'exit 42' (verify exit code) */
	{ long cpid=my_fork(); if(cpid==0){ long a[5]={(long)bbpath,(long)"sh",(long)"-c",(long)"exit 42",0};
		long r=my_execve(bbpath,(long)a,(long)empty_envp); prints("[FAIL] sh -c exit42: ");printn(r);println();my_exit(127);} int st=0;my_wait4(cpid,&st,0,0);
		prints("[DBG] exit42 status=");printn(st);println(); }

	/* 5) sh -c 'echo ARGS:0=$0 1=$1 2=$2' aa bb (verify argv) */
	{ long cpid=my_fork(); if(cpid==0){ my_chdir(root); long a[7]={(long)bbpath,(long)"sh",(long)"-c",(long)"echo ARGS:0=$0 1=$1 2=$2",(long)"aa",(long)"bb",0};
		long r=my_execve(bbpath,(long)a,(long)empty_envp); prints("[FAIL] sh -c args: ");printn(r);println();my_exit(127);} int st=0;my_wait4(cpid,&st,0,0); }

	/* 6) sh < script (stdin redirect from script file) */
	{
		int fd = my_openat(-100, script, O_RDONLY);
		if (fd >= 0) {
			long cpid = my_fork();
			if (cpid == 0) {
				my_dup3(fd, 0, 0);
				my_close(fd);
				my_chdir(root);
				long a[3] = {(long)bbpath, (long)"sh", 0};
				long r = my_execve(bbpath, (long)a, (long)empty_envp);
				prints("[FAIL] sh stdin: ");printn(r);println();
				my_exit(127);
			}
			int st = 0;
			my_wait4(cpid, &st, 0, 0);
			my_close(fd);
			prints("[DBG] sh stdin exit=");printn(st);println();
		} else {
			prints("[FAIL] cannot open script for stdin");println();
		}
	}

	/* 7) sh /musl/busybox_testcode.sh (argument — competition pattern) */
	{ long cpid=my_fork(); if(cpid==0){ my_chdir(root); long a[4]={(long)bbpath,(long)"sh",(long)script,0};
		long r=my_execve(bbpath,(long)a,(long)empty_envp); prints("[FAIL] sh script: ");printn(r);println();my_exit(127);} int st=0;my_wait4(cpid,&st,0,0);
		prints("[DBG] sh script exit=");printn(st);println(); }

	/* 8) sh -c '. /musl/busybox_testcode.sh' (source the script) */
	{ long cpid=my_fork(); if(cpid==0){ my_chdir(root);
		long a[5]={(long)bbpath,(long)"sh",(long)"-c",(long)". ./busybox_testcode.sh",0};
		long r=my_execve(bbpath,(long)a,(long)empty_envp); prints("[FAIL] sh -c source: ");printn(r);println();my_exit(127);} int st=0;my_wait4(cpid,&st,0,0);
		prints("[DBG] source exit=");printn(st);println(); }

	prints("#### OS COMP TEST GROUP END "); prints(group_name); prints(" ####"); println();
}

__attribute__((section(".text.entry"), noinline, noreturn))
void _start(void)
{
	my_brk(0);
	prints("#### OS COMP TEST START ####"); println();

	for (int g = 0; g < scan_group_cnt; g++) {
		prints("#### OS COMP TEST GROUP START "); prints(scan_group_names[g]); prints(" ####"); println();
		run_elfs_in_dir(scan_groups[g]);
		prints("#### OS COMP TEST GROUP END "); prints(scan_group_names[g]); prints(" ####"); println();

		/* busybox test: strip /basic/ suffix */
		char root[64]; my_strcpy(root, scan_groups[g]);
		int len = my_strlen(root);
		if (len >= 7 && my_strcmp(root + len - 7, "/basic/") == 0) root[len - 6] = '\0';
		char bb_name[64]; my_strcpy(bb_name, "busybox-");
		my_strcat(bb_name, scan_group_names[g] + 6);
		run_busybox(root, bb_name);
	}

	prints("#### OS COMP TEST END ####"); println();
	my_shutdown();
}
