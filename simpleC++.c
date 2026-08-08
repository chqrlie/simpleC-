// =====================================================================
// simpleC++  (nano_cc) — a tiny single-file C-subset -> x86_64 compiler
// =====================================================================
// Emits GNU-as compatible x86_64 assembly (.intel_syntax noprefix) that
// links with:  gcc -nostdlib -no-pie out.s -o prog
//
// Supported subset:
//   * preprocessor: #include "...", #define (object macros),
//                   #ifndef/#ifdef/#else/#endif include guards,
//                   // and /* */ comments
//   * types: int, long, char, void, pointers, arrays, struct/union,
//            const/unsigned qualifiers (parsed, ignored), static/inline
//   * expressions: + - * / %, < > <= >= == !=, && ||, unary - ! * &,
//                  prefix & postfix ++/--, ternary ?:, casts, sizeof,
//                  function calls, indexing a[i], member access . and ->,
//                  assignment and compound assignment (+= -= *= /= %=)
//   * statements: if/else, while, for, do/while, break, continue, return,
//                 blocks, __asm__("..."), declarations (comma lists + init)
//
// Build:  gcc -std=c11 -O2 -Wall -Wextra -o nano_cc 'simpleC++.c'
// Use:    ./nano_cc input.c output.s
// =====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>

#if (defined(__GNUC__) || defined(__TINYC__))
#define attr_printf(a, b)  __attribute__((format(printf, a, b)))
#else
#define attr_printf(a, b)
#endif

#if 1
#else
// stdarg.h
#define va_list         long
#define va_start(ap, l) __builtin_va_start(ap)
#define va_arg(ap, t)   __builtin_va_arg(ap)
#define va_end(ap)      __builtin_va_end(ap)

// stdbool.h (should make these keywords)
typedef unsigned char bool;
#define true 1
#define false 0

// stddef.h
typedef unsigned long size_t;

// stdio.h
typedef struct FILE FILE;
struct FILE {
    int dummy;
};
extern FILE stdin[1];
extern FILE stdout[1];
extern FILE stderr[1];
#define NULL  ((void*)0)
#define EOF   (-1)
int fclose(FILE *stream);
FILE *fopen(const char *filename, const char *mode);
int fprintf(FILE *stream, const char *format, ...);
int printf(const char *format, ...);
int vfprintf(FILE *srtream, const char *format, va_list arg);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
int fputs(const char *s, FILE *stream);
void perror(const char *s);

// ctype.h
int isalnum(int c);
int isalpha(int c);
int isdigit(int c);
int isspace(int c);
int isxdigit(int c);
int tolower(int c);

// stdlib.h
void *calloc(size_t nmemb, size_t size);
void *malloc(size_t size);
void *realloc(void *p, size_t size);
void free(void *p);
#define _Noreturn
_Noreturn void exit(int status);

// string.h
void *memcpy(void *s1, const void *s2, size_t n);
int memcmp(void *s1, const void *s2, size_t n);
int strcmp(const char *s1, const char *s2);
char *strchr(const char *s, int c);
char *strcpy(char *s1, const char *s2);
size_t strlen(const char *s);
#endif

// ---------------------------------------------------------------------
// Output / errors / allocation
// ---------------------------------------------------------------------
static const char *progname;
static const char *filename;
static int lineno;
static int optimize;
static bool verbose;
static FILE *fout;
static void err_message(int pos, const char *kind, const char *fmt, va_list ap) {
    if (filename && pos) fprintf(stderr, "%s:%d: %s: ", filename, pos, kind);
    else fprintf(stderr, "%s: %s: ", progname, kind);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}
static void die(const char *fmt, ...) attr_printf(1,2);
static void die(const char *fmt, ...) {
    va_list a; va_start(a, fmt); err_message(lineno, "error", fmt, a); va_end(a);
    exit(1);
}
static void warning(int pos, const char *fmt, ...) attr_printf(2,3);
static void warning(int pos, const char *fmt, ...) {
    va_list a; va_start(a, fmt); err_message(pos, "warning", fmt, a); va_end(a);
}
static void emit(const char *fmt, ...) attr_printf(1,2);
static void emit(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(fout, fmt, a); va_end(a);
    fputc('\n', fout);
}
static int xdigit(int d) { return (d <= '9') ? d - '0' : tolower(d) - 'a' + 10; }
static int skip_blanks(const char *s) {
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    return i;
}
static int trim_len(const char *s) {
    int i = strlen(s);
    while (i && (s[i - 1] == ' ' || s[i - 1] == '\t')) i--;
    return i;
}
static int skip_word(const char *s) {
    int i = 0;
    while (isalnum((unsigned char)s[i]) || s[i] == '_') i++;
    return i;
}
static int pstrcpy(char *dest, int size, const char *src) {
    if (size) {
        for (int i = 0; i < size; i++) if ((dest[i] = src[i]) == 0) return i;
        dest[size - 1] = 0;
    }
    return size;
}

static void *alloc(size_t nelems, size_t size) {
    void *ptr = malloc(nelems * size);
    if (!ptr) die("out of memory");
    return ptr;
}
static void *allocz(size_t nelems, size_t size) {
    void *ptr = calloc(nelems, size);
    if (!ptr) die("out of memory");
    return ptr;
}
static void *reallocate(void *ptr, size_t *nelems, size_t size) {
    size_t new_nelems = *nelems + (*nelems >> 1);
#if 1    // XXX use realloc if available
    void *new_ptr = realloc(ptr, new_nelems * size);
    if (!new_ptr) die("out of memory");
#else
    void *new_ptr = alloc(new_nelems, size);
    if (ptr) { memcpy(new_ptr, ptr, *nelems * size); free(ptr); }
#endif
    *nelems = new_nelems;
    return new_ptr;
}

// =====================================================================
// 0.1. ATOM TABLE -> convert names and strings to single atoms
// =====================================================================

typedef unsigned int atom_t;
#define ATOM_MACRO 1
#define ATOM_USED  2
typedef struct Atom { atom_t next; int len; char flags; char str[7]; } Atom;
static Atom **atoms;
static size_t natoms, atoms_cap;
#define ATOM_HASH_LEN  1023
static atom_t atom_hash[ATOM_HASH_LEN];

const char *atom_str(atom_t i) { return atoms[i]->str; }
int atom_len(atom_t i) { return atoms[i]->len; }
#define atom_flags(i)  atoms[i]->flags
atom_t new_atom_len(const char *str, int len) {
    if (!atoms) {
        atoms = alloc(atoms_cap = 1024, sizeof(Atom*));
        atoms[0] = allocz(1, sizeof(Atom)); natoms = 1;
    }
    if (!len) { return 0; } // special case the empty string
    unsigned int hash = 0;
    for (int i = 0; i < len; i++) hash = hash * 37 + str[i];
    hash %= ATOM_HASH_LEN;
    atom_t a = atom_hash[hash];
    while (a) {
        Atom *ap = atoms[a];
        if (ap->len == len && !memcmp(ap->str, str, len)) return a;
        a = ap->next;
    }
    if (natoms >= atoms_cap)
        atoms = reallocate(atoms, &atoms_cap, sizeof(Atom*));
    Atom *ap = alloc(1, sizeof(Atom) - 7 + len + 1);
    memcpy(ap->str, str, len);
    ap->str[len] = 0;
    ap->len = len;
    ap->next = atom_hash[hash];
    a = natoms++;
    atom_hash[hash] = a;
    atoms[a] = ap;
    return a;
}
atom_t new_atom(const char *str) { return new_atom_len(str, strlen(str)); }

// =====================================================================
// 0.2. PREDEFINED ATOMS
// =====================================================================
enum {
    T_EMPTY, T_EOF, T_NUM, T_ID, T_STR, T_CHAR,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_BITOR, T_AMP, T_BITXOR, T_SHL, T_SHR,
    T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE,
    T_ANDAND, T_OROR, T_NOT, T_BITNOT, T_INC, T_DEC,
    T_ASSIGN,
    // combined assignment operators must be in the same order as operators
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_OREQ, T_ANDEQ, T_XOREQ, T_SHLEQ, T_SHREQ,
    T_LP, T_RP, T_LBRK, T_RBRK, T_LBRACE, T_RBRACE,
    T_SEMI, T_COMMA, T_QUESTION, T_COLON, T_DOT, T_ARROW, T_ELLIPSIS,

    K_INT, K_LONG, K_CHAR, K_VOID,
    K_IF, K_ELSE, K_WHILE, K_RETURN, K_ASM, K__ASM__,
    K_FOR, K_DO, K_BREAK, K_CONTINUE, K_STRUCT, K_UNION, K_SIZEOF,
    K_SWITCH, K_CASE, K_DEFAULT, K_GOTO, K_TYPEDEF, K_ENUM,

    K_CONST, K_UNSIGNED, K_SIGNED, K_STATIC, K_INLINE,
    K_REGISTER, K_VOLATILE, K_EXTERN,

    K_IFDEF, K_IFNDEF, K_ELIF, K_ENDIF, K_DEFINE, K_UNDEF, K_INCLUDE,
    ID__BUILTIN_VA_START, ID__BUILTIN_VA_ARG, ID__BUILTIN_VA_END,
    ID_MAIN, ID_PRINTF, ID_PUTS, ID_STRLEN, ID_STRCPY, ID_MEMCPY,
    T_count
};
static const char *token_name[T_count] = {
    "", "<EOF>", "number", "identifier", "string", "char const",
    "+", "-", "*", "/", "%", "|", "&", "^", "<<", ">>",
    "==", "!=", "<", ">", "<=", ">=",
    "&&", "||", "!", "~", "++", "--",
    "=", "+=", "-=", "*=", "/=", "%=", "|=", "&=", "^=", "<<=", ">>=",
    "(", ")", "[", "]", "{", "}",
    ";", ",", "?", ":", ".", "->", "...",
    "int", "long", "char", "void",
    "if", "else", "while", "return", "asm", "__asm__",
    "for", "do", "break", "continue", "struct", "union", "sizeof",
    "switch", "case", "default", "goto", "typedef", "enum",
    "const", "unsigned", "signed", "static", "inline",
    "register", "volatile", "extern",
    "ifdef", "ifndef", "elif", "endif", "define", "undef", "include",
    "__builtin_va_start", "__builtin_va_arg", "__builtin_va_end",
    "main", "printf", "puts", "strlen", "strcpy", "memcpy",
};

static void lex_init(void) {
    for (atom_t i = 0; i < T_count; i++) {
        if (new_atom(token_name[i]) != i)
            die("atom definition failure for '%s'", token_name[i]);
    }
}

// =====================================================================
// 1. PREPROCESSOR  ->  produces a single clean source buffer in SRC
// =====================================================================
#define MAX_SRC   400000
static char SRC[MAX_SRC];
static int  SRC_LEN;

// A macro is either object-like (#define PI 3) or function-like
// (#define MAX(a,b) ((a)>(b)?(a):(b))).  For function-like macros we keep
// the parameter names so expand_line() can substitute call arguments.
typedef struct {
    atom_t name; int nparams; atom_t params[8]; char value[512];
} Macro;
#define MAX_MACROS 1024
static Macro macros[MAX_MACROS];
static int   macro_cnt;

static Macro *macro_find(atom_t name) {
    if (atom_flags(name) & ATOM_MACRO) {
        for (int i = 0; i < macro_cnt; i++)
            if (macros[i].name == name) return &macros[i];
    }
    return NULL;
}
// Find-or-create a macro slot, reset to a clean object-like state.
static Macro *macro_intern(atom_t name) {
    Macro *m = macro_find(name);
    if (!m) { m = &macros[macro_cnt++]; }
    atom_flags(name) |= ATOM_MACRO;
    m->name = name;
    m->value[0] = 0; m->nparams = -1;
    return m;
}
static void macro_undef(atom_t name) {
    Macro *m = macro_find(name);
    if (m) {
        atom_flags(name) &= ~ATOM_MACRO;
        m->name = 0;
    }
}

static void src_putc(char c) {
    if (SRC_LEN >= MAX_SRC - 1) die("source too large");
    SRC[SRC_LEN++] = c;
}
static void src_puts(const char *s) { while (*s) src_putc(*s++); }

