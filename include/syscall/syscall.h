#pragma once

#include <misc/stddef.h>

void syscall(void);

uintptr_t sys_write(int fd, uintptr_t buf, size_t len);
uintptr_t sys_read(int fd, uintptr_t buf, size_t len);
uintptr_t sys_exit(int status);
uintptr_t sys_shutdown(void);
uintptr_t sys_reboot(void);
uintptr_t sys_openat(uintptr_t dirfd, uintptr_t pathname, uintptr_t flags, uintptr_t mode);
uintptr_t sys_close(uintptr_t fd);
uintptr_t sys_getdents(uintptr_t fd, uintptr_t buf, uintptr_t count);

/* 进程管理 */
uintptr_t sys_fork(void);
uintptr_t sys_execve(uintptr_t filename, uintptr_t argv, uintptr_t envp);
uintptr_t sys_wait4(uintptr_t pid, uintptr_t wstatus, uintptr_t options, uintptr_t rusage);
uintptr_t sys_getpid(void);
uintptr_t sys_getppid(void);

/* 内存管理 */
uintptr_t sys_brk(uintptr_t addr);
uintptr_t sys_mmap(uintptr_t addr, size_t len, int prot, int flags, int fd, loff_t offset);
uintptr_t sys_munmap(uintptr_t addr, size_t len);
uintptr_t sys_mprotect(uintptr_t addr, size_t len, int prot);

/* 文件描述符操作 */
uintptr_t sys_dup(uintptr_t oldfd);
uintptr_t sys_dup3(uintptr_t oldfd, uintptr_t newfd, uintptr_t flags);
uintptr_t sys_pipe2(uintptr_t pipefd, uintptr_t flags);
uintptr_t sys_lseek(uintptr_t fd, loff_t offset, int whence);
uintptr_t sys_readv(uintptr_t fd, uintptr_t iov, uintptr_t iovcnt);
uintptr_t sys_writev(uintptr_t fd, uintptr_t iov, uintptr_t iovcnt);
uintptr_t sys_fcntl(uintptr_t fd, uintptr_t cmd, uintptr_t arg);
uintptr_t sys_ioctl(uintptr_t fd, uintptr_t request, uintptr_t arg);

/* 文件系统 */
uintptr_t sys_mkdirat(uintptr_t dirfd, uintptr_t pathname, uintptr_t mode);
uintptr_t sys_unlinkat(uintptr_t dirfd, uintptr_t pathname, uintptr_t flags);

/* 信号（stub） */
uintptr_t sys_rt_sigaction(int sig, uintptr_t act, uintptr_t oact, size_t sigsetsize);
uintptr_t sys_rt_sigprocmask(int how, uintptr_t set, uintptr_t oset, size_t sigsetsize);

/* 杂项 */
uintptr_t sys_uname(uintptr_t buf);
uintptr_t sys_exit_group(int status);
