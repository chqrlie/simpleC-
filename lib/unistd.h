#ifndef LIB_UNISTD_H
#define LIB_UNISTD_H
typedef unsigned gid_t;
typedef long long off_t;
typedef int pid_t;
typedef unsigned uid_t;
typedef unsigned useconds_t;
int access(const char *name, int flags);
#define R_OK  4
#define W_OK  2
#define X_OK  1
#define F_OK  0
int chdir(const char* path);
int chown(const char* file, uid_t owner, gid_t group);
int close(int fd);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int execve(const char *path, char *const argv[], char *const envp[]);
pid_t fork(void);
char* getcwd(char* buf, size_t size);
pid_t getpid(void);
int isatty(int fd);
int link(const char* from, const char* to);
off_t lseek(int fd, off_t offset, int whence);
int pipe(int* pipedes);
ssize_t read(int fd, void *buf, size_t len);
int rmdir(const char* path);
unsigned sleep(unsigned seconds);
void sync(void);
long syscall(long number, ...);
int truncate(const char* path, off_t offset);
int unlink(const char* name);
int usleep(useconds_t useconds);
ssize_t write(int fd, const void *buf, size_t len);
#endif