// Substitute a function-like macro body: copy m->value into out[], replacing
// each parameter name with the matching call argument text.  argv[k] holds the
// (already whitespace-trimmed) text of the k-th argument.
static int subst_macro_body(const Macro *m, char argv[8][512], int argc,
                            char *out, int j, int cap) {
    #define PUT(ch) do { if (j < cap - 1) out[j++] = (ch); } while (0)
    const char *p = m->value;
    while (*p) {
        char vc = *p;
        if (isalpha((unsigned char)vc) || vc == '_') {
            int k = skip_word(p);
            atom_t w = new_atom_len(p, k);
            int pi = -1;
            for (int a = 0; a < m->nparams; a++) if (w == m->params[a]) { pi = a; break; }
            if (pi >= 0 && pi < argc) { for (const char *s = argv[pi]; *s; s++) PUT(*s); }
            else                      { for (int i = 0; i < k; i++) PUT(p[i]); }
            p += k;
        } else {
            PUT(vc); p++;
        }
    }
    #undef PUT
    return j;
}

// Expand object-like and simple function-like macros in one logical line.
static void expand_line(const char *in) {
    char work[8192];
    pstrcpy(work, sizeof(work), in);
    for (int pass = 0; pass < 8; pass++) {
        char out[8192]; int j = 0, changed = 0;
        #define OPUT(ch) do { if (j < (int)sizeof(out) - 1) out[j++] = (ch); } while (0)
        for (int i = 0; work[i]; ) {
            char c = work[i];
            if (c == '"' || c == '\'') {          // copy string/char literal verbatim
                char q = c; OPUT(c); i++;
                while (work[i] && work[i] != q) {
                    if (work[i] == '\\' && work[i+1]) { OPUT(work[i]); i++; }
                    OPUT(work[i]); i++;
                }
                if (work[i]) { OPUT(work[i]); i++; }
            } else if (isalpha((unsigned char)c) || c == '_') {
                int k = i; i += skip_word(work + i);
                atom_t word = new_atom_len(&work[k], i - k);
                Macro *m = macro_find(word);
                if (m && m->nparams >= 0) { // is_func
                    int t = i; t += skip_blanks(work + t);
                    if (work[t] == '(') {          // it's a macro invocation
                        t++;                       // past '('
                        // XXX: This does not work if macro arguments span multiple lines
                        char argv[8][512]; int argc = 0, ci = 0; argv[0][0] = 0;
                        int depth = 1;
                        // XXX: this does not work if macro argument is a string or char constant
                        while (work[t] && depth > 0) {
                            char cc = work[t];
                            if (cc == '(') { depth++; if (argc < 8 && ci < 511) argv[argc][ci++] = cc; t++; }
                            else if (cc == ')') { depth--; if (depth > 0) { if (argc < 8 && ci < 511) argv[argc][ci++] = cc; } t++; }
                            else if (cc == ',' && depth == 1) { if (argc < 8) { argv[argc][ci] = 0; argc++; } ci = 0; if (argc < 8) argv[argc][0] = 0; t++; }
                            else { if (argc < 8 && ci < 511) argv[argc][ci++] = cc; t++; }
                        }
                        if (argc < 8) { argv[argc][ci] = 0; argc++; }
                        // trim leading/trailing whitespace on each argument
                        for (int a = 0; a < argc; a++) {
                            char *s = argv[a];
                            int st = skip_blanks(s);
                            int len = trim_len(s + st);
                            if (st) { for (int i = 0; i < len; i++) s[i] = s[st + i]; }
                            s[len] = 0;
                        }
                        // empty call MACRO()  ->  zero arguments
                        if (argc == 1 && argv[0][0] == 0 && m->nparams == 0) argc = 0;
                        j = subst_macro_body(m, argv, argc, out, j, (int)sizeof(out));
                        i = t;                     // consume through ')'
                        changed = 1;
                    } else {                       // name not followed by '(' -> literal
                        while (k < i) { OPUT(work[k]); k++; }
                    }
                } else if (m) {                    // object-like macro
                    for (const char *p = m->value; *p; p++) OPUT(*p);
                    changed = 1;
                } else {
                    while (k < i) { OPUT(work[k]); k++; }
                }
            } else {
                OPUT(work[i]); i++;
            }
            if (j >= (int)sizeof(out) - 2) break;
        }
        #undef OPUT
        out[j] = 0;
        memcpy(work, out, j + 1);
        if (!changed) break;
    }
    src_puts(work);
}

// Strip // and /* */ comments (string/char aware) into `dst`.
static char *strip_comments(const char *s, int len) {
    char *dst = alloc(len + 1, 1);
    int j = 0;
    for (int i = 0; s[i]; ) {
        if (s[i] == '"' || s[i] == '\'') {
            char q = s[i]; dst[j++] = s[i++];
            while (s[i] && s[i] != q) {
                if (s[i] == '\\' && s[i+1]) dst[j++] = s[i++];
                dst[j++] = s[i++];
            }
            if (s[i]) dst[j++] = s[i++];
        } else if (s[i] == '/' && s[i+1] == '/') {
            while (s[i] && s[i] != '\n') i++;
        } else if (s[i] == '/' && s[i+1] == '*') {
            i += 2;
            while (s[i] && !(s[i] == '*' && s[i+1] == '/')) if (s[i++] == '\n') dst[j++] = '\n';
            if (s[i]) i += 2;
            if (j && !isspace(dst[j-1])) dst[j++] = ' ';
        } else {
            dst[j++] = s[i++];
        }
    }
    dst[j] = 0;
    if (verbose) printf("Stripped: %d bytes\n", j);
    return dst;
}

static void preprocess(const char *path);   // fwd

// Process already-comment-stripped text of one file.
// NB: iterate lines manually (no strtok) so nested #include recursion is safe.
static void process_text(const char *text) {
    int cond_sp = 0, skip = 0, seen_else = 0;

    lineno = 1;
    const char *cursor = text;
    while (*cursor) {
        char line[8192]; int li = 0;
        while (*cursor && *cursor != '\r' && *cursor != '\n') {
            if (li >= (int)sizeof(line)) die("line too long");
            line[li++] = *cursor++;
        }
        line[li] = 0;
        if (*cursor == '\r') cursor++;
        if (*cursor == '\n') cursor++;

        const char *p = line; p += skip_blanks(p);
        if (*p == '#') {
            p++;
            p += skip_blanks(p);
            int di = skip_word(p);
            atom_t dir = new_atom_len(p, di);
            p += di;
            p += skip_blanks(p);
            switch (dir) {
            case K_IFNDEF: case K_IFDEF:
                cond_sp++;
                skip += skip;
                seen_else += seen_else;
                if (!skip) {
                    int k = skip_word(p);
                    if (!k) die("expected macro name after '#%s'", atom_str(dir));
                    atom_t nm = new_atom_len(p, k);
                    int defined = macro_find(nm) != NULL;
                    skip |= (dir == K_IFDEF) ? !defined : defined;
                    p += k;
                }
                break;
            case K_IF:  // unsupported #if -> skip body
                cond_sp++;
                skip += skip + 1;
                seen_else += seen_else;
                break;
            case K_ELIF: // unsupported #if -> skip body
                if (!cond_sp) die("'#%s' without a '#if'", atom_str(dir));
                if (seen_else & 1) die("'#%s' after '#else'", atom_str(dir));
                skip |= 1;
                break;
            case K_ELSE:
                if (!cond_sp) die("'#%s' without a '#if'", atom_str(dir));
                if (seen_else & 1) die("'#%s' after '#else'", atom_str(dir));
                skip ^= 1; seen_else |= 1;
                break;
            case K_ENDIF:
                if (!cond_sp) die("'#%s' without a '#if'", atom_str(dir));
                cond_sp--;
                skip >>= 1; seen_else >>= 1;
                break;
            case K_DEFINE:
                if (!skip) {
                    int k = skip_word(p);
                    if (!k) die("expected macro name after '#%s'", atom_str(dir));
                    atom_t nm = new_atom_len(p, k);
                    Macro *m = macro_intern(nm);
                    p += k;
                    // function-like macro: '(' immediately after name (no space)
                    if (*p == '(') {
                        m->nparams = 0; p++;
                        p += skip_blanks(p);
                        if (*p && *p != ')') {
                            for (;;) {
                                int ai = skip_word(p);
                                if (!ai) die("expected parameter name for macro '%s'", atom_str(nm));
                                if (m->nparams >= 8) die("too many macro parameters for '%s'", atom_str(nm));
                                m->params[m->nparams++] = new_atom_len(p, ai);
                                p += ai;
                                p += skip_blanks(p);
                                if (*p == ')') break;
                                if (*p != ',') die("expected ',' or ')' after macro parameter name");
                                p++;
                                p += skip_blanks(p);
                            }
                        }
                        if (*p != ')') die("expected ')' after parameters of macro '%s'", atom_str(nm));
                        p++;
                    }
                    p += skip_blanks(p);
                    int len = trim_len(p);
                    if (len >= (int)sizeof(m->value)) { die("macro '%s' definition too long (max=%d)",
                                                            atom_str(nm), (int)sizeof(m->value) - 1); }
                    memcpy(m->value, p, len);
                    m->value[len] = 0;
                    p += len;
                }
                break;
            case K_UNDEF:
                if (!skip) {
                    int k = skip_word(p);
                    atom_t nm = new_atom_len(p, k);
                    macro_undef(nm);
                    p += k;
                }
                break;
            case K_INCLUDE:
                if (!skip) {
                    if (*p == '"') {
                        p++;
                        char fn[256]; int k = 0;
                        while (*p && *p != '"' && k < 255) fn[k++] = *p++;
                        fn[k] = 0;
                        preprocess(fn);
                    } else {
                        // <...> system includes are ignored (freestanding)
                    }
                }
                break;
            default:
                // ignore other preprocessing directives
                break;
            }
        } else {
            if (!skip) expand_line(line);
        }
        src_putc('\n'); lineno++; // preserve line numbers
    }
    if (cond_sp) warning(lineno, "missing '#endif'");
    SRC[SRC_LEN] = 0;
}

static void preprocess(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { die("%s: cannot open %s\n", progname, path); }
    size_t raw_cap = 64 * 1024;
    char *raw = alloc(raw_cap, 1);
    int c; size_t n = 0;
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > raw_cap) { raw = reallocate(raw, &raw_cap, 1); }
        raw[n++] = (char)c;
    }
    raw[n] = 0;
    if (verbose) printf("Read %s: %d bytes\n", path, (int)n);
    fclose(f);
    const char *save_filename = filename; int save_lineno = lineno;
    filename = path; lineno = 0;
    char *nocmt = strip_comments(raw, n);
    process_text(nocmt);                 // may recurse (with its own buffers)
    filename = save_filename; lineno = save_lineno;
    free(raw); free(nocmt);
    if (verbose) printf("Preprocessed: %d bytes\n", SRC_LEN);
}

// =====================================================================
// 2. LEXER
// =====================================================================

typedef struct {
    int   kind;
    int   lineno;
    long  ival;          // T_NUM / T_CHAR
    atom_t text;         // T_ID, T_STR (decoded bytes)
} Token;

#define MAX_TOK 60000
static Token toks[MAX_TOK];
static int   ntok;

static void add_tok(int kind) { toks[ntok].kind = kind; toks[ntok].lineno = lineno; ntok++; }
static const char *token_str(Token *t) {
    return atom_str(t->kind == T_ID ? t->text : t->kind);
}

#define sp SRC_INDEX        // avoid problem with generated assembly '[rip+sp]' considered invalid
static int  sp;                       // scan position in SRC
static void skip_space(void) { while (isspace((unsigned char)SRC[sp])) if (SRC[sp++] == '\n') lineno++; }

static int read_escape(void) {            // SRC[sp] points just past a backslash
    int c = (unsigned char)SRC[sp++];
    switch (c) {
    case 0: sp--; return 0;
    case 'n': return '\n';  case 't': return '\t';  case 'r': return '\r';
    case 'b': return '\b';  case 'f': return '\f';  case 'v': return '\v';
    case '\\': case '\'': case '"': return c;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        c -= '0';
        if (SRC[sp] >= '0' && SRC[sp] <= '7') {
            c = (c << 3) + (SRC[sp++] - '0');
            if (SRC[sp] >= '0' && SRC[sp] <= '7') {
                c = (c << 3) + (SRC[sp++] - '0');
            }
        }
        return c;
    case 'x':
        c = 0;
        while (isxdigit((unsigned char)SRC[sp])) c = c * 16 + xdigit((unsigned char)SRC[sp]);
        return c;
    default:
        warning(lineno, "invalid escape sequence '\\%c'", c);
        return c;
    }
}

