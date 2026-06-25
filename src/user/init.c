/*
 * init.c — 比赛测试运行器
 */
#include <misc/stddef.h>
/* #define DEBUG_DUMP_TREE */

static char env_path[] = "PATH=.:/musl:/glibc";
static long default_envp[2] = { (long)env_path, 0 };

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
#define SYS_pipe2       59
#define SYS_lseek       62
#define SYS_unlinkat    35

#define O_RDONLY    0
#define O_WRONLY    1
#define O_CREAT     0100
#define O_TRUNC     01000
#define O_DIRECTORY 0200000
#define SEEK_SET    0
#define DT_REG       8
#define DT_DIR       4
#define AT_REMOVEDIR 0x200

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
static long my_lseek(int fd, long off, int whence) { return syscall3(SYS_lseek, fd, off, whence); }
static long my_unlinkat(int dirfd, const char *path, int flags) { return syscall3(SYS_unlinkat, dirfd, (long)path, flags); }

static void ensure_busybox_wrapper(const char *root, const char *name)
{
	char path[256];
	char script[384];
	int fd;

	my_strcpy(path, root);
	my_strcat(path, name);

	my_unlinkat(-100, path, 0);

	my_strcpy(script, "#!");
	my_strcat(script, root);
	my_strcat(script, "busybox sh\nexec ");
	my_strcat(script, root);
	my_strcat(script, "busybox ");
	my_strcat(script, name);
	my_strcat(script, " \"$@\"\n");

	fd = my_openat(-100, path, O_CREAT | O_WRONLY | O_TRUNC);
	if (fd < 0)
		return;
	my_write(fd, script, my_strlen(script));
	my_close(fd);
}

static void ensure_bin_busybox(const char *root)
{
	char path[256];
	char script[384];
	int fd;

	my_strcpy(path, "/bin/busybox");
	my_unlinkat(-100, path, 0);

	my_strcpy(script, "#!");
	my_strcat(script, root);
	my_strcat(script, "busybox sh\nexec ");
	my_strcat(script, root);
	my_strcat(script, "busybox \"$@\"\n");

	fd = my_openat(-100, path, O_CREAT | O_WRONLY | O_TRUNC);
	if (fd < 0)
		return;
	my_write(fd, script, my_strlen(script));
	my_close(fd);
}

