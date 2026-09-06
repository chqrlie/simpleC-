#ifndef LIB_FCNTL_H
#define LIB_FCNTL_H

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
int creat(const char *path, int mode);
int open(const char *path, int flags, ...);
#endif
