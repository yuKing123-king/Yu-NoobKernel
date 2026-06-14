#pragma once

#include <misc/stdint.h>
#include <misc/stddef.h>

typedef u32 umode_t;
typedef u32 uid_t;
typedef u32 gid_t;
typedef s64 loff_t;
typedef u64 time_t;

#define S_IFMT 0170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000

#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000

#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100

#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010

#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

#define O_RDONLY 00000000
#define O_WRONLY 00000001
#define O_RDWR 00000002
#define O_ACCMODE 00000003
#define O_CREAT 00000100
#define O_CREATE 00000100
#define O_EXCL 00000200
#define O_NOCTTY 00000400
#define O_TRUNC 00001000
#define O_APPEND 00002000
#define O_NONBLOCK 00004000
#define O_DSYNC 00010000
#define O_DIRECTORY 00200000
#define O_NOFOLLOW 00400000
#define O_SYNC 04000000
#define O_LARGEFILE 000010000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14

#define AT_FDCWD (-100)

#define NAME_MAX 255
#define PATH_MAX 4096

struct qstr {
	char *name;
	u32 len;
	u32 hash;
};

struct dirent {
	u64 d_ino;
	u64 d_off;
	u16 d_reclen;
	u8 d_type;
	char d_name[NAME_MAX + 1];
};

struct statfs {
	u64 f_type;
	u64 f_bsize;
	u64 f_blocks;
	u64 f_bfree;
	u64 f_bavail;
	u64 f_files;
	u64 f_ffree;
	u64 f_fsid;
	u64 f_namelen;
	u64 f_frsize;
	u64 f_flags;
};

static inline u32 hash_string(const char *str, u32 len)
{
	u32 hash = 0;
	for (u32 i = 0; i < len; i++) {
		hash = hash * 31 + (u32)str[i];
	}
	return hash;
}

static inline void qstr_init(struct qstr *qstr, char *name)
{
	qstr->name = name;
	qstr->len = 0;
	while (name[qstr->len])
		qstr->len++;
	qstr->hash = hash_string(name, qstr->len);
}
