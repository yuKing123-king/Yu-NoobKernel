#pragma once

/*
 * 系统调用编号定义
 * 兼容 Linux RISC-V (generic) syscall ABI，使 BusyBox 无需修改调用号
 * 参考: arch/riscv/include/uapi/asm/unistd.h + include/uapi/asm-generic/unistd.h
 */

/* 已实现的 syscall */
#define SYS_read        63
#define SYS_write       64
#define SYS_close       57
#define SYS_exit_group  94
#define SYS_exit        93
#define SYS_openat      56
#define SYS_getdents64  61

/* 新增：进程管理 */
#define SYS_clone       220
#define SYS_fork        220 /* RISC-V: fork is clone with flags=0 */
#define SYS_execve      221
#define SYS_wait4       260
#define SYS_getpid      172
#define SYS_getppid     173

/* 新增：内存管理 */
#define SYS_brk         214
#define SYS_mmap        222
#define SYS_munmap      215
#define SYS_mprotect    226

/* 新增：文件描述符操作 */
#define SYS_dup         23
#define SYS_dup3        24
#define SYS_pipe2       59
#define SYS_fcntl       25
#define SYS_ioctl       29
#define SYS_lseek       62
#define SYS_readv       65
#define SYS_writev      66

/* 新增：文件系统操作 */
#define SYS_mkdirat     34
#define SYS_unlinkat    35
#define SYS_statx       291
#define SYS_fstatat     79
#define SYS_newfstatat  79

/* 新增：信号（stub） */
#define SYS_rt_sigaction    134
#define SYS_rt_sigprocmask  135
#define SYS_sigprocmask     135

/* 新增：杂项 */
#define SYS_uname      160
#define SYS_getrandom  278
#define SYS_set_tid_address 96

/* 自定义 syscall（非 Linux 标准） */
#define SYS_shutdown    500
#define SYS_reboot      501

/* 分发表最大容量 */
#define SYSCALL_MAX     512
