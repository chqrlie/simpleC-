#ifndef LIB_ERRNO_H
#define LIB_ERRNO_H
enum {  // Linux error codes
    EPERM = 1, ENOENT, ESRCH, EINTR, EIO, ENXIO, E2BIG, ENOEXEC, EBADF,
    ECHILD, EAGAIN, ENOMEM, EACCES, EFAULT, ENOTBLK, EBUSY, EEXIST, EXDEV,
    ENODEV, ENOTDIR, EISDIR, EINVAL, ENFILE, EMFILE, ENOTTY, ETXTBSY, EFBIG,
    ENOSPC, ESPIPE, EROFS, EMLINK, EPIPE, EDOM, ERANGE,
};
extern thread_local int errno;
extern const int sys_nerr;
extern const char * const sys_errlist[];
#endif
