#pragma once

/*
 * 标准错误代码定义
 */
#define OK 0	   /* 成功 */
#define EPERM 1	   /* 操作不允许 */
#define ENOENT 2   /* 文件或目录不存在 */
#define ESRCH 3	   /* 进程不存在 */
#define EINTR 4	   /* 系统调用被中断 */
#define EIO 5	   /* I/O错误 */
#define ENXIO 6	   /* 设备不存在或地址不存在 */
#define E2BIG 7	   /* 参数列表过长 */
#define ENOEXEC 8  /* 可执行文件格式错误 */
#define EBADF 9	   /* 文件描述符错误 */
#define ECHILD 10  /* 子进程不存在 */
#define EAGAIN 11  /* 资源暂时不可用 */
#define ENOMEM 12  /* 内存不足 */
#define EACCES 13  /* 权限不足 */
#define EFAULT 14  /* 地址错误 */
#define ENOTBLK 15 /* 不是块设备 */
#define EBUSY 16   /* 设备或资源忙 */
#define EEXIST 17  /* 文件已存在 */
#define EXDEV 18   /* 跨设备链接 */
#define ENODEV 19  /* 设备不存在 */
#define ENOTDIR 20 /* 不是目录 */
#define EISDIR 21  /* 是目录 */
#define EINVAL 22  /* 无效参数 */
#define ENFILE 23  /* 系统打开文件数过多 */
#define EMFILE 24  /* 进程打开文件数过多 */
#define ENOTTY 25  /* 不适当的ioctl */
#define ETXTBSY 26 /* 文本文件忙 */
#define EFBIG 27   /* 文件过大 */
#define ENOSPC 28  /* 设备空间不足 */
#define ESPIPE 29  /* 无效的寻址操作 */
#define EROFS 30   /* 只读文件系统 */
#define EMLINK 31  /* 链接数过多 */
#define EPIPE 32   /* 管道破裂 */
#define EDOM 33	   /* 数学参数超出定义域 */
#define ERANGE 34  /* 结果过大 */

/* 扩展错误代码 */
#define EDEADLK 35	   /* 资源死锁 */
#define ENAMETOOLONG 36	   /* 文件名过长 */
#define ENOLCK 37	   /* 没有可用的锁 */
#define ENOSYS 38	   /* 功能未实现 */
#define ENOTEMPTY 39	   /* 目录非空 */
#define ELOOP 40	   /* 符号链接层级过多 */
#define EWOULDBLOCK EAGAIN /* 操作会阻塞 */
#define ENOMSG 42	   /* 无消息 */
#define EIDRM 43	   /* 标识符被删除 */
#define ECHRNG 44	   /* 通道号超出范围 */
#define EL2NSYNC 45	   /* 二级不同步 */
#define EL3HLT 46	   /* 三级暂停 */
#define EL3RST 47	   /* 三级重置 */
#define ELNRNG 48	   /* 链接号超出范围 */
#define EUNATCH 49	   /* 未连接协议驱动 */
#define ENOCSI 50	   /* 无CSI结构可用 */
#define EL2HLT 51	   /* 二级暂停 */
#define EBADE 52	   /* 无效的交换 */
#define EBADR 53	   /* 无效的请求描述符 */
#define EXFULL 54	   /* 交换满 */
#define ENOANO 55	   /* 不是匿名节点 */
#define EBADRQC 56	   /* 无效的请求码 */
#define EBADSLT 57	   /* 无效的槽位 */