static void ensure_busybox_wrappers(const char *root)
{
	static const char *const applets[] = {
		"ls",   "sleep", "kill", "uniq", "sort", "cat",  "grep",
		"wc",   "more",  "stat", "find", "cp",   "rm",   "mv",
		"mkdir","rmdir", "touch", NULL,
	};

	for (int i = 0; applets[i]; i++)
		ensure_busybox_wrapper(root, applets[i]);
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

static int my_streq(const char *a, const char *b)
{
	return my_strcmp(a, b) == 0;
}

static void print_busybox_case_banner(const char *phase, const char *line)
{
	prints("========== ");
	prints(phase);
	prints(" test_busybox");
	if (line && line[0]) {
		prints(": ");
		prints(line);
	}
	prints(" ==========");
	println();
}

static void print_group_case_banner(const char *phase, const char *line)
{
	prints("========== ");
	prints(phase);
	prints(" test_group");
	if (line && line[0]) {
		prints(": ");
		prints(line);
	}
	prints(" ==========");
	println();
}

static void print_case_result(const char *kind, const char *name, int ok)
{
	prints("testcase ");
	prints(kind);
	prints(" ");
	prints(name);
	if (ok)
		prints(" success");
	else
		prints(" fail");
	println();
}

static int line_needs_shell(const char *line)
{
	for (int i = 0; line[i]; i++) {
		char c = line[i];
		if (c == '|' || c == '>' || c == '<')
			return 1;
	}
	return 0;
}

static int tokenize_simple_cmd(char *line, char **argv, int max_args)
{
	int argc = 0;
	int i = 0;

	while (line[i]) {
		while (line[i] == ' ' || line[i] == '\t')
			i++;
		if (!line[i])
			break;
		if (argc >= max_args - 1)
			return -1;

		if (line[i] == '"' || line[i] == '\'') {
			char quote = line[i++];
			argv[argc++] = &line[i];
			while (line[i] && line[i] != quote)
				i++;
			if (!line[i])
				return -1;
			line[i++] = '\0';
			continue;
		}

		argv[argc++] = &line[i];
		while (line[i] && line[i] != ' ' && line[i] != '\t')
			i++;
		if (line[i])
			line[i++] = '\0';
	}

	argv[argc] = 0;
	return argc;
}

static void run_busybox_cmd_list(const char *root, const char *group_name,
				      const char *bbpath)
{
	char cmdfile[256];
	my_strcpy(cmdfile, root);
	my_strcat(cmdfile, "busybox_cmd.txt");

	int fd = my_openat(-100, cmdfile, O_RDONLY);
	if (fd < 0) {
		prints("[FAIL] cannot open busybox_cmd.txt"); println();
		return;
	}

	char buf[4096];
	long nread = 0;
	while (nread < (long)sizeof(buf) - 1) {
		long r = my_read(fd, buf + nread, sizeof(buf) - 1 - nread);
		if (r < 0) {
			nread = r;
			break;
		}
		if (r == 0)
			break;
		nread += r;
	}
	my_close(fd);
	if (nread < 0) {
		prints("[FAIL] cannot read busybox_cmd.txt"); println();
		return;
	}
	buf[nread] = '\0';

	int start = 0;
	for (int i = 0;; i++) {
		char ch = buf[i];
		if (ch != '\n' && ch != '\0')
			continue;

		buf[i] = '\0';
		char *line = &buf[start];
		if (i > start && line[i - start - 1] == '\r')
			line[i - start - 1] = '\0';

		if (line[0] != '\0') {
			char linebuf[256];
			my_strcpy(linebuf, line);
			print_busybox_case_banner("START", linebuf);
			prints("[BBRUN] "); prints(linebuf); println();
			long cpid = my_fork();
			if (cpid == 0) {
				my_chdir(root);
				long r;
				if (line_needs_shell(linebuf)) {
					long a[5] = {(long)bbpath, (long)"sh", (long)"-c",
						     (long)linebuf, 0};
					r = my_execve(bbpath, (long)a, (long)default_envp);
				} else {
					char cmd[256];
					char *argv[20];
					long a[21];
					my_strcpy(cmd, linebuf);
					int argc = tokenize_simple_cmd(cmd, argv, 20);
					if (argc <= 0) {
						prints("[FAIL] parse busybox cmd"); println();
						my_exit(127);
					}
					a[0] = (long)bbpath;
					for (int j = 0; j < argc; j++)
						a[j + 1] = (long)argv[j];
					a[argc + 1] = 0;
					r = my_execve(bbpath, (long)a, (long)default_envp);
				}
				prints("[FAIL] busybox sh -c: "); printn(r); println();
				my_exit(127);
			}
			int st = 0;
			my_wait4(cpid, &st, 0, 0);
			int code = (st >> 8) & 0xff;
			if (code != 0 && !my_streq(linebuf, "false")) {
				prints("testcase busybox "); prints(linebuf); prints(" fail"); println();
			} else {
				prints("testcase busybox "); prints(linebuf); prints(" success"); println();
			}
			print_busybox_case_banner("END", linebuf);
		}

		if (ch == '\0')
			break;
		start = i + 1;
	}

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
				if (cpid == 0) { my_chdir(dirpath); long argv[2]={(long)path,0}; long ret=my_execve(path,(long)argv,(long)default_envp);
					prints("[FAIL] execve failed: "); printn(ret); println(); my_exit(127); }
				int status = 0; my_wait4(cpid, &status, 0, 0);
				if (my_strcmp(name, "yield") == 0) { pos = nread; break; }
			}
			pos += d->d_reclen;
		}
	}
	my_close(fd);
}

