// selfhost-shim.h — declarations only, no code.
//
// simpleC++.c is written against a hosted C library. nano_cc has no <stdio.h>
// to read, so `make selfhost` swaps the five system includes for this file:
// it declares the library surface the compiler uses, in the C subset nano_cc
// understands, and declares NOTHING else.
//
// The point is to separate two very different questions that "can it compile
// itself?" runs together:
//
//   1. Can nano_cc PARSE AND GENERATE CODE for every construct in its own
//      source? That is a compiler question, and the answer is yes.
//   2. Does a freestanding C library exist for it to link against? That is a
//      library question, and the answer is not yet -- `make selfhost` prints
//      the exact list of what is still missing.
//
// So this file is a measuring instrument, not a step towards a real bootstrap.
// The real bootstrap needs these implemented on raw syscalls, the way
// nano-nolibc.h already does write/open/close/exit/printf.

#ifndef SELFHOST_SHIM_H
#define SELFHOST_SHIM_H

#define NULL 0
#define EOF (-1)

typedef long size_t;
typedef struct FILE FILE;

FILE *stderr;
FILE *stdout;

// ---- stdio ----
FILE *fopen(char *path, char *mode);
int   fclose(FILE *f);
int   fgetc(FILE *f);
int   fputs(char *s, FILE *f);
int   fputc(int c, FILE *f);
void  fprintf(FILE *f, char *fmt, ...);
void  printf(char *fmt, ...);
int   snprintf(char *b, long n, char *fmt, ...);
void  perror(char *m);

// ---- stdlib ----
void *malloc(long n);
void *calloc(long n, long m);
void  free(void *p);
void  exit(int code);

// ---- string ----
int   strcmp(char *a, char *b);
long  strlen(char *s);
void *memcpy(void *d, void *s, long n);
void *memmove(void *d, void *s, long n);

// ---- ctype ----
int   isalpha(int c);
int   isalnum(int c);
int   isdigit(int c);
int   isxdigit(int c);
int   isspace(int c);
int   tolower(int c);

// ---- stdarg ----
// Same shape nano-nolibc.h uses: the va_list is a pointer into the register
// save area and the compiler provides the three builtins.
#define va_list         long
#define va_start(ap, l) __builtin_va_start(ap)
#define va_arg(ap, t)   __builtin_va_arg(ap)
#define va_end(ap)      __builtin_va_end(ap)
void vfprintf(FILE *f, char *fmt, long ap);

#endif