static char const ops[] = {
#define ____ 0
    '(', T_LP,       ____,      ____,         ____,
    ')', T_RP,       ____,      ____,         ____,
    '[', T_LBRK,     ____,      ____,         ____,
    ']', T_RBRK,     ____,      ____,         ____,
    '{', T_LBRACE,   ____,      ____,         ____,
    '}', T_RBRACE,   ____,      ____,         ____,
    ';', T_SEMI,     ____,      ____,         ____,
    ',', T_COMMA,    ____,      ____,         ____,
    '<', T_LT,       T_SHL,     T_LE,         T_SHLEQ,
    '=', T_ASSIGN,   T_EQ,      ____,         ____,
    '>', T_GT,       T_SHR,     T_GE,         T_SHREQ,
    '+', T_PLUS,     T_INC,     T_PLUSEQ,     ____,
    '-', T_MINUS,    T_DEC,     T_MINUSEQ,    ____,
    '*', T_STAR,     ____,      T_STAREQ,     ____,
    '/', T_SLASH,    ____,      T_SLASHEQ,    ____,
    '%', T_PERCENT,  ____,      T_PERCENTEQ,  ____,
    '!', T_NOT,      ____,      T_NE,         ____,
    '~', T_BITNOT,   ____,      ____,         ____,
    '?', T_QUESTION, ____,      ____,         ____,
    ':', T_COLON,    ____,      ____,         ____,
    '&', T_AMP,      T_ANDAND,  T_ANDEQ,      ____,
    '|', T_BITOR,    T_OROR,    T_OREQ,       ____,
    '^', T_BITXOR,   ____,      T_XOREQ,      ____,
    0,
#undef ____
};

static void lex(void) {
    for (;;) {
        skip_space();
        char c = SRC[sp];
        if (c == 0) { add_tok(T_EOF); break; }

        if (isdigit((unsigned char)c)) {
            long v = 0;
            if (c == '0') {
                if (SRC[sp+1] == 'x' || SRC[sp+1] == 'X') {
                    sp += 2;
                    while (isxdigit((unsigned char)SRC[sp])) {
                        v = v * 16 + xdigit(SRC[sp++]);
                    }
                } else {
                    while (SRC[sp] >= '0' && SRC[sp] <= '7') v = v * 8 + (SRC[sp++] - '0');
                }
            } else {
                while (isdigit((unsigned char)SRC[sp])) v = v * 10 + (SRC[sp++] - '0');
            }
            if (c == '.' || c == 'E' || c == 'e') die("floating point not supported");
            if (isalnum((unsigned char)SRC[sp])) die("invalid integer literal");
            toks[ntok].ival = v; add_tok(T_NUM); continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            int i = sp; sp += skip_word(&SRC[sp]);
            atom_t name = new_atom_len(SRC + i, sp - i);
            if (name >= K_INT) {
                if (name <= K_ENUM) { add_tok(name); continue; }
                if (name <= K_EXTERN) continue;   // drop qualifiers
            }
            toks[ntok].text = name; add_tok(T_ID); continue;
        }
        if (c == '"') {                              // string literal (+ adjacent concat)
            char buf[8192]; size_t len = 0;
            for (;;) {
                sp++;                                 // skip opening quote
                while (SRC[sp] && SRC[sp] != '"' && SRC[sp] != '\n') {
                    int ch = SRC[sp++];
                    if (ch == '\\') ch = read_escape();
                    if (len >= sizeof(buf)) die("string too long");
                    buf[len++] = (char)ch;
                }
                if (SRC[sp] != '"') die("unterminated string");
                sp++;               // skip closing quote
                skip_space();
                if (SRC[sp] != '"') break;   // concatenate
            }
            toks[ntok].text = new_atom_len(buf, len); add_tok(T_STR); continue;
        }
        if (c == '\'') {
            sp++;
            int ch = SRC[sp];
            if (ch == '\\' && SRC[sp+1]) { sp++; ch = read_escape(); }
            else if (ch) sp++;
            if (SRC[sp] == '\'') sp++;
            else die("malformed character constant");
            toks[ntok].ival = ch; add_tok(T_CHAR); continue;
        }
        if (c == '.') {
            if (isdigit((unsigned char)SRC[sp+1])) { die("floating point not supported"); }
            if (SRC[sp+1] == '.' && SRC[sp+2] == '.') {
                sp += 3; add_tok(T_ELLIPSIS); continue;
            }
            sp += 1; add_tok(T_DOT); continue;
        }
        if (c == '-' && SRC[sp+1] == '>') { sp += 2; add_tok(T_ARROW); continue; }
        const char *p = ops;
        while (p < ops + sizeof(ops) && *p && *p != c) p += 5;
        if (*p) {
            if (p[2] && SRC[sp+1] == c) {
                if (p[4] && SRC[sp+2] == '=') { sp += 3; add_tok(p[4]); continue; }
                else { sp += 2; add_tok(p[2]); continue; }
            }
            if (p[3] && SRC[sp+1] == '=') { sp += 2; add_tok(p[3]); continue; }
            else { sp += 1; add_tok(p[1]); continue; }
        }
        warning(lineno, "unknown character '%c' in source", c);
        sp += 1;
        continue;
    }
    if (verbose) printf("Tokenized: %d tokens\n", ntok);
}

// =====================================================================
// 3. TYPES
// =====================================================================
enum { TY_INT, TY_CHAR, TY_LONG, TY_VOID, TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION, TY_ENUM };
typedef struct Type Type;
typedef struct Member { atom_t name; Type *type; int offset, align, pad; struct Node *init; struct Member *next; } Member;
struct Type {
    char kind, align; int arr_len; Type *ptr; struct Node *arr_len_expr;
    Member *members; int struct_size; atom_t tag;  // TY_STRUCT / TY_UNION
};

static Type ty_int_s  = { TY_INT,  8, 0, 0, 0, 0, 0, 0 };
static Type ty_char_s = { TY_CHAR, 1, 0, 0, 0, 0, 0, 0 };
static Type ty_long_s = { TY_LONG, 8, 0, 0, 0, 0, 0, 0 };
static Type ty_void_s = { TY_VOID, 1, 0, 0, 0, 0, 0, 0 };

static Type *ty_int(void)  { return &ty_int_s; }
static Type *ty_char(void) { return &ty_char_s; }
static Type *ty_long(void) { return &ty_long_s; }
static Type *ty_void(void) { return &ty_void_s; }

static Type *ptr_to(Type *base) {
    Type *t = allocz(1, sizeof(Type)); t->kind = TY_PTR; t->ptr = base; return t;
}
static int ty_size(Type *t) {
    switch (t->kind) {
        case TY_ARRAY:  return t->arr_len * ty_size(t->ptr);
        case TY_CHAR:   return 1;
        case TY_VOID:   return 0;
        case TY_STRUCT: case TY_UNION: return t->struct_size;
        default:        return 8;
    }
}
static int ty_align(Type *t) { return t->align; }
static Member *find_member(Type *st, atom_t name) {
    if (st->kind == TY_PTR) st = st->ptr;
    for (Member *m = st->members; m; m = m->next) if (m->name == name) return m;
    return NULL;
}
// element size for pointer/array arithmetic (bytes per step); 0 if not a pointer
static int elem_size(Type *t) {
    if (t->kind == TY_ARRAY) return ty_size(t->ptr);
    if (t->kind == TY_PTR) { int s = ty_size(t->ptr); return s ? s : 1; }
    return 0;
}
static int is_ptrish(Type *t) { return t->kind == TY_ARRAY || t->kind == TY_PTR; }
static bool same_type(Type *t1, Type *t2) {
    if (t1 && t2) {
        if (t1->kind != t2->kind) return false;
        switch (t1->kind) {
        case TY_ARRAY: return t1->arr_len == t2->arr_len && same_type(t1->ptr, t2->ptr);
        case TY_PTR:   return same_type(t1->ptr, t2->ptr);
        case TY_STRUCT: case TY_UNION: case TY_ENUM: return t1->tag == t2->tag;
        }
    }
    return t1 == t2;
}

// =====================================================================
// 4. AST
// =====================================================================
enum {
    N_NUM, N_STR, N_VAR, N_CALL, N_ASSIGN, N_BIN, N_UNARY,
    N_POST, N_CAST, N_DEREF, N_ADDR, N_LOGAND, N_LOGOR,
    N_IF, N_WHILE, N_RETURN, N_BLOCK, N_EXPR, N_DECL, N_ASM, N_EMPTY,
    N_FOR, N_DOWHILE, N_BREAK, N_CONTINUE, N_TERNARY, N_PRE,
    N_MEMBER, N_SIZEOF,
    N_SWITCH, N_CASE, N_DEFAULT, N_GOTO, N_LABEL,
};

#define HAS_BREAK     1     // bits in n->ival
#define HAS_CONTINUE  2
typedef struct Node Node;
struct Node {
    int   kind;
    char  op;               // token kind for N_BIN / N_POST (T_INC/T_DEC)
    bool  unused;           // N_ASSIGN: is assigned value used
    long  ival;             // N_NUM, N_CASE, N_DEFAULT, N_LABEL, N_FOR, N_DO, N_WHILE, N_SWITCH
    atom_t str;             // N_STR, N_ASM decoded text
    atom_t name;            // N_VAR / N_CALL / N_DECL
    int   lineno;
    Type *type;             // result / declared type
    Node *lhs, *rhs, *cond, *init, *cases;
    Node *args[8]; int nargs;
    Node *body[512]; int nbody;   // N_BLOCK statements
};

static Token *cur(void);
static Node *new_node(int k) {
    Node *n = allocz(1, sizeof(Node));
    n->kind = k; n->lineno = cur()->lineno; n->op = cur()->kind;
    return n;
}
static Node *new_node1(int k, Node *lhs) { Node *n = new_node(k); n->lhs = lhs; return n; }
static Node *new_bin_node(Node *lhs) { return new_node1(N_BIN, lhs); }
static Node *new_num_node(long ival, Type *t) { Node *n = new_node(N_NUM); n->ival = ival; n->type = t; return n; }
static void error(Node *n, const char *fmt, ...) attr_printf(2,3);
static void error(Node *n, const char *fmt, ...) {
    int pos = n ? n->lineno : cur()->lineno;
    va_list a; va_start(a, fmt); err_message(pos, "error", fmt, a); va_end(a);
    exit(1);
}
static Type *array_of(Type *base, Node *len_expr) {
    Type *arr = allocz(1, sizeof(Type)); arr->kind = TY_ARRAY; arr->ptr = base;
    arr->arr_len = -1;
    arr->arr_len_expr = len_expr;
    if (len_expr && len_expr->kind == N_NUM) arr->arr_len = len_expr->ival;
    return arr;
}

// ---- symbols ----
typedef struct Sym { atom_t name; int pos; Type *type; bool is_global, is_constant; int offset;
                     struct Node *init; long ival; struct Sym *next; } Sym;
static Sym *globals, *globals_tail;

// per-function local table (params + locals), built during offset pass
static Sym *locals;
static int  frame_size;

static Sym *sym_find(Sym *list, atom_t name) {
    for (; list; list = list->next) if (list->name == name) return list;
    return NULL;
}
static Sym *add_global(atom_t name, int pos, Type *t) {
    Sym *s = allocz(1, sizeof(Sym)); s->name = name; s->pos = pos;
    s->type = t; s->is_global = 1;
    if (globals) globals_tail->next = s; else globals = s;
    return globals_tail = s;
}

static int ntypedefs;
static Node *typedefs[256];
static void add_typedef(Node *n) {
    if (ntypedefs >= 256) die("too many typedefs");
    typedefs[ntypedefs++] = n;
}
static Type *find_typedef(atom_t name) {
    for (int i = 0; i < ntypedefs; i++) {
        if (typedefs[i]->name == name) return typedefs[i]->type;
    }
    return NULL;
}

