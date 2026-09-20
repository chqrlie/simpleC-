#ifndef LIB_UNISTD_H
#define LIB_UNISTD_H
ssize_t read(int fd, void *buf, size_t len);
ssize_t write(int fd, const void *buf, size_t len);
int close(int fd);
int isatty(int fd);
long syscall(long number, ...);
typedef int pid_t;
pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
#endif
