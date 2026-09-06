#ifndef LIB_STDLIB_H
#define LIB_STDLIB_H
#ifndef NULL
#define NULL  ((void*)0)
#endif
_Noreturn void exit(int code);
long strtol(const char *s, char **endp, int base);
int atoi(const char *s);
void *malloc(size_t size);
void free(void *p);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *p, size_t size);
void malloc_stats(void);
#endif