// =====================================================================
// 5. PARSER
// =====================================================================
static int  P;                       // token cursor
static Token *cur(void) { return &toks[P]; }
static int  at(int k)   { return toks[P].kind == k; }
static int  eat(int k)  { if (toks[P].kind == k) { P++; return 1; } return 0; }
static void expect(int k) { if (!eat(k)) { error(NULL, "expected '%s', got '%s'", token_name[k], token_str(cur())); } }

static atom_t getid(void) {
    if (toks[P].kind == T_ID) return toks[P++].text;
    expect(T_ID); return 0;
}

static bool is_type_start(Token *t) {
    int k = t->kind;
    if (k == K_INT || k == K_LONG || k == K_CHAR || k == K_VOID ||
        k == K_ENUM || k == K_STRUCT || k == K_UNION) return true;
    if (k == T_ID && find_typedef(t->text)) return true;
    return false;
}

// ---- struct/union tag table ----
// XXX: these should be global/local based
typedef struct Tag { atom_t name; Type *type; int kind; struct Tag *next; } Tag;
static Tag *tags;
static Type *tag_get(atom_t name, int kind) { // find or forward-declare
    if (name) {
        for (Tag *e = tags; e; e = e->next) {
            if (e->name == name && e->kind == kind) return e->type;
        }
    }
    Type *t = allocz(1, sizeof(Type)); t->kind = kind; t->tag = name;
    if (name) {
        Tag *e = allocz(1, sizeof(Tag));
        e->name = name; e->kind = kind; e->type = t;
        e->next = tags; tags = e;
    }
    return t;
}

static Node *parse_expr(void);
static Node *parse_assign(void);
static Node *parse_stmt(void);
static Type *parse_type_base_only(void);

static int  label_num;
static Node *this_switch;
static Node *this_loop;
static struct Label *add_label(atom_t name, int num);
static struct Label *find_label(atom_t name);
static Node *parse_const_expr(void);
static Node *parse_init(void);
static bool eval_expr(Node *n, long *vp);

// struct/union specifier:  (struct|union) [tag] [ { members } ]
static Type *parse_struct(int kind) {
    atom_t tag = at(T_ID) ? getid() : 0;
    Type *st = tag_get(tag, kind);
    if (eat(T_LBRACE)) {                           // definition
        if (st->members) error(NULL, "%s already has a definition", atom_str(kind));
        Member *head = NULL, *tail = NULL;
        int off = 0, maxsz = 0;
        st->align = 1;
        while (!at(T_RBRACE) && !at(T_EOF)) {
            Type *mbase = parse_type_base_only();
            for (;;) {
                Type *mt = mbase;
                while (eat(T_STAR)) mt = ptr_to(mt);
                atom_t mnm = getid();
                while (eat(T_LBRK)) {                 // array member
                    Node *len_expr = at(T_RBRK) ? NULL : parse_const_expr();
                    mt = array_of(mt, len_expr);
                    expect(T_RBRK);
                }
                int msz = ty_size(mt);
                int align = ty_align(mt);
                if (align > st->align) st->align = align;
                Member *m = allocz(1, sizeof(Member));
                m->name = mnm; m->type = mt;
                if (tail) { // set padding bytes in previous element
                    int off0 = off;
                    off = (off + align - 1) & ~(align - 1);
                    tail->pad = off - off0;
                    tail->next = m;
                } else {
                    head = m;
                }
                tail = m;
                if (kind == TY_UNION) {
                    if (msz > maxsz) maxsz = msz;
                } else {
                    m->offset = off; off += msz;
                }
                if (!eat(T_COMMA)) break;
            }
            expect(T_SEMI);
        }
        expect(T_RBRACE);
        st->members = head;
        if (kind == TY_UNION) {
            st->struct_size = (maxsz + st->align - 1) & ~(st->align - 1);
            for (Member *m = head; m; m = m->next) m->pad = st->struct_size - ty_size(m->type);
        } else {
            st->struct_size = (off + st->align - 1) & ~(st->align - 1);
            if (tail) tail->pad = st->struct_size - off;
        }
    }
    return st;
}

// enum specifier:  enum [tag] [ { members [= value], } ]
static Type *parse_enum(void) {
    atom_t tag = at(T_ID) ? getid() : 0;
    Type *st = tag_get(tag, TY_ENUM);
    if (eat(T_LBRACE)) {                           // definition
        Member **tailp = &st->members;
        long off = 0;
        while (!at(T_RBRACE) && !at(T_EOF)) {
            int pos = toks[P].lineno;
            atom_t name = getid();
            Member *m = allocz(1, sizeof(Member));
            m->name = name;
            if (eat(T_ASSIGN)) {
                m->init = parse_const_expr();
                if (!eval_expr(m->init, &off))
                    error(m->init, "expression is not constant");
            }
            m->offset = off++;
            *tailp = m;
            tailp = &m->next;
            Sym *s = add_global(name, pos, st);
            s->is_global = 1;
            s->is_constant = 1;
            s->ival = off;
            s->init = m->init;
            if (!eat(T_COMMA)) break;
        }
        expect(T_RBRACE);
    }
    return st;
}

// parse base type + pointer stars; returns Type*
static Type *parse_type(void) {
    Type *base = parse_type_base_only();
    while (eat(T_STAR)) base = ptr_to(base);
    return base;
}

static Node *parse_primary(void) {
    if (eat(T_LP)) {
        // cast?  ( type ) unary
        if (is_type_start(cur())) {
            Type *t = parse_type();
            expect(T_RP);
            Node *n = new_node(N_CAST); n->type = t; n->lhs = parse_assign();
            return n;
        }
        Node *n = parse_expr(); expect(T_RP); return n;
    }
    if (at(T_NUM))  { Node *n = new_num_node(cur()->ival, ty_long()); P++; return n; }
    if (at(T_CHAR)) { Node *n = new_num_node(cur()->ival, ty_int());  P++; return n; }
    if (at(T_STR))  { Node *n = new_node(N_STR); n->str = cur()->text;
                      n->type = ptr_to(ty_char()); P++; return n; }
    if (at(T_ID)) {
        atom_t nm = getid();
        if (at(T_LP)) {                       // function call
            Node *n = new_node(N_CALL); P++; n->name = nm;
            while (!at(T_RP)) {
                n->args[n->nargs++] = parse_assign();
                if (!eat(T_COMMA)) break;
            }
            expect(T_RP); n->type = ty_long(); return n;    // XXX: type should be func return type
        }
        P--; Node *n = new_node(N_VAR); P++; n->name = nm; return n;
    }
    error(NULL, "expected expression, got '%s'", token_str(cur())); return 0;
}

// postfix: primary ( [expr] | ++ | -- )*
static Node *parse_postfix(void) {
    Node *n = parse_primary();
    for (;;) {
        switch (toks[P].kind) {
        case T_LBRK:              // a[i]  ->  *(a + i)
            n = new_bin_node(n); n->op = T_PLUS;
            n = new_node1(N_DEREF, n); P++;
            n->lhs->rhs = parse_expr();
            expect(T_RBRK);
            break;
        case T_DOT:               // a.field
            n = new_node1(N_MEMBER, n); P++;
            n->name = getid();
            break;
        case T_ARROW:             // p->field  ==  (*p).field
            n = new_node1(N_DEREF, n);
            n = new_node1(N_MEMBER, n); P++;
            n->name = getid();
            break;
        case T_INC: case T_DEC:
            n = new_node1(N_POST, n); P++;
            break;
        default:
            return n;
        }
    }
}

static Node *parse_unary(void) {
    switch (toks[P].kind) {
    case K_SIZEOF: {
        Node *n = new_node(N_SIZEOF); P++;
        if (at(T_LP) && is_type_start(&toks[P+1])) { P++; n->type = parse_type(); expect(T_RP); }
        else n->lhs = parse_unary();
        return n;
    }
    case T_MINUS:
    case T_NOT:
    case T_BITNOT: { Node *n = new_node(N_UNARY); P++; n->lhs = parse_unary(); return n; }
    case T_STAR:   { Node *n = new_node(N_DEREF); P++; n->lhs = parse_unary(); return n; }
    case T_AMP:    { Node *n = new_node(N_ADDR);  P++; n->lhs = parse_unary(); return n; }
    case T_INC:
    case T_DEC:    { Node *n = new_node(N_PRE); P++; n->lhs = parse_unary(); return n; }
    }
    return parse_postfix();
}

static Node *parse_mul(void) {
    Node *n = parse_unary();
    while (at(T_STAR) || at(T_SLASH) || at(T_PERCENT)) { n = new_bin_node(n); P++; n->rhs = parse_unary(); }
    return n;
}
static Node *parse_add(void) {
    Node *n = parse_mul();
    while (at(T_PLUS) || at(T_MINUS)) { n = new_bin_node(n); P++; n->rhs = parse_mul(); }
    return n;
}
// C precedence:  <<  >>  bind tighter than the relational operators.
static Node *parse_shift(void) {
    Node *n = parse_add();
    while (at(T_SHL) || at(T_SHR)) { n = new_bin_node(n); P++; n->rhs = parse_add(); }
    return n;
}
static Node *parse_rel(void) {
    Node *n = parse_shift();
    while (at(T_LT) || at(T_GT) || at(T_LE) || at(T_GE)) { n = new_bin_node(n); P++; n->rhs = parse_shift(); }
    return n;
}
static Node *parse_eq(void) {
    Node *n = parse_rel();
    while (at(T_EQ) || at(T_NE)) { n = new_bin_node(n); P++; n->rhs = parse_rel(); }
    return n;
}
// Bitwise AND / XOR / OR sit between equality and logical-AND, in that order.
static Node *parse_band(void) {
    Node *n = parse_eq();
    while (at(T_AMP)) { n = new_bin_node(n); P++; n->rhs = parse_eq(); }
    return n;
}
static Node *parse_bxor(void) {
    Node *n = parse_band();
    while (at(T_BITXOR)) { n = new_bin_node(n); P++; n->rhs = parse_band(); }
    return n;
}
static Node *parse_bor(void) {
    Node *n = parse_bxor();
    while (at(T_BITOR)) { n = new_bin_node(n); P++; n->rhs = parse_bxor(); }
    return n;
}
static Node *parse_land(void) {
    Node *n = parse_bor();
    while (at(T_ANDAND)) { n = new_node1(N_LOGAND, n); P++; n->rhs = parse_bor(); }
    return n;
}
static Node *parse_lor(void) {
    Node *n = parse_land();
    while (at(T_OROR)) { n = new_node1(N_LOGOR, n); P++; n->rhs = parse_land(); }
    return n;
}
static Node *parse_ternary(void) {
    Node *n = parse_lor();
    if (at(T_QUESTION)) {
        Node *t = new_node(N_TERNARY); P++;
        t->cond = n; t->lhs = parse_expr(); expect(T_COLON); t->rhs = parse_ternary();
        return t;
    }
    return n;
}
static Node *parse_const_expr(void) { return parse_ternary(); }
static Node *parse_assign(void) {
    Node *n = parse_ternary();
    switch (toks[P].kind) {
    case T_ASSIGN:  case T_PLUSEQ:  case T_MINUSEQ:  case T_STAREQ:
    case T_SLASHEQ:  case T_PERCENTEQ:  case T_OREQ:  case T_ANDEQ:
    case T_XOREQ:  case T_SHLEQ:  case T_SHREQ:
        n = new_node1(N_ASSIGN, n); P++; n->rhs = parse_assign();
        break;
    }
    return n;
}
static Node *parse_expr(void) {
    Node *n = parse_assign();
    while (at(T_COMMA)) { n = new_bin_node(n); P++; n->rhs = parse_assign(); }
    return n;
}