/* 内核专用错误代码 */
#define EDEADLOCK EDEADLK
#define EBFONT 59	   /* 错误的字体文件格式 */
#define ENOSTR 60	   /* 设备不是流 */
#define ENODATA 61	   /* 包无数据 */
#define ETIME 62	   /* 定时器超时 */
#define ENOSR 63	   /* 流资源不足 */
#define ENONET 64	   /* 机器不在网络上 */
#define ENOPKG 65	   /* 包未安装 */
#define EREMOTE 66	   /* 对象是远程的 */
#define ENOLINK 67	   /* 链接已被切断 */
#define EADV 68		   /* 广告错误 */
#define ESRMNT 69	   /* srmount错误 */
#define ECOMM 70	   /* 通信错误 */
#define EPROTO 71	   /* 协议错误 */
#define EMULTIHOP 72	   /* 多跳尝试 */
#define EDOTDOT 73	   /* RFS特定错误 */
#define EBADMSG 74	   /* 错误的消息 */
#define EOVERFLOW 75	   /* 值过大 */
#define ENOTUNIQ 76	   /* 名称在网络上不唯一 */
#define EBADFD 77	   /* 文件描述符处于错误状态 */
#define EREMCHG 78	   /* 远程地址已更改 */
#define ELIBACC 79	   /* 无法访问所需的共享库 */
#define ELIBBAD 80	   /* 访问损坏的共享库 */
#define ELIBSCN 81	   /* .lib段损坏 */
#define ELIBMAX 82	   /* 链接共享库过多 */
#define ELIBEXEC 83	   /* 无法直接执行共享库 */
#define EILSEQ 84	   /* 非法字节序列 */
#define ERESTART 85	   /* 应该重新启动 */
#define ESTRPIPE 86	   /* 流管道错误 */
#define EUSERS 87	   /* 用户太多 */
#define ENOTSOCK 88	   /* 套接字操作在非套接字上 */
#define EDESTADDRREQ 89	   /* 需要目标地址 */
#define EMSGSIZE 90	   /* 消息过大 */
#define EPROTOTYPE 91	   /* 协议类型错误 */
#define ENOPROTOOPT 92	   /* 协议不可用 */
#define EPROTONOSUPPORT 93 /* 协议不支持 */
#define ESOCKTNOSUPPORT 94 /* 套接字类型不支持 */
#define EOPNOTSUPP 95	   /* 操作不支持 */
#define EPFNOSUPPORT 96	   /* 协议族不支持 */
#define EAFNOSUPPORT 97	   /* 地址族不支持 */
#define EADDRINUSE 98	   /* 地址已在使用 */
#define EADDRNOTAVAIL 99   /* 无法分配请求的地址 */
#define ENETDOWN 100	   /* 网络已关闭 */
#define ENETUNREACH 101	   /* 网络不可达 */
#define ENETRESET 102	   /* 网络连接被重置 */
#define ECONNABORTED 103   /* 连接被中止 */
#define ECONNRESET 104	   /* 连接被重置 */
#define ENOBUFS 105	   /* 缓冲区空间不足 */
#define EISCONN 106	   /* 传输端点已连接 */
#define ENOTCONN 107	   /* 传输端点未连接 */
#define ESHUTDOWN 108	   /* 传输端点关闭后无法发送 */
#define ETOOMANYREFS 109   /* 引用过多 */
#define ETIMEDOUT 110	   /* 连接超时 */
#define ECONNREFUSED 111   /* 连接被拒绝 */
#define EHOSTDOWN 112	   /* 主机已关闭 */
#define EHOSTUNREACH 113   /* 主机不可达 */
#define EALREADY 114	   /* 连接已在进行中 */
#define EINPROGRESS 115	   /* 操作正在进行中 */
#define ESTALE 116	   /* 磁带已过期 */
#define EUCLEAN 117	   /* 结构需要清理 */
#define ENOTNAM 118	   /* 不是XENIX命名类型文件 */
#define ENAVAIL 119	   /* 没有可用的XENIX信号量 */
#define EISNAM 120	   /* 是命名类型文件 */
#define EREMOTEIO 121	   /* 远程I/O错误 */
#define EDQUOT 122	   /* 超出磁盘配额 */

/* 内核特定错误代码 */
#define ENOMEDIUM 123	/* 无介质 */
#define EMEDIUMTYPE 124 /* 介质类型错误 */

/*
 * 错误处理宏
 */
#define IS_ERR(ptr)                                                            \
	(unlikely((unsigned long)(ptr) >= (unsigned long)-MAX_ERRNO))
#define PTR_ERR(ptr) ((long)(ptr))
#define PTR(err) ((void *)((long)(err)))
#define PTR_ERR_OR_ZERO(ptr) (IS_ERR(ptr) ? PTR_ERR(ptr) : 0)

/* 最大错误号 */
#define MAX_ERRNO 4095

/*
 * 错误信息结构体
 */
struct error_info {
	int err_code;
	const char *err_name;
	const char *err_desc;
};

/*
 * 函数声明
 */
const char *strerror(int errnum);
const char *get_error_name(int errnum);
void print_error(const char *prefix, int errnum);
