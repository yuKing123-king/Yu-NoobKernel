#include <misc/errno.h>
#include <misc/log.h>

/*
 * 错误信息表
 */
static const struct error_info error_table[] = {
	{ OK, "ERR_OK", "Success" },
	{ EPERM, "ERR_EPERM", "Operation not permitted" },
	{ ENOENT, "ERR_ENOENT", "No such file or directory" },
	{ ESRCH, "ERR_ESRCH", "No such process" },
	{ EINTR, "ERR_EINTR", "Interrupted system call" },
	{ EIO, "ERR_EIO", "Input/output error" },
	{ ENXIO, "ERR_ENXIO", "No such device or address" },
	{ E2BIG, "ERR_E2BIG", "Argument list too long" },
	{ ENOEXEC, "ERR_ENOEXEC", "Exec format error" },
	{ EBADF, "ERR_EBADF", "Bad file descriptor" },
	{ ECHILD, "ERR_ECHILD", "No child processes" },
	{ EAGAIN, "ERR_EAGAIN", "Resource temporarily unavailable" },
	{ ENOMEM, "ERR_ENOMEM", "Cannot allocate memory" },
	{ EACCES, "ERR_EACCES", "Permission denied" },
	{ EFAULT, "ERR_EFAULT", "Bad address" },
	{ ENOTBLK, "ERR_ENOTBLK", "Block device required" },
	{ EBUSY, "ERR_EBUSY", "Device or resource busy" },
	{ EEXIST, "ERR_EEXIST", "File exists" },
	{ EXDEV, "ERR_EXDEV", "Invalid cross-device link" },
	{ ENODEV, "ERR_ENODEV", "No such device" },
	{ ENOTDIR, "ERR_ENOTDIR", "Not a directory" },
	{ EISDIR, "ERR_EISDIR", "Is a directory" },
	{ EINVAL, "ERR_EINVAL", "Invalid argument" },
	{ ENFILE, "ERR_ENFILE", "Too many open files in system" },
	{ EMFILE, "ERR_EMFILE", "Too many open files" },
	{ ENOTTY, "ERR_ENOTTY", "Inappropriate ioctl for device" },
	{ ETXTBSY, "ERR_ETXTBSY", "Text file busy" },
	{ EFBIG, "ERR_EFBIG", "File too large" },
	{ ENOSPC, "ERR_ENOSPC", "No space left on device" },
	{ ESPIPE, "ERR_ESPIPE", "Illegal seek" },
	{ EROFS, "ERR_EROFS", "Read-only file system" },
	{ EMLINK, "ERR_EMLINK", "Too many links" },
	{ EPIPE, "ERR_EPIPE", "Broken pipe" },
	{ EDOM, "ERR_EDOM", "Numerical argument out of domain" },
	{ ERANGE, "ERR_ERANGE", "Numerical result out of range" },
	{ EDEADLK, "ERR_EDEADLK", "Resource deadlock avoided" },
	{ ENAMETOOLONG, "ERR_ENAMETOOLONG", "File name too long" },
	{ ENOLCK, "ERR_ENOLCK", "No locks available" },
	{ ENOSYS, "ERR_ENOSYS", "Function not implemented" },
	{ ENOTEMPTY, "ERR_ENOTEMPTY", "Directory not empty" },
	{ ELOOP, "ERR_ELOOP", "Too many levels of symbolic links" },
	{ ENOMSG, "ERR_ENOMSG", "No message of desired type" },
	{ EIDRM, "ERR_EIDRM", "Identifier removed" },
};

#define ERROR_TABLE_SIZE (sizeof(error_table) / sizeof(error_table[0]))

/*
 * 根据错误号返回错误描述
 */
const char *strerror(int errnum) {
	unsigned int i;

	if (errnum < 0)
		errnum = -errnum;

	for (i = 0; i < ERROR_TABLE_SIZE; i++) {
		if (error_table[i].err_code == errnum)
			return error_table[i].err_desc;
	}

	return "Unknown error";
}

/*
 * 根据错误号返回错误名称
 */
const char *get_error_name(int errnum) {
	unsigned int i;

	if (errnum < 0)
		errnum = -errnum;

	for (i = 0; i < ERROR_TABLE_SIZE; i++) {
		if (error_table[i].err_code == errnum)
			return error_table[i].err_name;
	}

	return "ERR_UNKNOWN";
}

/*
 * 打印错误信息
 */
void print_error(const char *prefix, int errnum) {
	if (prefix) {
		errorf("%s: %s (%s)\n", prefix, strerror(errnum),
		       get_error_name(errnum));
	} else {
		errorf("%s (%s)\n", strerror(errnum), get_error_name(errnum));
	}
}