// a declaration inside a block: type declarator [= init] (, declarator [= init])* ;
static Node *parse_decl_stmt(void) {
    Type *base = parse_type_base_only();   // fwd-declared below
    Node *blk = new_node(N_BLOCK);
    if (eat(T_SEMI)) return blk;           // bare  struct Foo { ... };  (type only)
    for (;;) {
        Type *t = base;
        while (eat(T_STAR)) t = ptr_to(t);
        atom_t nm = getid();
        while (eat(T_LBRK)) {                          // array: char buf[24];
            Node *len_expr = at(T_RBRK) ? NULL : parse_const_expr();
            t = array_of(t, len_expr);
            expect(T_RBRK);
        }
        Node *d = new_node(N_DECL); d->name = nm; d->type = t;
        if (eat(T_ASSIGN)) {
            d->init = parse_init();
            if (t->kind == TY_ARRAY && t->arr_len < 0 && d->init->kind == N_BLOCK) {
                t->arr_len = d->init->nbody;
            }
        }
        blk->body[blk->nbody++] = d;
        if (!eat(T_COMMA)) break;
    }
    expect(T_SEMI);
    return blk;
}
// parse just the base type (no trailing stars) — stars belong to each declarator
static Type *parse_type_base_only(void) {
    if (eat(K_STRUCT)) return parse_struct(TY_STRUCT);
    if (eat(K_UNION))  return parse_struct(TY_UNION);
    if (eat(K_ENUM))   return parse_enum();
    if (eat(K_INT))    return ty_int();
    if (eat(K_LONG))   return ty_long();
    if (eat(K_CHAR))   return ty_char();
    if (eat(K_VOID))   return ty_void();
    if (at(T_ID)) {
        Type *type = find_typedef(toks[P].text);
        if (type) { P++; return type; }
    }
    error(NULL, "expected type, got '%s'", token_str(cur())); return 0;
}
static Node *parse_typedef(void) {
    P++;    // skip 'typedef'
    Node *n = parse_decl_stmt();
    n->op = K_TYPEDEF;
    return n;
}

static Node *parse_init(void) {
    if (at(T_LBRACE)) {
        Node *n = new_node(N_BLOCK); P++;
        while (!at(T_RBRACE) && !at(T_EOF)) {
            n->body[n->nbody++] = parse_init();
            if (!eat(T_COMMA)) break;
        }
        expect(T_RBRACE);
        return n;
    } else {
        return parse_assign();
    }
}

static Node *parse_loop_body(Node *n) {
    Node *save_this_loop = this_loop;
    Node *save_this_switch = this_switch;
    this_loop = n;
    this_switch = NULL;
    Node *lhs = parse_stmt();
    this_loop = save_this_loop;
    this_switch = save_this_switch;
    return lhs;
}

static Node *parse_block(void) {
    if (!at(T_LBRACE)) expect(T_LBRACE);
    Node *n = new_node(N_BLOCK); P++;
    while (!at(T_RBRACE) && !at(T_EOF)) n->body[n->nbody++] = parse_stmt();
    expect(T_RBRACE);
    return n;
}

static Node *parse_stmt(void) {
    if (at(T_LBRACE)) return parse_block();
    if (at(T_SEMI))  { Node *n = new_node(N_EMPTY); P++; return n; }
    if (at(K_IF)) {
        Node *n = new_node(N_IF); P++;
        expect(T_LP); n->cond = parse_expr(); expect(T_RP);
        n->lhs = parse_stmt();
        if (eat(K_ELSE)) n->rhs = parse_stmt();
        return n;
    }
    if (at(K_WHILE)) {
        Node *n = new_node(N_WHILE); P++;
        expect(T_LP); n->cond = parse_expr(); expect(T_RP);
        n->lhs = parse_loop_body(n);
        return n;
    }
    if (at(K_SWITCH)) {
        Node *n = new_node(N_SWITCH); P++;
        expect(T_LP); n->cond = parse_expr(); expect(T_RP);
        Node *save_this_switch = this_switch;
        this_switch = n;
        n->lhs = parse_block();
        this_switch = save_this_switch;
        return n;
    }
    if (at(K_CASE)) {
        if (!this_switch) error(NULL, "'case' outside a 'switch' statement");
        Node *n = new_node(N_CASE); P++;
        n->ival = ++label_num;
        n->cond = parse_const_expr();
        if (this_switch) {  // append node to case list (quadratic but small n)
            Node **casep = &this_switch->cases;
            while (*casep) { casep = &(*casep)->lhs; } *casep = n;
        }
        expect(T_COLON);
        return n;
    }
    if (at(K_DEFAULT))  {
        if (!this_switch) error(NULL, "'default' outside a 'switch' statement");
        else if (this_switch->rhs) error(NULL, "duplicate 'default' in 'switch' statement");
        Node *n = new_node(N_DEFAULT); P++;
        n->ival = ++label_num;
        if (this_switch) this_switch->rhs = n;
        expect(T_COLON);
        return n;
    }
    if (at(K_GOTO)) {
        Node *n = new_node(N_GOTO); P++;
        n->name = getid();
        expect(T_SEMI);
        return n;
    }
    if (at(K_FOR)) {
        Node *n = new_node(N_FOR); P++;
        expect(T_LP);
        if (is_type_start(cur())) n->init = parse_decl_stmt();      // consumes ';'
        else {
            if (!at(T_SEMI)) { n->init = new_node(N_EXPR); n->init->lhs = parse_expr(); }
            expect(T_SEMI);
        }
        if (!at(T_SEMI)) n->cond = parse_expr();
        expect(T_SEMI);
        // XXX should accept comma expression
        if (!at(T_RP)) n->rhs = parse_expr(); // step
        expect(T_RP);
        n->lhs = parse_loop_body(n);
        return n;
    }
    if (at(K_DO)) {
        Node *n = new_node(N_DOWHILE); P++;
        n->lhs = parse_loop_body(n);
        expect(K_WHILE); expect(T_LP); n->cond = parse_expr(); expect(T_RP); expect(T_SEMI);
        return n;
    }
    if (at(K_BREAK)) {
        if (this_loop) { this_loop->ival |= HAS_BREAK; }
        else if (this_switch) { this_switch->ival |= HAS_BREAK; }
        else { error(NULL, "'break' outside a loop or 'switch' statement"); }
        Node *n = new_node(N_BREAK); P++; expect(T_SEMI); return n;
    }
    if (at(K_CONTINUE)) {
        if (this_loop) { this_loop->ival |= HAS_CONTINUE; }
        else error(NULL, "'continue' outside a loop statement");
        Node *n = new_node(N_CONTINUE); P++; expect(T_SEMI); return n;
    }
    if (at(K_RETURN)) {
        Node *n = new_node(N_RETURN); P++;
        if (!at(T_SEMI)) n->lhs = parse_expr();
        expect(T_SEMI);
        return n;
    }
    if (at(K_TYPEDEF)) {
        // XXX: should be handled like a local decl
        Node *n = parse_typedef();
        return n;
    }
    if (at(K_ASM) || at(K__ASM__)) {
        Node *n = new_node(N_ASM); P++;
        expect(T_LP);
        if (!at(T_STR)) expect(T_STR);
        n->str = cur()->text; P++;
        expect(T_RP); expect(T_SEMI);
        return n;
    }
    if (at(T_ID) && toks[P+1].kind == T_COLON) {
        Node *n = new_node(N_LABEL);
        atom_t name = getid();
        if (find_label(name)) warning(n->lineno, "duplicate label '%s'", atom_str(name));
        n->name = name;
        n->ival = ++label_num;
        add_label(name, (int)n->ival);
        expect(T_COLON);
        return n;
    }
    if (is_type_start(cur())) return parse_decl_stmt();
    Node *n = new_node(N_EXPR); n->lhs = parse_expr(); expect(T_SEMI);
    return n;
}

// 5.1 Constant expression evaluator

static Sym *lookup(atom_t name);
static Type *static_typeof(Node *n);

static bool eval_expr(Node *n, long *vp) {
    long v1, v2;
    switch (n->kind) {
    case N_EXPR: return eval_expr(n->lhs, vp);
    case N_NUM:  *vp = n->ival; return true;
    case N_VAR: {
        Sym *s = lookup(n->name);
        if (!s) error(n, "undeclared identifier '%s'", atom_str(n->name));
        if (!s->is_constant) {
            s->is_constant = eval_expr(s->init, &s->ival);
            if (!s->is_constant) error(n, "symbol is not constant: '%s'", atom_str(n->name));
        }
        *vp = s->ival; return true;
    }
    case N_SIZEOF: {
        Type *t = n->type ? n->type : static_typeof(n->lhs);
        *vp = ty_size(t);
        return true;
    }
    case N_CAST:
        if (!eval_expr(n->lhs, vp)) return false;
        // XXX: should apply cast
        return true;
    case N_TERNARY:
        if (!eval_expr(n->cond, &v1)) return false;
        return eval_expr(v1 ? n->lhs : n->rhs, vp);
    case N_UNARY:
        if (!eval_expr(n->lhs, &v1)) return false;
        switch (n->op) {
        case T_MINUS: *vp = -v1; return true;
        case T_PLUS: *vp = v1; return true;
        case T_BITNOT: *vp = ~v1; return true;
        default: break;
        }
        break;
    case N_LOGAND:
        if (!eval_expr(n->lhs, &v1)) return false;
        if (v1 && !eval_expr(n->rhs, &v1)) return false;
        *vp = (v1 != 0);
        return true;
    case N_LOGOR:
        if (!eval_expr(n->lhs, &v1)) return false;
        if (!v1 && !eval_expr(n->rhs, &v1)) return false;
        *vp = (v1 != 0);
        return true;
    case N_BIN:
        if (!eval_expr(n->lhs, &v1)) return false;
        if (!eval_expr(n->rhs, &v2)) return false;
        switch (n->op) {
        case T_PLUS:    *vp = v1 + v2;  return true;
        case T_MINUS:   *vp = v1 - v2;  return true;
        case T_STAR:    *vp = v1 * v2;  return true;
        case T_SLASH:   *vp = v1 / v2;  return true;   // XXX: check overflow
        case T_PERCENT: *vp = v1 % v2;  return true;   // XXX: check overflow
        case T_AMP:     *vp = v1 & v2;  return true;
        case T_BITOR:   *vp = v1 | v2;  return true;
        case T_BITXOR:  *vp = v1 ^ v2;  return true;
        case T_SHL:     *vp = v1 << v2; return true;
        case T_SHR:     *vp = v1 >> v2; return true;
        case T_LT:      *vp = v1 < v2;  return true;
        case T_GT:      *vp = v1 > v2;  return true;
        case T_LE:      *vp = v1 <= v2; return true;
        case T_GE:      *vp = v1 >= v2; return true;
        case T_EQ:      *vp = v1 == v2; return true;
        case T_NE:      *vp = v1 != v2; return true;
        case T_COMMA:   *vp = v2; return true;
        default:
            error(n, "bad binary operator '%s'", token_name[n->op]);
            break;
        }
        break;
    }
    return false;
}

// ---- top level ----
typedef struct Func { atom_t name; Node *params[8]; int nparams; Type *ptype[8];
                      bool is_variadic, used; Node *body; Type *rtype; struct Label *labels; struct Func *next; } Func;
static Func *funcs, **funcs_tail;
static Func *this_fn;

typedef struct Label Label;
struct Label { atom_t name; int num; bool used, found; struct Label *next; };
static Label *add_label(atom_t name, int num) {
    Label *lab = allocz(1, sizeof(Label));
    lab->name = name;
    lab->num = num;
    lab->next = this_fn->labels;
    return this_fn->labels = lab;
}
static Label *find_label(atom_t name) {
    for (Label *lab = this_fn->labels; lab; lab = lab->next) {
        if (lab->name == name) return lab;
    }
    return NULL;
}

static Func *find_func(atom_t name) {
    for (Func *fn = funcs; fn; fn = fn->next) {
        if (fn->name == name) return fn;
    }
    return NULL;
}

