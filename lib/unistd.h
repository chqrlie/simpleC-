#ifndef LIB_UNISTD_H
#define LIB_UNISTD_H
ssize_t read(int fd, void *buf, size_t len);
ssize_t write(int fd, const void *buf, size_t len);
int close(int fd);
int isatty(int fd);
long syscall(long number, ...);
#endif