static int my_starts_with(const char *s, const char *prefix)
{
	for (int i = 0; prefix[i]; i++) {
		if (s[i] != prefix[i])
			return 0;
	}
	return 1;
}

static void cleanup_test_artifacts(const char *dirpath)
{
	int fd = my_openat(-100, dirpath, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		return;

	char buf[4096];
	long nread;
	while ((nread = my_getdents64(fd, buf, sizeof(buf))) > 0) {
		int pos = 0;
		while (pos < (int)nread) {
			struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
			if (d->d_reclen == 0)
				break;
			char *name = d->d_name;
			if (!(name[0] == '.' && (name[1] == '\0' ||
			    (name[1] == '.' && name[2] == '\0'))) &&
			    (my_strcmp(name, "test.txt") == 0 ||
			     my_strcmp(name, "test_dir") == 0 ||
			     my_strcmp(name, "test") == 0 ||
			     my_starts_with(name, "busybox_cmd.bak"))) {
				my_unlinkat(fd, name, 0);
				my_unlinkat(fd, name, AT_REMOVEDIR);
			}
			pos += d->d_reclen;
		}
	}
	my_close(fd);
}

static const char *scan_groups[] = { "/musl/basic/", "/glibc/basic/" };
static const char *scan_group_names[] = { "basic-musl", "basic-glibc" };
static int scan_group_cnt = 2;

static void run_lua_group(const char *root, const char *group_name)
{
	static const char *const lua_cases[] = {
		"date.lua",
		"file_io.lua",
		"max_min.lua",
		"random.lua",
		"remove.lua",
		"round_num.lua",
		"sin30.lua",
		"sort.lua",
		"strings.lua",
		0,
	};
	char luapath[256];

	my_strcpy(luapath, root);
	my_strcat(luapath, "lua");

	prints("#### OS COMP TEST GROUP START "); prints(group_name); prints(" ####"); println();
	for (int i = 0; lua_cases[i]; i++) {
		char case_name[128];
		long cpid;
		int status = 0;

		my_strcpy(case_name, group_name);
		my_strcat(case_name, ": ");
		my_strcat(case_name, lua_cases[i]);

		print_group_case_banner("START", case_name);
		prints("[RUN] ./lua "); prints(lua_cases[i]); println();

		cpid = my_fork();
		if (cpid == 0) {
			long argv[3] = { (long)luapath, (long)lua_cases[i], 0 };
			my_chdir(root);
			long ret = my_execve(luapath, (long)argv, (long)default_envp);
			prints("[FAIL] lua execve failed: "); printn(ret); println();
			my_exit(127);
		}

		my_wait4(cpid, &status, 0, 0);
		print_case_result("lua", case_name, ((status >> 8) & 0xff) == 0);
		print_group_case_banner("END", case_name);
	}
	prints("#### OS COMP TEST GROUP END "); prints(group_name); prints(" ####"); println();
}

static void run_busybox(const char *root, const char *group_name)
{
	char script[256];
	my_strcpy(script, root); my_strcat(script, "busybox_testcode.sh");

	prints("#### OS COMP TEST GROUP START "); prints(group_name); prints(" ####"); println();

	char bbpath[256];
	my_strcpy(bbpath, root); my_strcat(bbpath, "busybox");
	cleanup_test_artifacts(root);
	ensure_bin_busybox(root);
	ensure_busybox_wrappers(root);

	run_busybox_cmd_list(root, group_name, bbpath);

	prints("#### OS COMP TEST GROUP END "); prints(group_name); prints(" ####"); println();
}

__attribute__((section(".text.entry"), noinline, noreturn))
void _start(void)
{
	my_brk(0);
	prints("#### OS COMP TEST START ####"); println();

	for (int g = 0; g < scan_group_cnt; g++) {
		cleanup_test_artifacts(scan_groups[g]);
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

		char lua_name[64]; my_strcpy(lua_name, "lua-");
		my_strcat(lua_name, scan_group_names[g] + 6);
		run_lua_group(root, lua_name);
	}

	prints("#### OS COMP TEST END ####"); println();
	my_shutdown();
}