static void parse_toplevel(void) {
    if (at(K_TYPEDEF)) {
        // XXX: should be handled like a decl
        Node *n = parse_typedef();
        for (int i = 0; i < n->nbody; i++) { add_typedef(n->body[i]); }
        return;
    }
    Type *base = parse_type_base_only();
    if (eat(T_SEMI)) return;                           // bare  struct Foo { ... };
    Type *t = base;
    while (eat(T_STAR)) t = ptr_to(t);
    atom_t nm = getid();

    // XXX: parse function pointers and such
    if (eat(T_LP)) {                                   // function definition
        bool has_prototype = false;
        Func *fn = find_func(nm);
        if (fn) {
            has_prototype = true;
            if (!same_type(fn->rtype, t))
                warning(cur()->lineno, "return type mismatch with '%s' function prototype", atom_str(nm));
        } else {
            fn = allocz(1, sizeof(Func));
            fn->name = nm;
            fn->rtype = t;
            if (!funcs_tail) funcs_tail = &funcs; *funcs_tail = fn; funcs_tail = &fn->next;
        }
        int nparams = 0;
        bool is_variadic = false;
        while (!at(T_RP)) {
            if (eat(T_ELLIPSIS)) { is_variadic = true; break; }   // printf(char *fmt, ...)
            Type *pt = parse_type_base_only();
            while (eat(T_STAR)) pt = ptr_to(pt);
            if (pt->kind == TY_VOID && !at(T_ID)) break;   // (void)
            Node *pv = new_node(N_DECL);
            if (at(T_ID)) pv->name = getid();   // argument name is optional
            while (eat(T_LBRK)) {                 // array member
                // XXX: handle fake array specification -> pointer
                Node *len_expr = at(T_RBRK) ? NULL : parse_const_expr();
                pt = array_of(pt, len_expr);
                expect(T_RBRK);
            }
            pv->type = pt;
            if (has_prototype && fn->ptype[nparams] && !same_type(fn->ptype[nparams], pt))
                warning(cur()->lineno, "type mismatch with prototype on argument %d", nparams + 1);
            fn->params[nparams] = pv; fn->ptype[nparams] = pt; nparams++;
            if (!eat(T_COMMA)) break;
        }
        expect(T_RP);
        if (has_prototype && (fn->nparams != nparams || fn->is_variadic != is_variadic))
            warning(cur()->lineno, "argument count mismatch with prototype");
        fn->is_variadic = is_variadic;
        fn->nparams = nparams;
        if (!eat(T_SEMI)) {
            if (fn->body) { error(NULL, "function '%s' already has a body", atom_str(fn->name)); }
            this_fn = fn;
            fn->body = parse_block(); // prototype / extern decl — no body to emit
            this_fn = NULL;
        }
        if (verbose) printf("-> %s\n", atom_str(nm));
        return;
    }
    // global variable(s):  type name [= ...] (, ...) ;   (initialisers ignored -> .bss)
    for (;;) {
        int pos = toks[P].lineno;
        while (eat(T_LBRK)) {
            Node *len_expr = at(T_RBRK) ? NULL : parse_const_expr();
            t = array_of(t, len_expr);
            expect(T_RBRK);
        }
        Sym *sym = add_global(nm, pos, t);
        if (eat(T_ASSIGN)) {
            sym->init = parse_init();
            if (t->kind == TY_ARRAY && t->arr_len < 0 && sym->init->kind == N_BLOCK) {
                t->arr_len = sym->init->nbody;
            }
        }
        if (verbose) printf("-> %s\n", atom_str(nm));
        if (!eat(T_COMMA)) break;
        t = base; while (eat(T_STAR)) t = ptr_to(t);
        nm = getid();
    }
    expect(T_SEMI);
}

// =====================================================================
// 6. OFFSET ASSIGNMENT (per function)
// =====================================================================
// This seems broken: it does not allow the same name to be used in different blocks
static Sym *add_local(atom_t name, int pos, Type *t) {
    Sym *s = allocz(1, sizeof(Sym)); s->name = name; s->pos = pos;
    s->type = t; s->is_global = 0;
    int sz = ty_size(t); if (sz < 8) sz = 8; sz = (sz + 7) & ~7;
    frame_size += sz; s->offset = frame_size;
    s->next = locals; locals = s;
    return s;
}
static void collect_locals(Node *n) {
    if (!n) return;
    switch (n->kind) {
        case N_BLOCK: for (int i = 0; i < n->nbody; i++) collect_locals(n->body[i]); break;
        case N_DECL:
            if (!sym_find(locals, n->name)) add_local(n->name, n->lineno, n->type);
            if (n->init) collect_locals(n->init);
            break;
        case N_IF: case N_TERNARY:
            collect_locals(n->cond); collect_locals(n->lhs); collect_locals(n->rhs); break;
        case N_WHILE: case N_DOWHILE:
            collect_locals(n->lhs); collect_locals(n->cond); break;
        case N_FOR:
            collect_locals(n->init); collect_locals(n->cond); collect_locals(n->rhs);
            collect_locals(n->lhs); break;
        case N_RETURN: case N_EXPR: case N_UNARY: case N_DEREF: case N_ADDR:
        case N_CAST: case N_POST: case N_PRE: case N_MEMBER:
            collect_locals(n->lhs); break;
        case N_ASSIGN: case N_BIN: case N_LOGAND: case N_LOGOR:
            collect_locals(n->lhs); collect_locals(n->rhs); break;
        case N_CALL:  for (int i = 0; i < n->nargs; i++) collect_locals(n->args[i]); break;
        case N_SWITCH: collect_locals(n->cond); collect_locals(n->lhs); break;
        default: break;
    }
}

static bool check_optimize_call(Node *n) {
    if (!optimize) return false;
    // optimize some function calls
    if (n->name == ID_PRINTF) {
        if (n->nargs != 1 || n->args[0]->kind != N_STR) return false;
        const char *fmt = atom_str(n->args[0]->str);
        if (!*fmt) { n->kind = N_NUM; n->ival = 0; n->type = ty_long(); return true; }
        size_t len = strlen(fmt);
        if (strchr(fmt, '%') || fmt[len-1] != '\n') return false;
        n->name = ID_PUTS;
        n->args[0]->str = new_atom_len(fmt, len - 1);
        return true;
    }
    if (n->name == ID_STRLEN) {
        if (n->nargs != 1 || n->args[0]->kind != N_STR) return false;
        const char *s = atom_str(n->args[0]->str);
        size_t len = strlen(s); // do not use atom len to allow embedded nuls
        n->kind = N_NUM; n->ival = strlen(s); n->type = ty_long();
        return true;
    }
    if (n->name == ID_STRCPY) {
        if (n->nargs != 2 || n->args[1]->kind != N_STR) return false;
        const char *s = atom_str(n->args[1]->str);
        size_t len = strlen(s); // do not use atom len to allow embedded nuls
        n->name = ID_MEMCPY; n->ival = strlen(s); n->type = ty_long();
        n->args[2] = new_num_node(len + 1, ty_long());
        n->args[2]->lineno = n->args[1]->lineno;
        n->nargs = 3;
        return true;
    }
    return false;
}

static void check_used(Node *n);
static void check_used_func(atom_t name) {
    Func *fn = find_func(name);
    if (fn && !fn->used) {
        fn->used = true;
        check_used(fn->body);
    }
}

static void check_used(Node *n) {
    if (!n) return;
    switch (n->kind) {
        case N_BLOCK:
            // XXX: should handle scoping
            for (int i = 0; i < n->nbody; i++) check_used(n->body[i]); break;
        case N_DECL: check_used(n->init); break; // XXX: should handle scoping
        case N_IF: case N_TERNARY:
            check_used(n->cond); check_used(n->lhs); check_used(n->rhs); break;
        case N_WHILE: case N_DOWHILE:
            check_used(n->lhs); check_used(n->cond); break;
        case N_FOR:
            // XXX: should handle scoping
            check_used(n->init); check_used(n->cond); check_used(n->rhs);
            check_used(n->lhs); break;
        case N_RETURN: case N_EXPR: case N_UNARY: case N_DEREF: case N_ADDR:
        case N_CAST: case N_POST: case N_PRE: case N_MEMBER:
            check_used(n->lhs); break;
        case N_ASSIGN: case N_BIN: case N_LOGAND: case N_LOGOR:
            check_used(n->lhs); check_used(n->rhs); break;
        case N_CALL:
            if (check_optimize_call(n)) { check_used(n); break; }
            check_used_func(n->name);
            for (int i = 0; i < n->nargs; i++) check_used(n->args[i]);
            break;
        case N_SWITCH: check_used(n->cond); check_used(n->lhs); break;
        case N_VAR: break; // XXX: should look up symbol and set used bit
    }
}

// =====================================================================
// 7. CODE GENERATION
// =====================================================================
static int label_id;
static const char *ARGREG[6] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };

// current function's varargs state (set in gen_func, read by __builtin_va_* codegen)
static int cur_va_off, cur_named;

static Type *gen_expr(Node *n);       // fwd
static void  gen_stmt(Node *n);

static Sym *lookup(atom_t name) {
    Sym *s = sym_find(locals, name);
    if (!s) s = sym_find(globals, name);
    return s;
}
static void promote_rax(int size) {
    if (size == 1) emit("    movsx rax, al");
}
static void load_rax(int size) {                 // rax = *rax (size-aware, signed char)
    if (size == 1) emit("    movsx rax, byte ptr [rax]");
    else           emit("    mov rax, [rax]");
}
static void store_rcx_rax(int size) {            // *rcx = rax
    if (size == 1) emit("    mov [rcx], al");
    else           emit("    mov [rcx], rax");
}

// leave the ADDRESS of an lvalue node in rax
static Type *gen_addr(Node *n) {
    if (n->kind == N_VAR) {
        Sym *s = lookup(n->name);
        if (!s) error(n, "undeclared identifier '%s'", atom_str(n->name));
        if (s->is_global) emit("    lea rax, [rip + %s]", atom_str(n->name));
        else              emit("    lea rax, [rbp - %d]  # %s", s->offset, atom_str(n->name));
        return s->type;
    }
    if (n->kind == N_DEREF) {                      // &*p  ==  p
        Type *t = gen_expr(n->lhs);
        return is_ptrish(t) ? t->ptr : ty_long();
    }
    if (n->kind == N_MEMBER) {                      // &(s.field)
        Type *st = gen_addr(n->lhs);               // rax = &struct
        Member *m = find_member(st, n->name);
        if (!m) error(n, "no such struct member: %s", atom_str(n->name));
        if (m->offset) emit("    add rax, %d  # %s", m->offset, atom_str(n->name));
        return m->type;
    }
    error(n, "not an lvalue"); return 0;
}

// best-effort static type inference (used only by sizeof(expr))
static Type *static_typeof(Node *n) {
    switch (n->kind) {
        case N_NUM:    return n->type ? n->type : ty_long();
        case N_STR:    return ptr_to(ty_char());
        case N_CAST:   return n->type;
        case N_VAR:  { Sym *s = lookup(n->name); return s ? s->type : ty_long(); }
        case N_MEMBER: { Type *st = static_typeof(n->lhs); Member *m = find_member(st, n->name);
                         return m ? m->type : ty_long(); }
        case N_DEREF: { Type *t = static_typeof(n->lhs); return is_ptrish(t) ? t->ptr : ty_long(); }
        case N_ADDR:   return ptr_to(static_typeof(n->lhs));
        default:       return ty_long();
    }
}

static int gen_quoted_string(char *buf, size_t size, const char *str, int slen, unsigned char sep) {
    size_t j = 0;
    for (int i = 0; i < slen; i++) {
        unsigned char ch = str[i];
        switch (ch) {
        case '\n': ch = 'n'; goto escape;
        case '\t': ch = 'n'; goto escape;
        case '\r': ch = 'r'; goto escape;
        case '"': case '\'': if (ch == sep) goto escape; else goto normal;
        case '\\':
        escape:
            if (j + 2 >= size) break;
            buf[j++] = '\\'; buf[j++] = ch;
            continue;
        default:
        normal:
            if (ch >= 32 && ch <= 126) {
                if (j + 1 >= size) break;
                buf[j++] = ch;
                continue;
            }
            if (j + 4 >= size) break;
            buf[j++] = '\\';
            int has_digit = isdigit((unsigned char)str[i+1]);
            if (ch >= 64 || has_digit) buf[j++] = '0' + (ch >> 6);
            if (ch >= 8  || has_digit) buf[j++] = '0' + ((ch >> 3) & 7);
            buf[j++] = '0' + (ch & 7);
            continue;
        }
        break;
    }
    if (j < size) buf[j] = 0;
    return j;
}

static void gen_string_def(atom_t id) {
    char buf[8192];
    gen_quoted_string(buf, sizeof(buf), atom_str(id), atom_len(id), '"');
    emit(".LC%d: .string \"%s\"", id, buf);
}

