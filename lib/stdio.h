#ifndef LIB_STDIO_H
#define LIB_STDIO_H
#ifndef NULL
#define NULL  ((void*)0)
#endif
#ifndef attr_printf
#define attr_printf(a, b)
#endif
#define _IOFBF   0
#define _IOLBF   1
#define _IONBF   2
#define BUFSIZ  4096
typedef struct FILE {
    int hd;
    unsigned char bmode, flags, alloc;
    size_t size, pos, cap, len;
    unsigned char *buf;
} FILE;
#define NFILE 20
extern FILE _iob[NFILE];
#define stdin  (&_iob[0])
#define stdout (&_iob[1])
#define stderr (&_iob[2])
#define EOF   (-1)
int fclose(FILE *fp);
FILE *fopen(const char *filename, char *mode);
int setvbuf(FILE *fp, char *buf, int mode, size_t size);
int _filbuf(FILE *fp);
#define getc(fp)  (((fp)->pos < (fp)->len) ? (fp)->buf[(fp)->pos++] : _filbuf(fp))
#define getchar(c)  getc(stdin)
int fgetc(FILE *fp);
char *fgets(char *buf, size_t n, FILE *fp);
size_t fread(void *p, size_t size, size_t nmemb, FILE *fp);
int _flsbuf(int c, FILE *fp);
#define putc(c, fp)  (((fp)->pos < (fp)->cap) ? (fp)->buf[(fp)->pos++] = (unsigned char)(c) : _flsbuf(c, fp))
#define putchar(c)  putc(c, stdout)
int fputc(int c, FILE *fp);
int puts(const char *s);
int fputs(const char *s, FILE *fp);
size_t fwrite(const void *p, size_t size, size_t nmemb, FILE *fp);
int fflush(FILE *fp);
int printf(const char *fmt, ...) attr_printf(1, 2);
int fprintf(FILE *fp, const char *fmt, ...) attr_printf(2, 3);
int vfprintf(FILE *fp, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) attr_printf(3, 4);
void perror(const char *s);
#endif
