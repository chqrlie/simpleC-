#ifndef LIB_STRING_H
#define LIB_STRING_H
#ifndef NULL
#define NULL  ((void*)0)
#endif
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *p1, const void *p2, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strcpy(char *d, const char *s);
int strcmp(const char *a, const char *b);
const char *strerror(int errnum);
char *strdup(const char *s);
#endif