static void gen_string(Node *n) {
    char buf[36];
    atom_t id = n->str;
    int len = gen_quoted_string(buf, sizeof(buf), atom_str(id), atom_len(id), '"');
    if (len > 32) strcpy(buf + 32, "...");
    emit("    lea rax, [rip + .LC%d]  # \"%s\"", id, buf);
    atom_flags(id) |= ATOM_USED;
}

static Type *gen_bin(Node *n, int op, Type *lt, Type *rt) {
    // rax = left, rcx = right
    switch (op) {
    case T_PLUS:
    case T_MINUS:
        if (is_ptrish(lt) && !is_ptrish(rt)) {
            int s = elem_size(lt); if (s > 1) emit("    imul rcx, %d", s);
        } else if (!is_ptrish(lt) && is_ptrish(rt) && op == T_PLUS) {
            int s = elem_size(rt); if (s > 1) emit("    imul rax, %d", s);
        }
        emit(op == T_PLUS ? "    add rax, rcx" : "    sub rax, rcx");
        return is_ptrish(lt) ? lt : (is_ptrish(rt) ? rt : ty_long());
    case T_STAR:    emit("    imul rax, rcx"); break;
    case T_SLASH:   emit("    cqo"); emit("    idiv rcx"); break;
    case T_PERCENT: emit("    cqo"); emit("    idiv rcx"); emit("    mov rax, rdx"); break;
    case T_AMP:     emit("    and rax, rcx"); break;
    case T_BITOR:   emit("    or rax, rcx");  break;
    case T_BITXOR:  emit("    xor rax, rcx"); break;
    case T_SHL:     emit("    shl rax, cl");  break;   // count in cl (low byte of rcx)
    case T_SHR:     emit("    sar rax, cl");  break;   // arithmetic shift right
    case T_LT: case T_GT: case T_LE: case T_GE: case T_EQ: case T_NE: {
        emit("    cmp rax, rcx");
        const char *cc = op==T_LT?"setl":op==T_GT?"setg":op==T_LE?"setle":
        op==T_GE?"setge":op==T_EQ?"sete":"setne";
        emit("    %s al", cc); emit("    movzx rax, al");
        break;
    }
    default: error(n, "bad binary operator '%s'", token_name[op]);
    }
    return ty_long();
}

static Type *gen_expr(Node *n) {
    switch (n->kind) {
    case N_NUM:  emit("    mov rax, %ld", n->ival); return n->type ? n->type : ty_long();
    case N_STR:  gen_string(n); return n->type;
    case N_VAR: {
        Sym *s = lookup(n->name);
        if (!s) error(n, "undeclared identifier '%s'", atom_str(n->name));
        if (s->is_constant) { emit("    mov rax, %ld", s->ival); return ty_long(); }
        // arrays and structs are used by-address (decay); scalars are loaded
        if (s->type->kind == TY_ARRAY || s->type->kind == TY_STRUCT || s->type->kind == TY_UNION) { gen_addr(n); return s->type; }
        gen_addr(n); load_rax(ty_size(s->type));
        return s->type;
    }
    case N_MEMBER: {
        Type *mt = gen_addr(n);                    // rax = &member
        if (mt->kind == TY_ARRAY || mt->kind == TY_STRUCT || mt->kind == TY_UNION) return mt;   // decay
        load_rax(ty_size(mt));
        return mt;
    }
    case N_SIZEOF: {
        Type *t = n->type ? n->type : static_typeof(n->lhs);
        emit("    mov rax, %d", ty_size(t));
        return ty_long();
    }
    case N_CAST: gen_expr(n->lhs); return n->type;
    case N_DEREF: {
        Type *t = gen_expr(n->lhs);
        Type *pt = is_ptrish(t) ? t->ptr : ty_long();
        load_rax(ty_size(pt));
        return pt;
    }
    case N_ADDR: {
        Type *t = gen_addr(n->lhs);
        return ptr_to(t);
    }
    case N_ASSIGN: {
        Type *lt = gen_addr(n->lhs); emit("    push rax");
        int size = ty_size(lt);
        if (n->op == T_ASSIGN) {
            gen_expr(n->rhs);
        } else {
            load_rax(size); emit("    push rax");
            Type *rt = gen_expr(n->rhs);
            emit("    mov rcx, rax"); emit("    pop rax");
            gen_bin(n, n->op - T_PLUSEQ + T_PLUS, lt, rt);
        }
        emit("    pop rcx");
        store_rcx_rax(size);
        if (!n->unused) promote_rax(size);
        return lt;
    }
    case N_POST: {                                 // x++ / x--  (returns old value)
        Type *lt = gen_addr(n->lhs);
        int step = is_ptrish(lt) ? elem_size(lt) : 1;
        int sz = ty_size(lt);
        emit("    push rax");                      // save &x
        load_rax(sz);                              // rax = old
        emit("    mov rcx, rax");
        if (n->op == T_INC) emit("    add rcx, %d", step);
        else                emit("    sub rcx, %d", step);
        emit("    mov rdx, [rsp]");                // rdx = &x
        if (sz == 1) emit("    mov [rdx], cl"); else emit("    mov [rdx], rcx");
        emit("    add rsp, 8");
        return lt;                                 // rax still = old value
    }
    case N_PRE: {                                  // ++x / --x  (returns new value)
        Type *lt = gen_addr(n->lhs);
        int step = is_ptrish(lt) ? elem_size(lt) : 1;
        int sz = ty_size(lt);
        emit("    push rax");                      // save &x
        load_rax(sz);                              // rax = old
        if (n->op == T_INC) emit("    add rax, %d", step);
        else                emit("    sub rax, %d", step);
        emit("    mov rcx, [rsp]");                // rcx = &x
        store_rcx_rax(sz);                         // *&x = new (rax)
        emit("    add rsp, 8");
        return lt;                                 // rax = new value
    }
    case N_TERNARY: {
        int els = label_id++, end = label_id++;
        gen_expr(n->cond); emit("    test rax, rax"); emit("    jz .L%d", els);
        gen_expr(n->lhs);  emit("    jmp .L%d", end);
        emit(".L%d:", els); gen_expr(n->rhs);
        emit(".L%d:", end);
        return ty_long();
    }
    case N_UNARY:
        gen_expr(n->lhs);
        if      (n->op == T_MINUS)  emit("    neg rax");
        else if (n->op == T_BITNOT) emit("    not rax");
        else { emit("    test rax, rax"); emit("    sete al"); emit("    movzx rax, al"); }
        return ty_long();
    case N_LOGAND: {
        int f = label_id++, e = label_id++;
        gen_expr(n->lhs); emit("    test rax, rax"); emit("    jz .L%d", f);
        gen_expr(n->rhs); emit("    test rax, rax"); emit("    jz .L%d", f);
        emit("    mov rax, 1"); emit("    jmp .L%d", e);
        emit(".L%d:", f); emit("    mov rax, 0"); emit(".L%d:", e);
        return ty_long();
    }
    case N_LOGOR: {
        int tl = label_id++, e = label_id++;
        gen_expr(n->lhs); emit("    test rax, rax"); emit("    jnz .L%d", tl);
        gen_expr(n->rhs); emit("    test rax, rax"); emit("    jnz .L%d", tl);
        emit("    mov rax, 0"); emit("    jmp .L%d", e);
        emit(".L%d:", tl); emit("    mov rax, 1"); emit(".L%d:", e);
        return ty_long();
    }
    case N_CALL: {
        // ---- variadic built-ins (handled inline, not real calls) ----
        if (n->name == ID__BUILTIN_VA_START) {
            gen_addr(n->args[0]);                  // rax = &ap
            emit("    mov rcx, rax");
            emit("    lea rax, [rbp - %d]", cur_va_off - cur_named * 8);  // &save[named]
            emit("    mov [rcx], rax");            // ap = first vararg slot
            return ty_long();
        }
        if (n->name == ID__BUILTIN_VA_ARG) {
            gen_addr(n->args[0]);                  // rax = &ap
            emit("    mov rcx, rax");              // rcx = &ap
            emit("    mov rax, [rcx]");            // rax = ap
            emit("    mov rdx, [rax]");            // rdx = *ap  (the argument value)
            emit("    add rax, 8");
            emit("    mov [rcx], rax");            // ap += 8
            emit("    mov rax, rdx");
            return ty_long();
        }
        if (n->name == ID__BUILTIN_VA_END) return ty_long();   // no-op

        // XXX: should look up symbol instead of global function to handle function pointers
        Func *fn = find_func(n->name);
        if (fn && fn->nparams != n->nargs && (!fn->is_variadic || fn->nparams > n->nargs)) {
            warning(n->lineno, "argument count mismatch '%s' expects %d, got %d",
                    atom_str(n->name), fn->nparams, n->nargs);
        }
        // XXX: should check and convert arguments according to prototype
        // XXX: here we could support default argument values
        for (int i = 0; i < n->nargs; i++) { gen_expr(n->args[i]); emit("    push rax"); }
        for (int i = n->nargs - 1; i >= 0; i--) emit("    pop %s", ARGREG[i]);
        if (!fn || fn->is_variadic) emit("    xor eax, eax"); // variadic-safe; harmless otherwise
        emit("    call %s", atom_str(n->name));
        if (fn) return fn->rtype;
        warning(n->lineno, "function not found '%s'", atom_str(n->name));
        return ty_long();
    }
    case N_BIN: {
        if (n->op == T_COMMA) { n->lhs->unused = true; gen_expr(n->lhs); return gen_expr(n->rhs); }
        Type *lt = gen_expr(n->lhs); emit("    push rax");
        Type *rt = gen_expr(n->rhs); emit("    mov rcx, rax"); emit("    pop rax");
        return gen_bin(n, n->op, lt, rt);
    }
    default: error(n, "cannot generate expression"); return 0;
    }
}

static void gen_asm(Node *n) {
    // split decoded asm text on newlines and ';' — emit each instruction line
    const char *p = atom_str(n->str);
    char line[512]; int i = 0;
    for (;; p++) {
        char c = *p;
        if (c == '\n' || c == ';' || c == 0) {
            line[i] = 0;
            // trim leading spaces
            char *q = line; q += skip_blanks(q);
            if (*q) emit("    %s", q);
            i = 0;
            if (c == 0) break;
        } else if (i < 511) line[i++] = c;
    }
}

// break/continue target stack
static int brk_lbl[64], cont_lbl[64], loop_sp;
static void loop_push(int b, int c) { brk_lbl[loop_sp] = b; cont_lbl[loop_sp] = c; loop_sp++; }
static void loop_pop(void) { loop_sp--; }

static void gen_stmt(Node *n) {
    switch (n->kind) {
    case N_BLOCK: for (int i = 0; i < n->nbody; i++) gen_stmt(n->body[i]); break;
    case N_EMPTY: break;
    case N_DECL:
        if (n->init) {
            Sym *s = lookup(n->name);
            if (!s) error(n, "symbol not found '%s'", atom_str(n->name));
            emit("    lea rax, [rbp - %d]", s->offset);
            emit("    push rax");
            gen_expr(n->init);
            emit("    pop rcx");
            store_rcx_rax(ty_size(s->type));
        }
        break;
    case N_EXPR: n->lhs->unused = true; gen_expr(n->lhs); break;
    case N_RETURN:
        if (n->lhs) gen_expr(n->lhs);
        emit("    leave"); emit("    ret");
        break;
    case N_IF: {
        int els = label_id++, end = label_id++;
        gen_expr(n->cond); emit("    test rax, rax"); emit("    jz .L%d", els);
        gen_stmt(n->lhs);  emit("    jmp .L%d", end);
        emit(".L%d:", els); if (n->rhs) gen_stmt(n->rhs);
        emit(".L%d:", end);
        break;
    }
    case N_WHILE: {
        int top = label_id++, end = label_id++;
        emit(".L%d:", top);
        gen_expr(n->cond); emit("    test rax, rax"); emit("    jz .L%d", end);
        loop_push(end, top); gen_stmt(n->lhs); loop_pop();
        emit("    jmp .L%d", top);
        emit(".L%d:", end);
        break;
    }
    case N_DOWHILE: {
        int top = label_id++, cont = label_id++, end = label_id++;
        emit(".L%d:", top);
        loop_push(end, cont); gen_stmt(n->lhs); loop_pop();
        emit(".L%d:", cont);
        gen_expr(n->cond); emit("    test rax, rax"); emit("    jnz .L%d", top);
        emit(".L%d:", end);
        break;
    }
    case N_FOR: {
        int top = label_id++, cont = label_id++, end = label_id++;
        if (n->init) gen_stmt(n->init);
        emit(".L%d:", top);
        if (n->cond) { gen_expr(n->cond); emit("    test rax, rax"); emit("    jz .L%d", end); }
        loop_push(end, cont); gen_stmt(n->lhs); loop_pop();
        emit(".L%d:", cont);
        if (n->rhs) { n->rhs->unused = true; gen_expr(n->rhs); }  // step
        emit("    jmp .L%d", top);
        emit(".L%d:", end);
        break;
    }
    case N_BREAK:
        if (loop_sp == 0) error(n, "'break' outside loop or switch");
        emit("    jmp .L%d", brk_lbl[loop_sp-1]);
        break;
    case N_CONTINUE:
        if (loop_sp == 0) error(n, "'continue' outside loop");
        emit("    jmp .L%d", cont_lbl[loop_sp-1]);
        break;
    case N_SWITCH: {
        int end = label_id++;
        int def = n->rhs ? (int)n->rhs->ival : -1;

        gen_expr(n->cond);
        // Emit "if (switch_value == case_value) goto case_label" for every case.
        // XXX: should check for duplicates
        for (Node *e = n->cases; e; e = e->lhs) {
            long val;
            if (!eval_expr(e->cond, &val)) error(e->cond, "'case' expression is not constant");
            emit("    cmp rax, %ld", val);
            emit("    jz .S%d", (int)e->ival);
        }
        if (def != -1) emit("    jmp .S%d", def);   // no match -> default
        else           emit("    jmp .L%d", end);   // no match, no default -> done

        // break exits the switch; continue passes through to the enclosing loop
        int current_cont = (loop_sp > 0) ? cont_lbl[loop_sp-1] : -1;
        loop_push(end, current_cont);
        gen_stmt(n->lhs);               // the body places the case labels inline
        loop_pop();

        emit(".L%d:", end);
        break;
    }
    case N_CASE:
    case N_DEFAULT:
    case N_LABEL:
        emit(".S%d:", (int)n->ival);
        break;
    case N_GOTO: {
        Label *lab = find_label(n->name);
        if (!lab) {
            error(n, "label '%s' not found", atom_str(n->name));
            break;
        }
        lab->used = true;
        emit("    jmp .S%d", lab->num);
        break;
    }
    case N_ASM: gen_asm(n); break;
    default: gen_expr(n); break;                    // expression used as statement
    }
}

static void gen_func(Func *fn) {
    if (!fn->body) return;  // external function prototype
    this_fn = fn;
    locals = NULL; frame_size = 0;
    // params first (so they get the lowest offsets, in declared order)
    for (int i = 0; i < fn->nparams; i++) add_local(fn->params[i]->name, fn->params[i]->lineno, fn->ptype[i]);
    collect_locals(fn->body);
    // reserve a 48-byte register save area for variadic functions
    int va_off = 0;
    if (fn->is_variadic) { frame_size += 48; va_off = frame_size; }
    cur_va_off = va_off; cur_named = fn->nparams;
    int fs = (frame_size + 15) & ~15;

    emit("%s:", atom_str(fn->name));
    emit("    push rbp");
    emit("    mov rbp, rsp");
    if (fs > 0) emit("    sub rsp, %d", fs);
    for (int i = 0; i < fn->nparams && i < 6; i++) {
        Sym *s = sym_find(locals, fn->params[i]->name);
        int sz = ty_size(s->type);
        if (sz == 1) emit("    mov [rbp - %d], %s", s->offset,
                          i==0?"dil":i==1?"sil":i==2?"dl":i==3?"cl":i==4?"r8b":"r9b");
        else emit("    mov [rbp - %d], %s", s->offset, ARGREG[i]);
    }
    if (fn->is_variadic) {
        // spill all six integer arg registers so va_arg can walk them
        //const char *r[6] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
        for (int i = 0; i < 6; i++) emit("    mov [rbp - %d], %s", va_off - i * 8, ARGREG[i]);
    }
    gen_stmt(fn->body);
    emit("    leave"); emit("    ret");             // safety epilogue
}

static void emit_init(Type *t, atom_t name, Node *init) {
    if (init) {
        long ival;
        switch (t->kind) {
        case TY_VOID:
            break;
        case TY_INT:
        case TY_LONG:
        case TY_ENUM:
            if (eval_expr(init, &ival)) {
                if (name) emit("%s:", atom_str(name));
                emit("    dq %ld", ival);
                return;
            }
            break;
        case TY_CHAR:
            if (eval_expr(init, &ival)) {
                if (name) emit("%s:", atom_str(name));
                emit("    db %d", (unsigned char)ival);
                return;
            }
            break;
        case TY_PTR:
            if (t->ptr->kind == TY_CHAR) {
                if (init->kind == N_STR) {
                    atom_flags(init->str) |= ATOM_USED;
                    if (name) emit("%s:", atom_str(name));
                    emit("    dq .LC%d", init->str);
                    return;
                }
            }
            if (init->kind == N_NUM) {
                if (eval_expr(init, &ival)) {
                    if (name) emit("%s:", atom_str(name));
                    emit("    dq %ld", ival);
                    return;
                }
            }
            // XXX: support other initializers
            break;
        case TY_ARRAY:
            if (init->kind != N_BLOCK) break;
            if (name) emit("%s:", atom_str(name));
            for (int i = 0; i < t->arr_len; i++) {
                emit_init(t->ptr, 0, init->body[i]);
            }
            return;
        case TY_STRUCT:
        case TY_UNION:
            if (init->kind != N_BLOCK) break;
            if (name) emit("%s:", atom_str(name));
            int i = 0;
            for (Member *m = t->members; m; m = m->next, i++) {
                int pad = m->pad;
                emit_init(m->type, 0, init->body[i]);
                if (pad) emit("    .zero %d", pad);
                if (t->kind == TY_UNION) break;
            }
            return;
        }
        if (name) { warning(init->lineno, "unsupported initializer for '%s'", atom_str(name)); }
        else { warning(init->lineno, "unsupported initializer"); }
    }
    int sz = ty_size(t); if (sz < 1) sz = 8;
    if (name) emit("%s: .zero %d", atom_str(name), sz);
    else emit("    .zero %d", sz);
}

int output_tokens(FILE *fp, Token *t, int n) {
    // XXX: should pretty-print with auto indent
    char buf[8192];
    char c;
    int line = 1;
    for (int i = 0; i < n; i++, t++) {
        while (line < t->lineno) { fprintf(fp, "\n"); line++; }
        switch (t->kind) {
        case T_EOF:   break;
        case T_NUM:   fprintf(fp, " %ld", t->ival); break;
        case T_ID:    fprintf(fp, " %s", atom_str(t->text)); break;
        case T_STR:
            gen_quoted_string(buf, sizeof(buf), atom_str(t->text), atom_len(t->text), '"');
            fprintf(fp, " \"%s\"", buf);
            break;
        case T_CHAR:
            c = (char)t->ival;
            gen_quoted_string(buf, sizeof(buf), &c, 1, '\'');
            fprintf(fp, " '%s'", buf);
            break;
        default:    fprintf(fp, " %s", atom_str(t->kind)); break;
        }
    }
    return 0;
}

// =====================================================================
// 8. MAIN
// =====================================================================

void usage(const char *msg, const char *arg) {
    if (msg) { fprintf(stderr, "%s: %s%s\n", progname, msg, arg); }
    fprintf(stderr, "Usage: %s [OPTIONS] <input.c> [<output.s]\n", progname);
    if (!msg) {
        fprintf(stderr,
                "  --kernel       kernel mode (no bss, no _start)\n"
                "  --libc         hosted mode (no _start, link to the C library\n"
                "  -E             output preprocessed tokens\n"
                "  -O             perform optimizations\n"
                "  -o <output>    set the output filename\n"
                "  -v  --verbose  output progress messages\n");
    }
    exit(1);
}

int main(int argc, char **argv) {
    // Optional flags may precede the file names.  --kernel suppresses the
    // Linux _start/exit stub so a bare-metal boot stub can provide the entry
    // point and simply call main() (no Linux syscalls exist in a kernel).
    progname = argv[0];
    if (argc == 1) usage(NULL, NULL);
    bool kernel_mode = false, preprocess_mode = false, libc_mode = false;
    const char *inpath = NULL, *outpath = NULL;
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (!strcmp(arg, "--kernel") || !strcmp(arg, "-k")) kernel_mode = true;
        else if (!strcmp(arg, "--help") || !strcmp(arg, "-?")) usage(NULL, NULL);
        else if (!strcmp(arg, "--libc")) libc_mode = true;
        else if (!strcmp(arg, "-E")) preprocess_mode = true;
        else if (!strcmp(arg, "-O")) optimize++;
        else if (!strcmp(arg, "--verbose") || !strcmp(arg, "-v")) verbose = true;
        else if (!strcmp(arg, "-o")) { if (!argv[i+1]) usage("missing output filename", ""); outpath = argv[++i]; }
        else if (*arg == '-') usage("invalid option: ", arg);
        else if (!inpath)  inpath  = arg;
        else if (!outpath) outpath = arg;
        else usage("too many arguments", "");
    }
    if (!inpath) usage("missing filename", "");

    lex_init();
    filename = inpath;
    lineno = 1;
    preprocess(inpath);
    lex();

    if (!preprocess_mode) { while (!at(T_EOF)) parse_toplevel(); }
    if (!outpath) {
        if (preprocess_mode) { fout = stdout; }
        else {
            int len = strlen(inpath);
            if (len > 2 && !memcmp(inpath + len - 2, ".c", 2)) len -= 2;
            char *p = alloc(len + 3, 1);
            memcpy(p, inpath, len); strcpy(p + len, ".s"); outpath = p;
        }
    }
    if (!fout) {
        if (!strcmp(outpath, "-")) fout = stdout;
        else fout = fopen(outpath, "w");
        if (!fout) { perror("fopen"); return 1; }
    }
    if (preprocess_mode) {
        int status = output_tokens(fout, toks, ntok);
        if (fout != stdout) fclose(fout);
        return status;
    }

    check_used_func(ID_MAIN);

    emit(".intel_syntax noprefix");
    emit("    .section .text");
    emit("    .globl main");

    if (!kernel_mode && !libc_mode) {
        // hosted freestanding entry point: run main, then exit(rax)
        emit("    .globl _start");
        emit("_start:");
        emit("    call main");
        emit("    mov rdi, rax");
        emit("    mov rax, 60");
        emit("    syscall");
    }

    for (Func *f = funcs; f; f = f->next) if (f->used) gen_func(f);

    // Globals.  Hosted mode puts them in .bss (zeroed by the loader).  Kernel
    // mode uses .data so the zero bytes are emitted into the object and survive
    // an objcopy to a flat binary — the bare-metal image needs no separate
    // .bss zero-fill step.
    // XXX: should output global data in order:
    // - initialized data in .data and .rodata sections
    // - strings in .rodata sections
    // - uninitialized data in .bss sections
    emit("    .section .data");
    emit("    .align 8");
    for (Sym *g = globals; g; g = g->next) {
        if (g->is_constant) continue;
        if (!kernel_mode && !g->init) continue;
        emit_init(g->type, g->name, g->init);
    }
    if (!kernel_mode) {
        emit("    .section .bss");
        emit("    .align 8");
        for (Sym *g = globals; g; g = g->next) {
            if (g->is_constant || g->init) continue;
            emit_init(g->type, g->name, g->init);
        }
    }
    emit("    .section .rodata");
    for (atom_t s = 0; s < natoms; s++) {
        if (atom_flags(s) & ATOM_USED) gen_string_def(s);
    }

    fclose(fout);
    if (verbose) printf("Compiled %s -> %s%s\n", inpath, outpath, kernel_mode ? " (kernel mode)" : "");
    return 0;
}
