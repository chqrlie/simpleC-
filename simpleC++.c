// =====================================================================
// simpleC++  (nano_cc) — a tiny single-file C-subset -> x86_64 compiler
// =====================================================================
// Emits GNU-as compatible x86_64 assembly (.intel_syntax noprefix) that
// links with:  gcc -nostdlib -no-pie out.s -o prog
//
// Build:  gcc -std=c11 -O2 -Wall -Wextra -o nano_cc 'simpleC++.c'
// Use:    ./nano_cc input.c output.s
// =====================================================================

//#define NO_REALLOC

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <malloc.h>

#if (defined(__GNUC__) || defined(__TINYC__))
#define attr_printf(a, b)  __attribute__((format(printf, a, b)))
#else
#define attr_printf(a, b)
#endif

// ---------------------------------------------------------------------
// Errors / allocation
// ---------------------------------------------------------------------
const char *progname;
static bool has_library;
static const char *src_name[32];
static int src_pos;
static int optimize;
static bool debug, verbose;
static void err_message(int pos, const char *kind, const char *fmt, va_list ap) {
    int fn = pos >> 24, lineno = pos & 0xffffff;
    if (pos && src_name[fn]) fprintf(stderr, "%s:%d: %s: ", src_name[fn], lineno, kind);
    else fprintf(stderr, "%s: %s: ", progname, kind);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}
static _Noreturn void die(const char *fmt, ...) attr_printf(1,2);
static void die(const char *fmt, ...) {
    va_list a; va_start(a, fmt); err_message(src_pos, "error", fmt, a); va_end(a);
    exit(1);
}
static void warning(int pos, const char *fmt, ...) attr_printf(2,3);
static void warning(int pos, const char *fmt, ...) {
    va_list a; va_start(a, fmt); err_message(pos, "warning", fmt, a); va_end(a);
}
static int xdigit(int d) {
    if (d >= '0' && d <= '9') return d - '0';
    if ((d |= 0x20) >= 'a' && d <= 'z') return d - 'a' + 10;
    return 255;
}
static size_t skip_blanks(const char *s) {
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    return i;
}
static size_t trim_len(const char *s) {
    size_t i = strlen(s);
    while (i && (s[i - 1] == ' ' || s[i - 1] == '\t')) i--;
    return i;
}
static size_t skip_word(const char *s) {
    size_t i = 0;
    while (isalnum((unsigned char)s[i]) || s[i] == '_') i++;
    return i;
}
static size_t skip_until(const char *s, char c) {
    size_t i = 0;
    while (s[i] && s[i] != '\n' && s[i] != c) i++;
    return i;
}
static size_t skip_string(const char *s, size_t *slen) {
    char sep = *s;
    for (size_t i = 1;; i++) {
        if (!s[i]) { if (slen) *slen = 0; return i; }
        if (s[i] == sep) { if (slen) *slen = 1; return i + 1; }
        if (s[i] == '\\' && s[i+1]) i++;
    }
}

// safe string copy with truncation and detection
static size_t pstrcpy(char *dest, size_t size, const char *src) {
    if (size) {
        for (size_t i = 0; i < size; i++) if ((dest[i] = src[i]) == 0) return i;
        dest[size - 1] = 0;
    }
    return size;
}
#if 0
// safe limited string copy with truncation and detection
static size_t pstrncpy(char *dest, size_t size, const char *src, size_t n) {
    if (size) {
        size_t i;
        for (i = 0; i < size && n; i++, n--) if ((dest[i] = src[i]) == 0) return i;
        if (i < size) { dest[i] = 0; return i; }
        dest[size - 1] = 0;
    }
    return size;
}
#endif
// safe fixed string copy with truncation and detection
static size_t pmemcpy(char *dest, size_t size, const char *src, size_t n) {
    size_t i = 0;
    for (i = 0; i < size && n; i++, n--) dest[i] = src[i];
    return i;
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
#ifndef NO_REALLOC    // use realloc if available
    void *new_ptr = realloc(ptr, new_nelems * size);
    if (!new_ptr) die("out of memory");
#else
    void *new_ptr = alloc(new_nelems, size);
    if (ptr) { memcpy(new_ptr, ptr, *nelems * size); free(ptr); }
#endif
    *nelems = new_nelems;
    return new_ptr;
}
static long now(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

// Reallocatable string buffer
typedef struct sbuf_t { char *buf; size_t len, cap; } sbuf_t;

static bool sbuf_init(sbuf_t *sb, size_t cap) {
    sb->len = 0; sb->cap = cap; sb->buf = cap ? alloc(cap, 1) : NULL; return true;
}
static void sbuf_deinit(sbuf_t *sb) { free(sb->buf); }
static bool sbuf_realloc(sbuf_t *sb) {
    size_t cap = sb->cap; sb->buf = reallocate(sb->buf, &cap, 1); sb->cap = cap; return true;
}
static char *sbuf_getptr(sbuf_t *sb) { if (sb->cap) sb->buf[sb->len] = '\0'; return sb->buf; }
static bool sbuf_putc(sbuf_t *sb, char c) {
    if (sb->len + 2 > sb->cap) sbuf_realloc(sb); sb->buf[sb->len++] = c; return true;
}
static bool sbuf_put(sbuf_t *sb, const char *s, size_t len) {
    while (sb->len + len + 1 > sb->cap) sbuf_realloc(sb);
    memcpy(sb->buf + sb->len, s, len); sb->len += len; return true;
}
//static bool sbuf_puts(sbuf_t *sb, const char *s) { return sbuf_put(sb, s, strlen(s)); }
static size_t sbuf_load_file(sbuf_t *sb, FILE *f) {
    for (;;) {
        if (sb->len + 1 >= sb->cap) sbuf_realloc(sb);
        size_t nread = fread(sb->buf + sb->len, 1, sb->cap - sb->len - 1, f);
        if (!nread) return sb->len;
        sb->len += nread;
    }
}

// =====================================================================
// 0.1. ATOM TABLE -> convert names and strings to single atoms
// =====================================================================

typedef unsigned int atom_t;
#define ATOM_MACRO 1
#define ATOM_USED  2
typedef struct Atom { atom_t next; unsigned int len; unsigned char flags; char str[7]; } Atom;
static Atom **atoms;
static size_t natoms, atoms_cap;
#define ATOM_HASH_LEN  1023
static atom_t atom_hash[ATOM_HASH_LEN];

static const char *atom_str(atom_t i) { return atoms[i]->str; }
static size_t atom_len(atom_t i) { return atoms[i]->len; }
#define atom_flags(i)  atoms[i]->flags
static atom_t new_atom_len(const char *str, size_t len) {
    if (!atoms) {
        atoms = alloc(atoms_cap = 1024, sizeof(Atom*));
        atoms[0] = allocz(1, sizeof(Atom)); natoms = 1;
    }
    if (!len) { return 0; } // special case the empty string
    unsigned long hash = 0;
    for (size_t i = 0; i < len; i++) hash = hash * 37 + (str[i] & 255);
    // Achtung Minen! we do not support unsigned arithmetics yet
    hash = hash % ATOM_HASH_LEN;
    atom_t a = atom_hash[hash];
    while (a) {
        Atom *ap = atoms[a];
        if (ap->len == len && !memcmp(ap->str, str, len)) return a;
        a = ap->next;
    }
    if (natoms >= atoms_cap) { atoms = reallocate(atoms, &atoms_cap, sizeof(Atom*)); }
    Atom *ap = alloc(1, sizeof(Atom) - 7 + len + 1);
    ap->next = atom_hash[hash];
    ap->len = (unsigned int)len;
    ap->flags = 0;
    memcpy(ap->str, str, len);
    ap->str[len] = 0;
    a = (atom_t)natoms++;
    atom_hash[hash] = a;
    atoms[a] = ap;
    return a;
}
static atom_t new_atom(const char *str) { return new_atom_len(str, strlen(str)); }

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

#define IS_TYPE(k)    ((k) >= K_INT && (k) < K_IF)
#define IS_KEYWORD(k) ((k) >= K_INT && (k) < K_IFDEF)
    K_INT, K_LONG, K_CHAR, K_SHORT, K_VOID, K_FLOAT, K_DOUBLE,
    K_SIGNED, K_UNSIGNED, K_CONST, K_VOLATILE, K_INLINE, K_NORETURN,
    K_AUTO, K_STATIC, K_REGISTER, K_EXTERN, K_THREAD_LOCAL, K_TYPEDEF,
    K_ENUM, K_STRUCT, K_UNION,

    K_IF, K_ELSE, K_WHILE, K_RETURN, K_ASM, K__ASM__,
    K_FOR, K_DO, K_BREAK, K_CONTINUE, K_SIZEOF,
    K_SWITCH, K_CASE, K_DEFAULT, K_GOTO,

    K_IFDEF, K_IFNDEF, K_ELIF, K_ENDIF, K_DEFINE, K_UNDEF,
    K_INCLUDE, K_LINE,

#define IS_BUILTIN(k) ((k) >= ID__BUILTIN_VA_START && (k) < ID_START)
    ID__BUILTIN_VA_START, ID__BUILTIN_VA_ARG, ID__BUILTIN_VA_END,
    ID__BUILTIN_BSWAP16, ID__BUILTIN_BSWAP32, ID__BUILTIN_BSWAP64,
    ID__BUILTIN_CLZ, ID__BUILTIN_CTZ,
    ID__BUILTIN_ROTATE_LEFT, ID__BUILTIN_ROTATE_RIGHT,
    ID__SYSCALL, ID__RDTSC, ID__RDTSCP, ID_ABS, ID_LABS,

    ID_START, ID_MAIN, ID_PRINTF, ID_PUTS, ID_STRLEN, ID_STRCPY, ID_MEMCPY,
    T_count
};
static const char *token_name[T_count] = {
    "", "<EOF>", "number", "identifier", "string", "char const",
    "+", "-", "*", "/", "%", "|", "&", "^", "<<", ">>",
    "==", "!=", "<", ">", "<=", ">=", "&&", "||", "!", "~", "++", "--",
    "=", "+=", "-=", "*=", "/=", "%=", "|=", "&=", "^=", "<<=", ">>=",
    "(", ")", "[", "]", "{", "}", ";", ",", "?", ":", ".", "->", "...",
    "int", "long", "char", "short", "void", "float", "double",
    "signed", "unsigned", "const", "volatile", "inline", "_Noreturn",
    "auto", "static", "register", "extern", "thread_local", "typedef",
    "enum", "struct", "union",
    "if", "else", "while", "return", "asm", "__asm__",
    "for", "do", "break", "continue", "sizeof",
    "switch", "case", "default", "goto",
    "ifdef", "ifndef", "elif", "endif", "define", "undef",
    "include", "line",
    "__builtin_va_start", "__builtin_va_arg", "__builtin_va_end",
    "__builtin_bswap16", "__builtin_bswap32", "__builtin_bswap64",
    "__builtin_clz", "__builtin_ctz",
    "__builtin_rotate_left",  "__builtin_rotate_right",
    "__syscall", "__rdtsc", "__rdtscp", "abs", "labs",
    "_start", "main", "printf", "puts", "strlen", "strcpy", "memcpy",
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

// A macro is either object-like (#define PI 3) or function-like
// (#define MAX(a,b) ((a)>(b)?(a):(b))).  For function-like macros we keep
// the parameter names so expand_line() can substitute call arguments.
typedef struct Macro {
    atom_t name; int pos, nparams, len; atom_t params[8]; struct Macro *next; char def[8];
} Macro;
static Macro *macros;

static Macro *macro_find(atom_t name) {
    if (atom_flags(name) & ATOM_MACRO) {
        for (Macro *m = macros; m; m = m->next) {
            if (m->name == name) return m;
        }
    }
    return NULL;
}
static Macro *macro_define(atom_t name, int pos, int nparams, atom_t *params,
                           int len, const char *def) {
    Macro *m = allocz(1, sizeof(Macro) - 8 + len + 1);
    atom_flags(name) |= ATOM_MACRO;
    m->name = name;
    m->pos = pos;
    m->len = len;
    m->nparams = nparams;
    if (nparams > 0) memcpy(m->params, params, nparams * sizeof(atom_t));
    memcpy(m->def, def, len);
    m->def[len] = '\0';
    m->next = macros;
    return macros = m;
}
static void macro_undef(atom_t name) {
    if (atom_flags(name) & ATOM_MACRO) {
        Macro *m, **tailp = &macros;
        while ((m = *tailp) != NULL) {
            if (m->name == name) { *tailp = m->next; free(m); break; }
            tailp = &m->next;
        }
        atom_flags(name) &= ~ATOM_MACRO;
    }
}

typedef struct MacroArguments {
    const char *argv[8];
    size_t len[8];
    int argc;
} MacroArguments;

static int find_macro_param(const Macro *m, atom_t name) {
    for (int i = 0; i < m->nparams; i++) if (m->params[i] == name) return i;
    return -1;
}

// Substitute a function-like macro body: copy m->def into out[], replacing
// each parameter name with the matching call argument text. ma->argv[k] holds the
// (already whitespace-trimmed) text of the k-th argument of length ma->len[k].
static size_t subst_macro_body(const Macro *m, MacroArguments *ma, char *out, size_t j, size_t cap) {
    const char *p = m->def;
    const char *q = p;
    char vc;
    while ((vc = *p) != 0) {
        if (isalpha((unsigned char)vc) || vc == '_') {
            size_t k = skip_word(p);
            int pi = find_macro_param(m, new_atom_len(p, k));
            if (pi >= 0) {
                j += pmemcpy(out + j, cap - j, q, (size_t)(p - q));
                j += pmemcpy(out + j, cap - j, ma->argv[pi], ma->len[pi]);
                q = p += k;
                continue;
            }
        }
        p++;
    }
    j += pmemcpy(out + j, cap - j, q, (size_t)(p - q));
    if (j == cap && cap) out[cap - 1] = '\0';
    return j;
}

// Expand object-like and simple function-like macros in one logical line.
static void expand_line(const char *in, sbuf_t *sb) {
    char work[8192];
    const char *p = in;
    for (int pass = 0;; pass++) {
        char out[8192]; size_t j = 0; bool changed = false;
        const char *q = p;
        char c;
        while ((c = *p) != '\0') {
            if (c == '"' || c == '\'') { p += skip_string(p, NULL); continue; }
            if (isalpha((unsigned char)c) || c == '_') {
                size_t k = skip_word(p);
                Macro *m = macro_find(new_atom_len(p, k));
                size_t b = skip_blanks(p + k);
                if (!m || (m->nparams >= 0 && p[k+b] != '(')) { p += k+b; continue; }
                j += pmemcpy(out + j, sizeof(out) - j, q, (size_t)(p - q));
                q = p += k;
                MacroArguments ma; ma.argc = 0;
                if (m->nparams >= 0) {
                    // XXX: This does not work if macro arguments span multiple lines
                    int depth = 1;
                    p += b + 1;   // past blanks and '('
                    const char *e = q = p += skip_blanks(p);
                    if (*p == ')' && m->nparams == 0) {
                        q = ++p;
                        j += pmemcpy(out + j, sizeof(out) - j, m->def, m->len);
                        changed = true;
                        continue;
                    }
                    for (;;) {
                        char cc = *p;
                        if (cc == '\0') die("macro '%s' argument list spans multiple lines", atom_str(m->name));
                        if ((cc == ')' && --depth == 0) || (cc == ',' && depth == 1)) { // end of argument
                            if (ma.argc >= m->nparams) die("too many arguments for macro '%s'", atom_str(m->name));
                            ma.argv[ma.argc] = q;
                            ma.len[ma.argc] = (size_t)(e - q);
                            ma.argc++;
                            q = ++p;   // past ')' or ','
                            if (cc == ')') {
                                if (ma.argc != m->nparams) die("missing arguments for macro '%s'", atom_str(m->name));
                                break;
                            }
                            e = q = p += skip_blanks(p);
                        } else {
                            if (cc == ' ' || cc == '\t') { p++; continue; }
                            if (cc == '(') { depth++; }
                            if (cc == '"' || cc == '\'') { p += skip_string(p, NULL); } else p++;
                            e = p;
                        }
                    }
                }
                j = subst_macro_body(m, &ma, out, j, sizeof(out));
                changed = true;
            } else {
                p++;
            }
        }
        if (!changed) { sbuf_put(sb, q, (size_t)(p - q)); return; }
        j += pmemcpy(out + j, sizeof(out) - j, q, (size_t)(p - q));
        if (j >= sizeof(out)) die("macro expansion overflow");
        if (pass == 8) { sbuf_put(sb, out, j); return; }    // should complain about recursion
        memcpy(work, out, j); work[j] = '\0'; p = work;
    }
}

// Strip // and /* */ comments (string/char aware) in place.
static size_t strip_comments(char *s) {
    char *dst = s, *out = dst;
    const char *p = s, *q = p;
    char c;
    while ((c = *p) != '\0') {
        if (c == '"' || c == '\'') {
            p += skip_string(p, NULL);
        } else {
            p++;
            if (c == '/' && (*p == '/' || *p == '*')) {
                const char *e = p - 1;
                while (q < e && isblank((unsigned char)e[-1])) e--;
                while (q < e) *out++ = *q++;
                if (*p == '/') {
                    while (*p && *p != '\n') p++; q = p;
                } else {
                    while (*++p) {
                        if (*p == '*' && p[1] == '/') { p += 2; break; }
                        if (*p == '\n') *out++ = '\n';    // preserve line numbers
                    }
                    q = p += skip_blanks(p);
                    if (*p != '\n' && out > dst && !isspace((unsigned char)out[-1])) *out++ = ' ';
                }
            }
        }
    }
    while (q < p) *out++ = *q++;
    *out = '\0';
    return out - dst;
}

static void preprocess(const char *path, sbuf_t *sb);

// Process already-comment-stripped text of one file.
static void process_text(const char *text, sbuf_t *sb) {
    int cond_sp = 0; long skip = 0, seen_else = 0;

    const char *cursor = text;
    while (*cursor) {
        char line[8192]; size_t li = 0;
        while (*cursor && *cursor != '\r' && *cursor != '\n') {
            if (li >= sizeof(line)) die("line too long");
            line[li++] = *cursor++;
        }
        line[li] = 0;
        if (*cursor == '\r') cursor++;
        if (*cursor == '\n') cursor++;

        const char *p = line; p += skip_blanks(p);
        if (*p == '#') {
            p++;
            p += skip_blanks(p);
            size_t di = skip_word(p);
            atom_t dir = new_atom_len(p, di); p += di;
            p += skip_blanks(p);
            switch (dir) {
            case K_IFNDEF: case K_IFDEF:
                cond_sp++;
                skip += skip;
                seen_else += seen_else;
                if (!skip) {
                    size_t k = skip_word(p);
                    if (!k) die("expected macro name after '#%s'", atom_str(dir));
                    atom_t nm = new_atom_len(p, k); p += k;
                    int defined = macro_find(nm) != NULL;
                    skip |= (dir == K_IFDEF) ? !defined : defined;
                }
                break;
            case K_IF:  // unsupported #if -> ignore expression, skip body
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
            case K_DEFINE: {
                if (skip) break;
                int pos = src_pos;
                size_t k = skip_word(p);
                if (!k) die("expected macro name after '#%s'", atom_str(dir));
                atom_t nm = new_atom_len(p, k); p += k;
                int nparams = -1; atom_t params[8]; for (k = 0; k < 8; k++) params[k] = 0;
                // function-like macro: '(' immediately after name (no space)
                if (*p == '(') {
                    nparams = 0; p++;
                    p += skip_blanks(p);
                    if (*p && *p != ')') {
                        for (;;) {
                            size_t ai = skip_word(p);
                            if (!ai) die("expected parameter name for macro '%s'", atom_str(nm));
                            if (nparams >= 8) die("too many macro parameters for '%s'", atom_str(nm));
                            params[nparams++] = new_atom_len(p, ai); p += ai;
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
                const char *def = p += skip_blanks(p);
                size_t len = trim_len(p); p += len;
                Macro *m = macro_find(nm);
                if (m) {  // if macro already exists, check if definition is identical
                    if (m->nparams == nparams && !memcmp(m->params, params, sizeof(params))
                    &&  (size_t)m->len == len && !memcmp(m->def, def, len)) break;
                    warning(pos, "macro '%s' redefinition is different", atom_str(nm));
                    macro_undef(nm);
                }
                macro_define(nm, pos, nparams, params, len, def);
                break;
                }
            case K_UNDEF: {
                if (skip) break;
                size_t k = skip_word(p);
                macro_undef(new_atom_len(p, k)); p += k;
                break;
            }
            case K_LINE:
                if (skip) break;
                sbuf_put(sb, "# ", 2); sbuf_put(sb, p, li - (p - line)); break;
            case K_INCLUDE:
                if (skip) break;
                if (*p == '"') {
                    size_t k = skip_until(++p, '"');
                    if (p[k] != '"') die("invalid include file name");
                    atom_t name = new_atom_len(p, k);
                    preprocess(atom_str(name), sb);
                } else
                if (*p == '<') {  // standard header: ignore and include nano libc
                    //size_t k = skip_until(++p, '>');
                    //if (p[k] != '>') die("invalid include file name");
                    if (!has_library) {
                        preprocess("nano-nolibc.h", sb);
                        preprocess("nano-malloc.h", sb);
                        has_library = true;
                    }
                }
                break;
            default:
                if (skip) break;
                if (isdigit((unsigned char)*p)) sbuf_put(sb, line, li); break;
                // ignore other preprocessing directives
                break;
            }
        } else {
            if (!skip) expand_line(line, sb);
        }
        sbuf_putc(sb, '\n'); src_pos++; // preserve line numbers
    }
    if (cond_sp) warning(src_pos, "missing '#endif'");
}

static int update_pos(int pos, const char *path, int lineno) {
    int fn = pos >> 24;
    if (path && *path) {
        for (fn = 0; src_name[fn] && strcmp(src_name[fn], path); fn++) continue;
        src_name[fn] = atom_str(new_atom(path));
    }
    return (fn << 24) + lineno;
}
static void sharp_line(int pos, sbuf_t *sb) {
    if (pos) {
        char buf[300]; int len = snprintf(buf, sizeof(buf), "# %d \"%s\"\n", pos & 0xffffff, src_name[pos >> 24]);
        sbuf_put(sb, buf, len);
    }
}

static void preprocess(const char *path, sbuf_t *sb) {
    FILE *f = fopen(path, "r");
    if (!f) { die("%s: cannot open %s\n", progname, path); }
    sbuf_t raw[1]; sbuf_init(raw, 64 * 1024);
    size_t n = sbuf_load_file(raw, f);
    if (verbose) printf("Read %s: %zu bytes\n", path, n);
    fclose(f);
    int save_pos = src_pos;
    src_pos = update_pos(src_pos, path, 1);
    sharp_line(src_pos, sb);
    raw->len = strip_comments(sbuf_getptr(raw));
    if (verbose) printf("Stripped: %td bytes\n", raw->len);
    process_text(sbuf_getptr(raw), sb);   // may recurse (with its own buffers)
    sharp_line(src_pos = save_pos, sb);
    sbuf_deinit(raw);
    if (verbose) printf("Preprocessed: %zu bytes\n", sb->len);
}

// =====================================================================
// 2. LEXER
// =====================================================================

typedef struct {
    unsigned char kind;
    bool  is_unsigned;   // T_NUM
    bool  is_long;       // T_NUM
    unsigned char base;  // T_NUM
    int   pos;
    long  ival;          // T_NUM / T_CHAR
    atom_t text;         // T_ID, T_STR (decoded bytes)
} Token;

#define MAX_TOK 60000
static Token toks[MAX_TOK];
static size_t ntok;

static Token *add_tok(int kind) { Token *t = &toks[ntok++]; t->kind = kind; t->pos = src_pos; return t; }
static const char *token_str(Token *t) {
    return atom_str(t->kind == T_ID ? t->text : (atom_t)t->kind);
}

static int read_escape(const char *p, size_t *len) { // `p` points just past a backslash
    *len = 1;
    int c = *p++;
    switch (c) {
    case 0: *len = 0; return 0;
    case 'n': return '\n';  case 't': return '\t';  case 'r': return '\r';
    case 'b': return '\b';  case 'f': return '\f';  case 'v': return '\v';
    case '\\': case '\'': case '"': return c;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        c -= '0';
        if (*p >= '0' && *p <= '7') {
            c = (c << 3) + (*p++ - '0'); *len = 2;
            if (*p >= '0' && *p <= '7') {
                c = (c << 3) + (*p++ - '0'); *len = 3;
            }
        }
        return (unsigned char)c;
    case 'x':
        c = 0;
        int d;
        while ((d = xdigit(*p++)) < 16) { c = c * 16 + d; *len += 1; }
        return (unsigned char)c;
    default:
        warning(src_pos, "invalid escape sequence '\\%c'", c);
        return (unsigned char)c;
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

static size_t lex(const char *p) {
    const char *start = p;
    for (;;) {
        p += skip_blanks(p);
        unsigned char c = *p++;
        if (c == '\n') { src_pos++; }
        if (isspace(c)) continue;

        if (c == 0) {
            p--; add_tok(T_EOF);
            if (verbose) printf("Tokenized: %zu tokens\n", ntok);
            return p - start;
        }
        if (isdigit(c)) {
            Token *t = add_tok(T_NUM);
            long v = c - '0';
            int base = 10, d;
            if (c == '0') {
                switch (*p | 0x20) {  // lowercase if letter
                case 'x': base = 16; p++; break;
                case 'b': base = 2;  p++; break;
                case 'o': base = 8;  p++; break;
                default:  base = 8;        break;
                }
            }
            while ((d = xdigit(c = *p)) < base) { v = v * base + d; p++; }
            t->base = base;
            t->ival = v;
            if (isdigit(c)) die("invalid digit '%c' for base %d", c, base);
            if (c == '.' || (c | 0x20) == 'e') die("floating point not supported");
            for (;; c = *++p) {
                switch (c | 0x20) {
                case 'u': t->is_unsigned = true; continue;
                case 'l': t->is_long     = true; continue;
                }
                break;
            }
            if (isalnum(c)) die("integer literal suffix '%c'", c);
            continue;
        }
        if (isalpha(c) || c == '_') {
            size_t k = skip_word(--p);
            atom_t name = new_atom_len(p, k); p += k;
            if (IS_KEYWORD(name)) { add_tok((int)name); continue; }
            Token *t = add_tok(T_ID); t->text = name; ; continue;
        }
        if (c == '"' || c == '\'') {  // string literal / character constant
            char buf[8192]; size_t len = 0;
            const char *thing = (c == '"') ? "string" : "character constant";
            unsigned char ch;
            while ((ch = *p++) != c) {
                if (!ch || ch == '\n') { p--; die("unterminated %s", thing); }
                if (ch == '\\') { size_t k; if (!*p) continue; ch = read_escape(p, &k); p += k; }
                if (len >= sizeof(buf)) die("%s too long", thing);
                buf[len++] = (char)ch;
            }
            Token *t;
            if (ch == '"') { t = add_tok(T_STR); t->text = new_atom_len(buf, len); continue; }
            if (len != 1) die("malformed character constant");
            t = add_tok(T_CHAR); t->ival = *buf; continue;
        }
        if (c == '.') {
            if (isdigit((unsigned char)*p)) { die("floating point not supported"); }
            if (*p == '.' && p[1] == '.') {
                p += 2; add_tok(T_ELLIPSIS); continue;
            }
            add_tok(T_DOT); continue;
        }
        if (c == '#') { // parse # number [filename]
            p += skip_blanks(p);
            const char *filename = NULL;
            int lineno = 0;
            while (isdigit((unsigned char)*p)) lineno = lineno * 10 + (*p++ - '0');
            p += skip_blanks(p);
            if (*p == '"') {
                size_t k = skip_until(++p, '"');
                filename = atom_str(new_atom_len(p, k)); p += k;
            }
            src_pos = update_pos(src_pos, filename, lineno);
            p += skip_until(p, '\n');
            p += (*p == '\n');
            continue;
        }
        if (c == '-' && *p == '>') { p++; add_tok(T_ARROW); continue; }
        const char *pp = ops;
        while (pp < ops + sizeof(ops) && *pp && *pp != c) pp += 5;
        if (*pp) {
            if (pp[2] && *p == c) {
                if (pp[4] && p[1] == '=') { p += 2; add_tok(pp[4]); continue; }
                else { p++; add_tok(pp[2]); continue; }
            }
            if (pp[3] && *p == '=') { p++; add_tok(pp[3]); continue; }
            else { add_tok(pp[1]); continue; }
        }
        warning(src_pos, "unknown character '%c' in source", c);
    }
}

// =====================================================================
// 3. TYPES
// =====================================================================
enum { TY_INT, TY_CHAR, TY_SHORT, TY_LONG, TY_VOID, TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION, TY_ENUM };
typedef struct Type Type;
typedef struct Member { atom_t name; int offset, align, pad; Type *type; struct Node *init; struct Member *next; } Member;
struct Type {
    unsigned char kind, align; bool is_unsigned; int pos; int arr_len; Type *ptr; struct Node *arr_len_expr;
    Member *members; int struct_size; atom_t tag;  // TY_STRUCT / TY_UNION
};

static Type ty_char_s   = { TY_CHAR,  1, false, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_schar_s  = { TY_CHAR,  1, false, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_uchar_s  = { TY_CHAR,  1, true,  0, 0, 0, 0, 0, 0, 0 };
static Type ty_short_s  = { TY_SHORT, 2, false, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_ushort_s = { TY_SHORT, 2, true,  0, 0, 0, 0, 0, 0, 0 };
static Type ty_int_s    = { TY_INT,   4, false, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_uint_s   = { TY_INT,   4, true,  0, 0, 0, 0, 0, 0, 0 };
static Type ty_long_s   = { TY_LONG,  8, false, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_ulong_s  = { TY_LONG,  8, true,  0, 0, 0, 0, 0, 0, 0 };
static Type ty_void_s   = { TY_VOID,  1, false, 0, 0, 0, 0, 0, 0, 0 };

#define ty_char()    &ty_char_s
#define ty_schar()   &ty_schar_s
#define ty_uchar()   &ty_uchar_s
#define ty_short()   &ty_short_s
#define ty_ushort()  &ty_ushort_s
#define ty_int()     &ty_int_s
#define ty_uint()    &ty_uint_s
#define ty_long()    &ty_long_s
#define ty_ulong()   &ty_ulong_s
#define ty_void()    &ty_void_s

static Type *ptr_to(Type *base) {
    Type *t = allocz(1, sizeof(Type)); t->align = 8; t->kind = TY_PTR; t->ptr = base; return t;
}
static int ty_size(Type *t) {
    switch (t->kind) {
        case TY_ARRAY:  return t->arr_len * ty_size(t->ptr);
        case TY_STRUCT: case TY_UNION: return t->struct_size;
        case TY_VOID:   return 0;
        default:        return t->align;
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

enum {
    HAS_INT      = 1 << (K_INT      - K_INT),
    HAS_LONG     = 1 << (K_LONG     - K_INT),
    HAS_CHAR     = 1 << (K_CHAR     - K_INT),
    HAS_SHORT    = 1 << (K_SHORT    - K_INT),
    HAS_VOID     = 1 << (K_VOID     - K_INT),
    HAS_FLOAT    = 1 << (K_FLOAT    - K_INT),
    HAS_DOUBLE   = 1 << (K_DOUBLE   - K_INT),
    HAS_SIGNED   = 1 << (K_SIGNED   - K_INT),
    HAS_UNSIGNED = 1 << (K_UNSIGNED - K_INT),
    HAS_CONST    = 1 << (K_CONST    - K_INT),
    HAS_VOLATILE = 1 << (K_VOLATILE - K_INT),
    HAS_INLINE   = 1 << (K_INLINE   - K_INT),
    HAS_NORETURN = 1 << (K_NORETURN - K_INT),
    HAS_AUTO     = 1 << (K_AUTO     - K_INT),
    HAS_STATIC   = 1 << (K_STATIC   - K_INT),
    HAS_REGISTER = 1 << (K_REGISTER - K_INT),
    HAS_EXTERN   = 1 << (K_EXTERN   - K_INT),
    HAS_THREAD_LOCAL = 1 << (K_THREAD_LOCAL - K_INT),
    HAS_TYPEDEF  = 1 << (K_TYPEDEF  - K_INT),
};

// =====================================================================
// 4. AST
// =====================================================================
enum {
    N_NUM, N_STR, N_VAR, N_CALL, N_ASSIGN, N_BIN, N_UNARY,
    N_POST, N_CAST, N_DEREF, N_ADDR, N_LOGAND, N_LOGOR,
    N_IF, N_WHILE, N_RETURN, N_BLOCK, N_EXPR, N_DECLIST, N_DECL, N_ASM, N_EMPTY,
    N_FOR, N_DOWHILE, N_BREAK, N_CONTINUE, N_TERNARY, N_PRE,
    N_MEMBER, N_SIZEOF,
    N_SWITCH, N_CASE, N_DEFAULT, N_GOTO, N_LABEL,
};

#define HAS_BREAK     1     // bits in n->ival
#define HAS_CONTINUE  2
#define HAS_DEFAULT   4
typedef struct Node Node;
struct Node {
    unsigned char kind;
    unsigned char op;       // token kind for N_BIN / N_POST (T_INC/T_DEC)
#define DISCARD   1         // expression value is discarded: no need to preserve reg value
#define CONST_VAL 2         // expression is integer constant, value in n->ival
    unsigned char flags;
#define MAX_ARGS 6
    unsigned char nargs;    // N_CALL
    int   pos;
    long  ival;             // N_NUM, N_CASE, N_DEFAULT, N_LABEL, N_FOR, N_DO, N_WHILE, N_SWITCH
    atom_t str;             // N_STR, N_ASM decoded text
    atom_t name;            // N_VAR / N_CALL / N_DECL
    Type *type;             // result / declared type
    Node *lhs, *rhs, *cond, *init, *next;
};

static Token *cur(void);
static Node *new_node(int k) {
    Node *n = allocz(1, sizeof(Node));
    n->kind = (unsigned char)k; n->pos = cur()->pos; n->op = (unsigned char)cur()->kind;
    return n;
}
static Node *new_node1(int k, Node *lhs) { Node *n = new_node(k); n->lhs = lhs; return n; }
static Node *new_bin_node(Node *lhs) { return new_node1(N_BIN, lhs); }
static Node *new_num_node(long ival, Type *t) { Node *n = new_node(N_NUM); n->ival = ival; n->type = t; return n; }
static Node *node_last(Node *n) { if (n) while (n->next) n = n->next; return n; }
static int node_length(Node *n) { int len = 0; while (n) { len++; n = n->next; } return len; }

static void error(Node *n, const char *fmt, ...) attr_printf(2,3);
static void error(Node *n, const char *fmt, ...) {
    int pos = n ? n->pos : cur()->pos;
    va_list a; va_start(a, fmt); err_message(pos, "error", fmt, a); va_end(a);
    exit(1);
}
static Type *array_of(Type *base, Node *len_expr) {
    Type *arr = allocz(1, sizeof(Type)); arr->align = base->align; arr->kind = TY_ARRAY; arr->ptr = base;
    arr->arr_len = -1;
    arr->arr_len_expr = len_expr;
    if (len_expr && len_expr->kind == N_NUM) arr->arr_len = (int)len_expr->ival;
    return arr;
}

// ---- symbols ----
typedef struct Sym { atom_t name; int pos, flags, offset; bool is_global, is_constant; Type *type;
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

static size_t ntypedefs;
static Node *typedefs[256];
static void add_typedef(Node *n) {
    if (ntypedefs >= 256) die("too many typedefs");
    typedefs[ntypedefs++] = n;
}
static Type *find_typedef(atom_t name) {
    for (size_t i = 0; i < ntypedefs; i++) {
        if (typedefs[i]->name == name) return typedefs[i]->type;
    }
    return NULL;
}

// =====================================================================
// 5. PARSER
// =====================================================================
static size_t P;                       // token cursor
static Token *cur(void) { return &toks[P]; }
static int at(int k)    { return toks[P].kind == k; }
static int eat(int k)   { if (toks[P].kind == k) { P++; return 1; } return 0; }
static void expect(int k) { if (!eat(k)) { error(NULL, "expected '%s', got '%s'", token_name[k], token_str(cur())); } }

static atom_t getid(void) {
    if (toks[P].kind == T_ID) return toks[P++].text;
    expect(T_ID); return 0;
}

static bool is_type_start(Token *t) {
    int k = t->kind;
    if (IS_TYPE(k)) return true;
    if (k == T_ID && find_typedef(t->text)) return true;
    return false;
}

// ---- struct/union tag table ----
// XXX: these should be global/local based
typedef struct Tag { atom_t name; int kind; Type *type; struct Tag *next; } Tag;
static Tag *tags;
static Type *tag_get(atom_t name, int kind, int pos) { // find or forward-declare
    if (name) {
        for (Tag *e = tags; e; e = e->next) {
            if (e->name == name && e->kind == kind) return e->type;
        }
    }
    Type *t = allocz(1, sizeof(Type)); t->kind = (unsigned char)kind;
    t->pos = pos; t->tag = name;
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
static Type *parse_type_base_only(int *flags);

static Node *this_switch;
static Node *this_loop;
static struct Label *add_label(Node *n);
static struct Label *find_label(atom_t name);
static Node *parse_const_expr(void);
static Node *parse_init(void);
static bool eval_expr(Node *n, long *vp);

static Type *parse_array(Type *t) {
    Node *len_expr = at(T_RBRK) ? NULL : parse_const_expr();
    expect(T_RBRK);
    if (eat(T_LBRK)) t = parse_array(t);
    return array_of(t, len_expr);
}

// struct/union specifier:  (struct|union) [tag] [ { members } ]
static Type *parse_struct(int kind) {
    int pos = cur()->pos; P++;
    atom_t tag = at(T_ID) ? getid() : 0;
    Type *st = tag_get(tag, kind, pos);
    if (eat(T_LBRACE)) {                           // definition
        if (st->members) error(NULL, "%s already has a definition", atom_str((atom_t)kind));
        Member *head = NULL, *tail = NULL;
        int off = 0, maxsz = 0;
        st->align = 1;
        while (!at(T_RBRACE) && !at(T_EOF)) {
            int flags;
            Type *mbase = parse_type_base_only(&flags);
            for (;;) {
                Type *mt = mbase;
                while (eat(T_STAR)) mt = ptr_to(mt);
                atom_t mnm = getid();
                if (eat(T_LBRK)) mt = parse_array(mt);
                int msz = ty_size(mt);
                int align = ty_align(mt);
                if (align > st->align) st->align = (unsigned char)align;
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
    int pos = cur()->pos; P++;
    atom_t tag = at(T_ID) ? getid() : 0;
    Type *st = tag_get(tag, TY_ENUM, pos);
    if (eat(T_LBRACE)) {                           // definition
        Member **tailp = &st->members;
        long off = 0;
        int align = 4;
        while (!at(T_RBRACE) && !at(T_EOF)) {
            int pos = toks[P].pos;
            atom_t name = getid();
            Member *m = allocz(1, sizeof(Member));
            m->name = name;
            if (eat(T_ASSIGN)) {
                m->init = parse_const_expr();
                if (!eval_expr(m->init, &off))
                    error(m->init, "expression is not constant");
            }
            if (align < 2 && (off < -128 || off > -127)) align = 2;
            if (align < 4 && (off < -32768 || off > -32767)) align = 4;
            if (off < -2147483648 || off > -2147483647) align = 8;
            m->offset = (int)off;   // m->offset should be a long
            *tailp = m;
            tailp = &m->next;
            Sym *s = add_global(name, pos, st);
            s->is_global = 1;
            s->is_constant = 1;
            s->ival = off;
            s->init = m->init;
            off++;
            if (!eat(T_COMMA)) break;
        }
        st->align = align;
        expect(T_RBRACE);
    }
    return st;
}

// parse base type + pointer stars; returns Type*
static Type *parse_type(int *flags) {
    Type *base = parse_type_base_only(flags);
    while (eat(T_STAR)) base = ptr_to(base);
    return base;
}

static Node *parse_string(void) {
    if (!at(T_STR)) expect(T_STR);
    atom_t str = cur()->text;
    Node *n = new_node(N_STR); n->str = str; P++;
    n->type = ptr_to(ty_char());
    if (at(T_STR)) {    // concatenate juxtaposed strings
        char buf[8192]; size_t len = 0;
        for (;;) {
            len += pmemcpy(buf + len, sizeof(buf) - len,
                           atom_str(str), atom_len(str));
            if (!at(T_STR)) break;
            str = cur()->text; P++;
        }
        if (len == sizeof buf) error(n, "string too long");
        n->str = new_atom_len(buf, len);
    }
    return n;
}

static Node *parse_primary(void) {
    if (eat(T_LP)) {
        // cast?  ( type ) unary
        if (is_type_start(cur())) {
            int flags;
            Type *t = parse_type(&flags);
            expect(T_RP);
            Node *n = new_node(N_CAST); n->type = t; n->lhs = parse_assign();
            return n;
        }
        Node *n = parse_expr(); expect(T_RP); return n;
    }
    if (at(T_NUM))  { Node *n = new_num_node(cur()->ival, cur()->is_unsigned ?
                                             (cur()->is_long ? ty_ulong() : ty_uint()) :
                                             (cur()->is_long ? ty_long() : ty_int()));
                      P++; return n; }
    if (at(T_CHAR)) { Node *n = new_num_node(cur()->ival, ty_int());  P++; return n; }
    if (at(T_STR))  { return parse_string(); }
    if (at(T_ID)) {
        atom_t nm = getid();
        if (at(T_LP)) {                       // function call
            Node *n = new_node(N_CALL); P++; n->name = nm;
            Node **ap = &n->rhs;
            while (!at(T_RP)) {
                if (n->nargs >= MAX_ARGS) error(NULL, "too many function arguments");
                Node *arg = parse_assign();
                *ap = arg; ap = &arg->next; n->nargs++;
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
        if (at(T_LP) && is_type_start(&toks[P+1])) {
            P++; n->type = parse_type(NULL); expect(T_RP);
        } else {
            n->lhs = parse_unary();
        }
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
    case T_ASSIGN:  case T_PLUSEQ:    case T_MINUSEQ:  case T_STAREQ:
    case T_SLASHEQ: case T_PERCENTEQ: case T_OREQ:     case T_ANDEQ:
    case T_XOREQ:   case T_SHLEQ:     case T_SHREQ:
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
    int flags;
    Type *base = parse_type_base_only(&flags);   // fwd-declared below
    Node *n = new_node(N_DECLIST);
    n->ival = flags;
    switch (base->kind) {  // check for bare struct/union/enum Foo { ... };
    case TY_STRUCT: case TY_UNION: case TY_ENUM: if (eat(T_SEMI)) return n;
    }
    Node **tailp = &n->rhs;
    for (;;) {
        Type *t = base;
        while (eat(T_STAR)) t = ptr_to(t);
        atom_t nm = getid();
        if (eat(T_LBRK)) t = parse_array(t);
        Node *d = new_node(N_DECL); d->name = nm; d->type = t; n->ival = flags;
        if (eat(T_ASSIGN)) {
            Node *init = d->init = parse_init();
            if (t->kind == TY_ARRAY && t->arr_len < 0 && init->kind == N_BLOCK) {
                t->arr_len = node_length(init->rhs);
            }
        }
        *tailp = d; tailp = &d->next;
        if (!eat(T_COMMA)) break;
    }
    expect(T_SEMI);
    return n;
}
// parse just the base type (no trailing stars) — stars belong to each declarator
static Type *parse_type_base_only(int *pflags) {
    Type *t = NULL; int flags = 0;
    for (;;) {
        int k;
        switch (k = cur()->kind) {
        case K_STRUCT:   if (t) goto invalid; return parse_struct(TY_STRUCT);
        case K_UNION:    if (t) goto invalid; return parse_struct(TY_UNION);
        case K_ENUM:     if (t) goto invalid; return parse_enum();
        case K_INT:
        case K_LONG:
        case K_CHAR:
        case K_SHORT:
        case K_VOID:
        case K_FLOAT:
        case K_DOUBLE:
        case K_SIGNED:
        case K_UNSIGNED:
        case K_CONST:
        case K_VOLATILE:
        case K_INLINE:
        case K_NORETURN:
        case K_AUTO:
        case K_STATIC:
        case K_REGISTER:
        case K_EXTERN:
        case K_THREAD_LOCAL:
        case K_TYPEDEF:  flags |= 1 << (k - K_INT); break;
        default:
            if (t) return t;
            if (k == T_ID) {
                Type *type = find_typedef(toks[P].text);
                if (type) { P++; return type; }
            }
            error(NULL, "expected type, got '%s'", token_str(cur())); return 0;
        }
        switch (flags & (HAS_INT | HAS_SHORT | HAS_LONG | HAS_CHAR | HAS_VOID |
                         HAS_SIGNED | HAS_UNSIGNED | HAS_FLOAT | HAS_DOUBLE)) {
        case 0:                                                   break;  // no type yet
        case HAS_CHAR:                           t = ty_char();   break;
        case HAS_SIGNED | HAS_CHAR:              t = ty_schar();  break;
        case HAS_UNSIGNED | HAS_CHAR:            t = ty_uchar();  break;
        case HAS_INT:
        case HAS_SIGNED:
        case HAS_SIGNED | HAS_INT:               t = ty_int();    break;
        case HAS_UNSIGNED:
        case HAS_UNSIGNED | HAS_INT:             t = ty_uint();   break;
        case HAS_LONG:
        case HAS_LONG | HAS_INT:
        case HAS_SIGNED | HAS_LONG:
        case HAS_SIGNED | HAS_LONG | HAS_INT:    t = ty_long();   break;
        case HAS_UNSIGNED | HAS_LONG:
        case HAS_UNSIGNED | HAS_LONG | HAS_INT:  t = ty_ulong();  break;
        case HAS_SHORT:
        case HAS_SHORT | HAS_INT:
        case HAS_SIGNED | HAS_SHORT:
        case HAS_SIGNED | HAS_SHORT | HAS_INT:   t = ty_short();  break;
        case HAS_UNSIGNED | HAS_SHORT:
        case HAS_UNSIGNED | HAS_SHORT | HAS_INT: t = ty_ushort(); break;
        case HAS_VOID: t = ty_void(); break;
        case HAS_FLOAT:
        case HAS_SHORT | HAS_FLOAT:
        case HAS_DOUBLE:
        case HAS_LONG | HAS_DOUBLE: error(NULL, "floating point types not supported"); return 0;
        default:
        invalid: error(NULL, "invalid type combination"); return 0;
        }
        switch (flags & (HAS_STATIC | HAS_REGISTER | HAS_EXTERN | HAS_TYPEDEF)) {
        case 0: case HAS_STATIC: case HAS_REGISTER: case HAS_EXTERN: case HAS_TYPEDEF: break;
        default: error(NULL, "invalid storage class combination at '%s'", atom_str(k)); return 0;
        }
        P++; if (pflags) *pflags = flags;
    }
}

static Node *parse_init(void) {
    if (at(T_LBRACE)) {
        Node *n = new_node(N_BLOCK); P++;
        Node **tailp = &n->rhs;
        while (!at(T_RBRACE) && !at(T_EOF)) {
            Node *e = parse_init();
            *tailp = e; tailp = &e->next;
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
    Node **tailp = &n->rhs;
    while (!at(T_RBRACE) && !at(T_EOF)) {
        Node *e = parse_stmt();
        *tailp = e; tailp = &e->next;
    }
    expect(T_RBRACE);
    return n;
}

static Node *parse_stmt(void) {
    Node *n;
    if (at(T_LBRACE)) return parse_block();
    if (at(T_SEMI))  { n = new_node(N_EMPTY); P++; return n; }
    if (at(K_IF)) {
        n = new_node(N_IF); P++;
        expect(T_LP); n->cond = parse_expr(); expect(T_RP);
        n->lhs = parse_stmt();
        if (eat(K_ELSE)) n->rhs = parse_stmt();
        return n;
    }
    if (at(K_WHILE)) {
        n = new_node(N_WHILE); P++;
        expect(T_LP); n->cond = parse_expr(); expect(T_RP);
        n->lhs = parse_loop_body(n);
        return n;
    }
    if (at(K_SWITCH)) {
        n = new_node(N_SWITCH); P++;
        expect(T_LP); n->cond = parse_expr(); expect(T_RP);
        Node *save_this_switch = this_switch;
        this_switch = n;
        n->rhs = parse_block();
        this_switch = save_this_switch;
        return n;
    }
    if (at(K_CASE)) {
        if (!this_switch) error(NULL, "'case' outside a 'switch' statement");
        n = new_node(N_CASE); P++; n->cond = parse_const_expr();
        //if (eat(T_ELLIPSIS)) n->rhs = parse_const_expr();
        goto link_case;
    }
    if (at(K_DEFAULT))  {
        if (!this_switch) error(NULL, "'default' outside a 'switch' statement");
        else {
            if (this_switch->ival & HAS_DEFAULT) {
                error(NULL, "duplicate 'default' in 'switch' statement");
            }
            this_switch->ival |= HAS_DEFAULT;
        }
        n = new_node(N_DEFAULT); P++;
        n->name = K_DEFAULT;
    link_case:
        if (this_switch) {
            // append node to case list (quadratic but small n)
            Node **np = &this_switch->lhs;
            while (*np) { np = &(*np)->lhs; } *np = n;
        }
        expect(T_COLON);
        return n;
    }
    if (at(K_GOTO)) {
        n = new_node(N_GOTO); P++;
        n->name = getid();
        expect(T_SEMI);
        return n;
    }
    if (at(K_FOR)) {
        n = new_node(N_FOR); P++;
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
        n = new_node(N_DOWHILE); P++;
        n->lhs = parse_loop_body(n);
        expect(K_WHILE); expect(T_LP); n->cond = parse_expr(); expect(T_RP); expect(T_SEMI);
        return n;
    }
    if (at(K_BREAK)) {
        if (this_switch) { this_switch->ival |= HAS_BREAK; }
        else if (this_loop) { this_loop->ival |= HAS_BREAK; }
        else { error(NULL, "'break' outside a loop or 'switch' statement"); }
        n = new_node(N_BREAK); P++; expect(T_SEMI); return n;
    }
    if (at(K_CONTINUE)) {
        if (this_loop) { this_loop->ival |= HAS_CONTINUE; }
        else error(NULL, "'continue' outside a loop statement");
        n = new_node(N_CONTINUE); P++; expect(T_SEMI); return n;
    }
    if (at(K_RETURN)) {
        n = new_node(N_RETURN); P++;
        if (!at(T_SEMI)) n->lhs = parse_expr();
        expect(T_SEMI);
        return n;
    }
    if (at(K_ASM) || at(K__ASM__)) {
        n = new_node(N_ASM); P++;
        expect(T_LP);
        n->str = parse_string()->str;
        expect(T_RP); expect(T_SEMI);
        return n;
    }
    if (at(T_ID) && toks[P+1].kind == T_COLON) {
        n = new_node(N_LABEL);
        n->name = getid();
        if (find_label(n->name)) warning(n->pos, "duplicate label '%s'", atom_str(n->name));
        add_label(n);
        expect(T_COLON);
        return n;
    }
    if (is_type_start(cur())) return parse_decl_stmt();
    n = new_node(N_EXPR); n->lhs = parse_expr(); expect(T_SEMI);
    return n;
}

// 5.1 Constant expression evaluator

static Sym *lookup(atom_t name, Node *n);
static Type *static_typeof(Node *n);

static bool eval_expr(Node *n, long *vp) {
    long v1, v2;
    switch (n->kind) {
    case N_EXPR: return eval_expr(n->lhs, vp);
    case N_NUM:  *vp = n->ival; return true;
    case N_VAR: {
        Sym *s = lookup(n->name, n);
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
        case T_SLASH:   *vp = v2 ? v1 / v2 : 0;  return true;   // XXX: check overflow
        case T_PERCENT: *vp = v2 ? v1 % v2 : 0;  return true;   // XXX: check overflow
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
typedef struct Func {
    atom_t name; int pos, flags, endpos; unsigned char nparams; bool is_variadic, used;
    Node *param; Node *body; Type *rtype; struct Label *labels; struct Func *next; } Func;
static Func *funcs, **funcs_tail;
static Func *this_fn;

typedef struct Label Label;
struct Label { atom_t name; bool used; Node *n; struct Label *next; };
static Label *add_label(Node *n) {
    Label *lab = allocz(1, sizeof(Label));
    lab->name = n->name;
    lab->n = n;
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
    // XXX: should merge with parse_decl_stmt
    if (at(K_TYPEDEF)) {
        // XXX: should be handled like a decl
        Node *n = parse_decl_stmt();
        for (Node *e = n->rhs; e; e = e->next) add_typedef(e);
        return;
    }
    int flags;
    int pos = cur()->pos;
    Type *base = parse_type_base_only(&flags);
    if (eat(T_SEMI)) return;                           // bare  struct Foo { ... };
    Type *t = base;
    while (eat(T_STAR)) t = ptr_to(t);
    atom_t nm = getid();

    // XXX: parse function pointers and such
    if (eat(T_LP)) {                                   // function definition
        if (flags & HAS_TYPEDEF) error(NULL, "function typedefs not supported");
        bool has_prototype = false;
        Func *fn = find_func(nm);
        if (fn) {
            has_prototype = true;
            if (!same_type(fn->rtype, t))
                warning(cur()->pos, "return type mismatch with '%s' function prototype", atom_str(nm));
        } else {
            fn = allocz(1, sizeof(Func));
            fn->name = nm;
            fn->rtype = t;
            fn->flags = flags;
            if (!funcs_tail) funcs_tail = &funcs; *funcs_tail = fn; funcs_tail = &fn->next;
        }
        int np = 0;
        bool is_variadic = false;
        Node **pp = &fn->param;
        if (at(K_VOID) && toks[P+1].kind == T_RP) P++; // (void) -> ()
        while (!at(T_RP)) {
            if (eat(T_ELLIPSIS)) { is_variadic = true; break; }   // printf(char *fmt, ...)
            if (np >= MAX_ARGS) error(NULL, "too many function arguments");
            int flags;
            Type *pt = parse_type_base_only(&flags);
            while (eat(T_STAR)) pt = ptr_to(pt);
            Node *pv = new_node(N_DECL);
            if (at(T_ID)) pv->name = getid();   // argument name is optional
            if (eat(T_LBRK)) {
                pt = parse_array(pt);   // pseudo array function parameter
                pt->kind = TY_PTR;
                pt->align = 8;
            }
            pv->type = pt;
            if (has_prototype && *pp && !same_type((*pp)->type, pt))
                warning(cur()->pos, "type mismatch with prototype on argument %d", np + 1);
            *pp = pv; pp = &pv->next; np++;
            if (!eat(T_COMMA)) break;
        }
        expect(T_RP);
        if (has_prototype && (fn->nparams != np || fn->is_variadic != is_variadic))
            warning(cur()->pos, "argument count mismatch with prototype");
        fn->is_variadic = is_variadic;
        fn->nparams = np;
        if (!eat(T_SEMI)) { // actual function definition
            if (fn->body) { error(NULL, "function '%s' already has a body", atom_str(fn->name)); }
            fn->pos = pos;
            this_fn = fn;
            fn->body = parse_block();
            this_fn = NULL;
            fn->endpos = toks[P-1].pos;
        }
        if (verbose) printf("-> %s\n", atom_str(nm));
        return;
    }
    if (flags & (HAS_NORETURN | HAS_INLINE)) warning(pos, "inline or _Noreturn can only be applied to functions");
    // global variable(s):  type name [= ...] (, ...) ;
    for (;;) {
        int pos = toks[P].pos;
        if (eat(T_LBRK)) t = parse_array(t);
        Sym *sym = add_global(nm, pos, t);
        sym->flags = flags;
        if (eat(T_ASSIGN)) {
            Node *init = sym->init = parse_init();
            if (t->kind == TY_ARRAY && t->arr_len < 0 && init->kind == N_BLOCK) {
                t->arr_len = node_length(init->rhs);
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
    s->type = t;
    // XXX: should handle static local
    int sz = ty_size(t); if (sz < 8) sz = 8; sz = (sz + 7) & ~7;
    frame_size += sz; s->offset = frame_size;
    s->next = locals; locals = s;
    return s;
}
static void collect_locals(Node *n) {
    if (!n) return;
    switch (n->kind) {
      case N_DECLIST:
      case N_BLOCK: for (Node *e = n->rhs; e; e = e->next) collect_locals(e); break;
      case N_DECL:
        if (!sym_find(locals, n->name)) add_local(n->name, n->pos, n->type);
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
      case N_CALL: for (Node *e = n->rhs; e; e = e->next) collect_locals(e); break;
      case N_SWITCH: collect_locals(n->cond); collect_locals(n->rhs); break;
      default: break;
    }
}

static bool check_rewrite(Node *n) {
    if (!optimize) return false;
    switch (n->name) {
    case ID_PRINTF: {   // optimize printf("Hello world\n");
        if (n->nargs != 1 || !(n->flags & DISCARD)) break;
        Node *arg = n->rhs; if (arg->kind != N_STR) break;
        const char *fmt = atom_str(arg->str);
        if (!*fmt) { n->kind = N_NUM; n->ival = 0; n->type = ty_int(); return true; }
        size_t len = strlen(fmt);
        if (strchr(fmt, '%') || fmt[len-1] != '\n') break;
        n->name = ID_PUTS;
        arg->str = new_atom_len(fmt, len - 1);
        return true;
    }
    case ID_STRLEN: {   // optimize str("str");
        if (n->nargs != 1) break;
        Node *arg = n->rhs; if (arg->kind != N_STR) break;
        const char *s = atom_str(arg->str);
        size_t len = strlen(s); // do not use atom len to allow embedded nuls
        n->kind = N_NUM; n->ival = (long)len; n->type = ty_ulong();
        return true;
    }
    case ID_STRCPY: {   // optimize strcpy(dest, "str");
        if (n->nargs != 2) break;
        Node *arg2 = n->rhs->next; if (arg2->kind != N_STR) break;
        const char *s = atom_str(arg2->str);
        long len = (long)strlen(s); // do not use atom len to allow embedded nuls
        n->name = ID_MEMCPY;
        arg2->next = new_num_node(len + 1, ty_ulong());
        arg2->next->pos = arg2->pos;
        n->nargs = 3;
        return true;
    }}
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
        case N_DECLIST:
        case N_BLOCK:
            // XXX: should handle scoping
            for (Node *e = n->rhs; e; e = e->next) check_used(e); break;
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
            if (check_rewrite(n)) { check_used(n); break; }
            if (!IS_BUILTIN(n->name)) check_used_func(n->name);
            for (Node *e = n->rhs; e; e = e->next) check_used(e); break;
            break;
        case N_SWITCH: check_used(n->cond); check_used(n->rhs); break;
        case N_VAR: break; // XXX: should look up symbol and set used bit
    }
}

static bool has_flow(Node *n) {
    if (!n) return true;
    switch (n->kind) {
    case N_GOTO:
    case N_RETURN: return false;
    case N_BLOCK:  return has_flow(node_last(n->rhs));
    case N_IF:     return has_flow(n->lhs) || has_flow(n->rhs);
    case N_FOR:    return n->cond || (n->ival & HAS_BREAK);
    case N_SWITCH: if ((n->ival & (HAS_BREAK | HAS_DEFAULT)) != HAS_DEFAULT) return true;
                   return has_flow(n->rhs);
    //case N_WHILE: case N_DOWHILE: // XXX: check constant loop condition and break
    //case N_CALL:    // XXX: check for _Noreturn attribute
    default:       return true;
    }
}

// =====================================================================
// 7. ASSEMBLY OUTPUT
// =====================================================================
static FILE *fout;
static bool out_comments;
static char next_comment[48];
static int next_label;
static void emit_indent(int indent) { while (indent > 0) { fputc('\t', fout); indent -= 4; } }
static void emit(const char *fmt, ...) attr_printf(1,2);
static void emit(const char *fmt, ...) {
    int indent = 28;
    if (next_label > 0) indent -= fprintf(fout, ".L%d:", next_label);
    next_label = 0;
    if (*fmt != ' ') {  // no empty format strings to avoid gcc warning
        emit_indent(indent - 20);
        va_list a; va_start(a, fmt); indent = 20 - vfprintf(fout, fmt, a); va_end(a);
    }
    if (*next_comment) {
        emit_indent(indent);
        fprintf(fout, "\t# %s", next_comment); *next_comment = '\0';
    }
    fputc('\n', fout);
}
static void emit_label(int lab) { if (!lab) return; if (next_label) emit(" "); next_label = lab; }
static void emit_comment(const char *comment) {
    if (out_comments) {
        if (*next_comment) emit(" ");
        if (comment) pstrcpy(next_comment, sizeof(next_comment), comment);
    }
}

static const char *cur_section;
static void emit_section(const char *name, int align) {
    if (cur_section != name) {
        cur_section = name;
        emit(" ");
        emit(".section %s", name);
        if (align) emit(".align %d", align);
    }
}
static void emit_entry(atom_t name, bool globl, bool skip) {
    if (!name) return;
    emit_label(-1); // flush label and comments
    if (skip) fputc('\n', fout);
    if (globl) emit(".globl %s", atom_str(name));
    fprintf(fout, "%s:\n", atom_str(name));
}

static void emit_jmp(Node *n, const char *instr, int lab) {
    if (!lab) error(n, "invalid jump");
    emit("%s .L%d", instr, lab);
}

// =====================================================================
// 7.1 CODE GENERATION
// =====================================================================
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
const char *reg64[] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
const char *reg32[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d" };
const char *reg16[] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w" };
const char *reg8[] =  { "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b" };

static int label_id = 1;
static int ARGREG[6] = { RDI, RSI, RDX, RCX, R8, R9 };

// current function's varargs state (set in gen_func, read by __builtin_va_* codegen)
static int cur_va_off, cur_named;

static Type *gen_expr(Node *n);
static Type *gen_expr2(Node *n, bool save_rax);
static void  gen_stmt(Node *n);

static Sym *lookup(atom_t name, Node *n) {
    Sym *s = sym_find(locals, name);
    if (!s) s = sym_find(globals, name);
    if (!s && n) error(n, "undeclared identifier '%s'", atom_str(name));
    return s;
}
static void promote_rax(Type *t) {
    switch (ty_size(t)) {
    case 1: emit(t->is_unsigned ? "movzx rax, al" : "movsx rax, al"); return;
    case 2: emit(t->is_unsigned ? "movzx rax, ax" : "movsx rax, ax"); return;
    case 4: emit(t->is_unsigned ? "mov eax, eax" : "movsx rax, eax"); return;
    }
}
static void load_ind(Type *t, int r2, int r1) {   // r2 = [r1] (size and type aware)
    const char *dest = reg64[r2], *src = reg64[r1];
    const char *instr = t->is_unsigned ? "movzx" : "movsx";
    switch (ty_size(t)) {
    case 1: emit("%s %s, byte ptr [%s]", instr, dest, src); return;
    case 2: emit("%s %s, word ptr [%s]", instr, dest, src); return;
    case 4: if (t->is_unsigned) emit("mov %s, [%s]", reg32[r2], src);
            else emit("movsx %s, dword ptr [%s]", dest, src); return;
    default: emit("mov %s, [%s]", dest, src); return;
    }
}
static void store_rcx(Type *t, int r) {    // [rcx] = reg (size and type aware)
    switch (ty_size(t)) {
    case 1: emit("mov [rcx], %s", reg8[r]); return;
    case 2: emit("mov [rcx], %s", reg16[r]); return;
    case 4: emit("mov [rcx], %s", reg32[r]); return;
    default: emit("mov [rcx], %s", reg64[r]); return;
    }
}

// leave the ADDRESS of an lvalue node in rax, return the lvalue type
static Type *gen_addr(Node *n) {
    if (n->kind == N_VAR) {
        Sym *s = lookup(n->name, n);
        if (s->is_global) emit("lea rax, [rip + %s]", atom_str(n->name));
        else              { emit_comment(atom_str(n->name)); emit("lea rax, [rbp - %d]", s->offset); }
        return s->type;
    }
    if (n->kind == N_DEREF) {                      // &*p  ==  p
        Type *t = gen_expr(n->lhs);
        //return t; //is_ptrish(t) ? t->ptr : ty_long(); @@@
        return is_ptrish(t) ? t->ptr : ty_long();
    }
    if (n->kind == N_MEMBER) {                     // &(s.field)
        Type *st = gen_addr(n->lhs);               // rax = &struct
        Member *m = find_member(st, n->name);
        if (!m) error(n, "no such struct member: %s", atom_str(n->name));
        emit_comment(atom_str(n->name));
        if (m->offset) { emit("add rax, %d", m->offset); }
        return m->type;
    }
    error(n, "not an lvalue"); return 0;
}

// leave the ADDRESS of an lvalue node in rcx, return the lvalue type
static Type *gen_addr2(Node *n, bool save_rax) {
    if (n->kind == N_VAR) {
        Sym *s = lookup(n->name, n);
        if (s->is_global) emit("lea rcx, [rip + %s]", atom_str(n->name));
        else              { emit_comment(atom_str(n->name)); emit("lea rcx, [rbp - %d]", s->offset); }
        return s->type;
    }
    if (n->kind == N_DEREF) {                      // &*p  ==  p
        Type *t = gen_expr2(n->lhs, save_rax);
        //return t; //is_ptrish(t) ? t->ptr : ty_long(); @@@
        return is_ptrish(t) ? t->ptr : ty_long();
    }
    if (n->kind == N_MEMBER) {                     // &(s.field)
        Type *st = gen_addr2(n->lhs, save_rax);    // rax = &struct
        Member *m = find_member(st, n->name);
        if (!m) error(n, "no such struct member: %s", atom_str(n->name));
        emit_comment(atom_str(n->name));
        if (m->offset) { emit("add rcx, %d", m->offset); }
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
        case N_VAR:  { Sym *s = lookup(n->name, NULL); return s ? s->type : ty_long(); }
        case N_MEMBER: { Type *st = static_typeof(n->lhs); Member *m = find_member(st, n->name);
                         return m ? m->type : ty_long(); }
        case N_DEREF: { Type *t = static_typeof(n->lhs); return is_ptrish(t) ? t->ptr : ty_long(); } // should report error
        case N_ADDR:   return ptr_to(static_typeof(n->lhs));
        default:       return ty_long();
    }
}

static size_t gen_quoted_string(char *buf, size_t size, const char *str, size_t slen, char sep) {
    size_t j = 0;
    if (sep && size > 1) buf[j++] = sep;
    for (size_t i = 0; i < slen; i++) {
        char ch = str[i];
        switch (ch) {
        case '\n': ch = 'n'; goto escape;  case '\t': ch = 't'; goto escape;
        case '\r': ch = 'r'; goto escape;  case '\b': ch = 'b'; goto escape;
        case '\f': ch = 'f'; goto escape;  case '\v': ch = 'v'; goto escape;
        case '"': case '\'': if (sep && ch != sep) goto normal;
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
            if (ch & 0700 || has_digit) buf[j++] = '0' + ((ch >> 6) & 7);
            if (ch & 0770 || has_digit) buf[j++] = '0' + ((ch >> 3) & 7);
            buf[j++] = '0' + (ch & 7);
            continue;
        }
        break;
    }
    if (sep && j + 1 < size) buf[j++] = sep;
    if (j < size) buf[j] = '\0';
    return j;
}

static void gen_string_def(atom_t id) {
    char buf[8192];
    gen_quoted_string(buf, sizeof(buf), atom_str(id), atom_len(id), '"');
    fprintf(fout, ".LC%d:\t.string %s\n", id, buf);
}

static void load_string(Node *n, int r) {
    atom_t id = n->str;
    if (out_comments) {
        char buf[37];
        size_t len = gen_quoted_string(buf, sizeof(buf), atom_str(id), atom_len(id), '"');
        if (len > 32) pstrcpy(buf + 32, sizeof(buf) - 32, "...\"");
        emit_comment(buf);
    }
    emit("lea %s, [rip + .LC%d]", reg64[r], id);
    atom_flags(id) |= ATOM_USED;
}

static void gen_mul_size(int r, int size) {
    if (size > 1) {
        if ((size & (size - 1)) == 0) { // power of 2
            emit("shl %s, %d", reg64[r], __builtin_ctz(size));
        } else {
            emit("imul %s, %d", reg64[r], size);
        }
    }
}

static Type *gen_bin(Node *n, int op, Type *lt, Type *rt) {
    // rax = left, rcx = right
    switch (op) {
    case T_PLUS:
        if (is_ptrish(lt)) {
            if (is_ptrish(rt)) error(n, "cannot add pointers");
            gen_mul_size(RCX, elem_size(lt));
            emit("add rax, rcx");
            return lt;
        }
        if (is_ptrish(rt)) {
            gen_mul_size(RAX, elem_size(rt));
            emit("add rax, rcx");
            return rt;
        }
        emit("add rax, rcx");
        break;
    case T_MINUS:
        if (is_ptrish(lt)) {
            int s = elem_size(lt);
            if (is_ptrish(rt)) {
                emit("sub rax, rcx");
                if (s > 1) {
                    if ((s & (s - 1)) == 0) { // power of 2
                        emit("sar rax, %d", __builtin_ctz(s));
                    } else {
                        emit("mov rcx, %d", s);
                        emit("cqo"); emit("idiv rcx");
                    }
                }
                break;
            }
            gen_mul_size(RCX, s);
            emit("sub rax, rcx");
            return lt;
        }
        emit("sub rax, rcx");
        break;
    case T_STAR:    emit("imul rax, rcx"); break;
    case T_SLASH:
        if (lt->is_unsigned | rt->is_unsigned) {
            emit("xor rdx,rdx"); emit("div rcx"); break;
        } else {
            emit("cqo"); emit("idiv rcx"); break;
        }
    case T_PERCENT:
        if (lt->is_unsigned | rt->is_unsigned) {
            emit("xor rdx,rdx"); emit("div rcx");
        } else {
            emit("cqo"); emit("idiv rcx");
        }
        emit("mov rax, rdx"); break;
    case T_AMP:     emit("and rax, rcx"); break;
    case T_BITOR:   emit("or rax, rcx");  break;
    case T_BITXOR:  emit("xor rax, rcx"); break;
    case T_SHL:     emit("shl rax, cl");  break;   // count in cl (low byte of rcx)
    case T_SHR:     if (lt->is_unsigned) emit("shr rax, cl"); else emit("sar rax, cl"); break;
    case T_LT: case T_GT: case T_LE: case T_GE: case T_EQ: case T_NE: {
        emit("cmp rax, rcx");
        const char *cc = op == T_LT ? "setl" : op == T_GT ? "setg" : op == T_LE ? "setle" :
            op == T_GE ? "setge" : op == T_EQ ? "sete" : "setne";
        if (lt->is_unsigned | rt->is_unsigned) {
            cc = op == T_LT ? "setb" : op == T_GT ? "seta" : op == T_LE ? "setbe" :
                op == T_GE ? "setae" : op == T_EQ ? "sete" : "setne";
        }
        emit("%s al", cc); emit("movzx rax, al");
        break;
    }
    default: error(n, "bad binary operator '%s'", token_name[op]);
    }
    return ty_long();
}

static Type *load_var(Node *n, int r) {
    const char *reg = reg64[r];
    Sym *s = lookup(n->name, n);
    if (s->is_constant) { emit_comment(atom_str(n->name)); emit("mov %s, %ld", reg, s->ival); return ty_long(); }
    Type *t = s->type;
    switch (t->kind) {
    case TY_ARRAY: case TY_STRUCT: case TY_UNION:  // arrays and structs are used by-address (decay); scalars are loaded
        if (s->is_global) emit("lea %s, [rip + %s]", reg, atom_str(n->name));
        else            { emit_comment(atom_str(n->name)); emit("lea %s, [rbp - %d]", reg, s->offset); }
        break;
    default: {
        const char *mov = t->is_unsigned ? "movzx" : "movsx";
        if (s->is_global) {
            switch (ty_size(t)) {
            case 1: emit("%s %s, byte ptr [rip + %s]", mov, reg, atom_str(n->name)); break;
            case 2: emit("%s %s, word ptr [rip + %s]", mov, reg, atom_str(n->name)); break;
            case 4: if (t->is_unsigned) emit("mov %s, [rip + %s]", reg32[r], atom_str(n->name));
                else emit("movsx %s, dword ptr [rip + %s]", reg, atom_str(n->name)); break;
            default: emit("mov %s, [rip + %s]", reg, atom_str(n->name)); break;
            }
        } else {
            emit_comment(atom_str(n->name));
            switch (ty_size(t)) {
            case 1: emit("%s %s, byte ptr [rbp - %d]", mov, reg, s->offset); break;
            case 2: emit("%s %s, word ptr [rbp - %d]", mov, reg, s->offset); break;
            case 4: if (t->is_unsigned) emit("mov %s, [rbp - %d]", reg32[r], s->offset);
                else emit("movsx %s, dword ptr [rbp - %d]", reg, s->offset); break;
            default: emit("mov %s, [rbp - %d]", reg, s->offset); break;
            }
        }
        break;
    }}
    return s->type;
}

static bool is_simple_load(Node *n) {
    switch (n->kind) {
    case N_NUM: case N_STR: case N_VAR: return true;
    }
    return false;
}
static bool all_simple_load(Node *n) {
    for (Node *arg = n->rhs; arg; arg = arg->next) if (!is_simple_load(arg)) return false;
    return true;
}

static void gen_simple_load(Node *n, int r) {
    switch (n->kind) {
    case N_NUM:  emit("mov %s, %ld", reg64[r], n->ival); return;
    case N_STR:  load_string(n, r); return;
    case N_VAR:  load_var(n, r); return;
    }
}

static Type *gen_expr(Node *n) {
    switch (n->kind) {
    case N_NUM:  emit("mov rax, %ld", n->ival); return n->type ? n->type : ty_long();
    case N_STR:  load_string(n, RAX); return n->type;
    case N_VAR:  return load_var(n, RAX);
    case N_MEMBER: {
        Type *mt = gen_addr(n);                    // rax = &member
        if (mt->kind == TY_ARRAY || mt->kind == TY_STRUCT || mt->kind == TY_UNION) return mt;   // decay
        load_ind(mt, RAX, RAX);
        return mt;
    }
    case N_SIZEOF: {
        Type *t = n->type ? n->type : static_typeof(n->lhs);
        emit("mov rax, %d", ty_size(t));
        return ty_ulong();
    }
    case N_CAST: gen_expr(n->lhs); return n->type;
    case N_DEREF: {
        Type *t = gen_expr(n->lhs);
        Type *pt = is_ptrish(t) ? t->ptr : ty_long();
        load_ind(pt, RAX, RAX);
        return pt;
    }
    case N_ADDR: {
        Type *t = gen_addr(n->lhs);
        return ptr_to(t);
    }
    case N_ASSIGN: {
        Type *lt;
        if (n->op == T_ASSIGN) {
            gen_expr(n->rhs);
            lt = gen_addr2(n->lhs, true);
            store_rcx(lt, RAX);
        } else {
            lt = gen_addr(n->lhs); emit("push rax");
            load_ind(lt, RAX, RAX);
            Type *rt = gen_expr2(n->rhs, true);
            gen_bin(n, n->op - T_PLUSEQ + T_PLUS, lt, rt);
            emit("pop rcx");
            store_rcx(lt, RAX);
        }
        if (!(n->flags & DISCARD)) promote_rax(lt);
        return lt;
    }
    case N_POST:                                   // x++ / x--  (returns old value)
    case N_PRE: {                                  // ++x / --x  (returns new value)
        // XXX: should optimize if address is simple
        Type *lt = gen_addr2(n->lhs, false);
        int step = is_ptrish(lt) ? elem_size(lt) : 1;
        const char *add = n->op == T_INC ? "add" : "sub";
        const char *inc = n->op == T_INC ? "inc" : "dec";
        if (n->flags & DISCARD) {
            const char *prefix = "";
            switch (ty_size(lt)) {
            case 1: prefix = "byte ptr "; break;
            case 2: prefix = "word ptr "; break;
            case 4: prefix = "dword ptr "; break;
            case 8: prefix = "qword ptr "; break;
            }
            if (step == 1) emit("%s %s[rcx]", inc, prefix);
            else emit("%s %s[rcx], %d", add, prefix, step);
        } else
        if (n->kind == N_PRE) {
            load_ind(lt, RAX, RCX);                // rax = old
            if (step == 1) emit("%s rax", inc);
            else emit("%s rax, %d", add, step);
            store_rcx(lt, RAX);                    // *&x = new (rax)
        } else {
            load_ind(lt, RAX, RCX);                // rax = old
            if (n->op == T_INC) emit("lea rdx, [rax + %d]", step);
            else emit("lea rdx, [rax - %d]", step);
            store_rcx(lt, RDX);
        }
        return lt;                                 // rax = new value
    }
    case N_TERNARY: {
        int els = label_id++, end = label_id++;
        gen_expr(n->cond); emit("test rax, rax"); emit_jmp(n, "jz", els);
        gen_expr(n->lhs); emit_jmp(n, "jmp", end);
        emit_label(els); gen_expr(n->rhs);
        emit_label(end);
        return ty_long();
    }
    case N_UNARY:
        gen_expr(n->lhs);
        if      (n->op == T_MINUS)  emit("neg rax");
        else if (n->op == T_BITNOT) emit("not rax");
        else { emit("test rax, rax"); emit("sete al"); emit("movzx rax, al"); }
        return ty_long();
    case N_LOGAND: {
        int f = label_id++;
        gen_expr(n->lhs); emit("test rax, rax"); emit_jmp(n, "jz", f);
        gen_expr(n->rhs); emit("test rax, rax");
        emit("mov rdx, 1"); emit("cmovnz rax, rdx"); emit_label(f);
        return ty_long();
    }
    case N_LOGOR: {
        int tl = label_id++;
        gen_expr(n->lhs); emit("test rax, rax"); emit_jmp(n, "jnz", tl);
        gen_expr(n->rhs); emit("test rax, rax");
        emit_label(tl); emit("mov rdx, 1"); emit("cmovnz rax, rdx");
        return ty_long();
    }
    case N_CALL: {
        // ---- variadic built-ins (handled inline, not real calls) ----
        switch (n->name) {
        case ID__BUILTIN_VA_START: {
            emit("lea rax, [rbp - %d]", cur_va_off - cur_named * 8);  // &save[named]
            gen_addr2(n->rhs, true);           // rcx = &ap
            emit("mov [rcx], rax");            // ap = first vararg slot
            return ty_long();
        }
        case ID__BUILTIN_VA_ARG: {
            gen_addr2(n->rhs, false);          // rcx = &ap
            emit("mov rax, [rcx]");            // rax = ap
            emit("mov rax, [rax]");            // rax = *ap  (the argument value)
            emit("add qword ptr [rcx], 8");    // ap += 8
            return ty_long();
        }
        case ID__BUILTIN_VA_END: return ty_void();   // no-op
        case ID__BUILTIN_ROTATE_LEFT:
            gen_expr(n->rhs);
            gen_expr2(n->rhs->next, true); emit("rol rax, cl");
            return ty_long();
        case ID__BUILTIN_ROTATE_RIGHT:
            gen_expr(n->rhs);
            gen_expr2(n->rhs->next, true); emit("ror rax, cl");
            return ty_long();
        case ID__BUILTIN_BSWAP16:
            gen_expr(n->rhs); emit("rol ax, 16");
            return ty_long();
        case ID__BUILTIN_BSWAP32:
            gen_expr(n->rhs); emit("bswap eax");
            return ty_long();
        case ID__BUILTIN_BSWAP64:
            gen_expr(n->rhs); emit("bswap rax");
            return ty_long();
        case ID__BUILTIN_CLZ:
            gen_expr(n->rhs); emit("bsr rax, rax"); emit("xor rax, 63");
            return ty_long();
        case ID__BUILTIN_CTZ:
            gen_expr(n->rhs); emit("rep bsf rax, rax");
            return ty_long();
        case ID__SYSCALL:
            if (!n->nargs) error(n, "missing syscall number argument");
            if (all_simple_load(n)) {
                int i = 0;
                for (Node *arg = n->rhs->next; arg; arg = arg->next) gen_simple_load(arg, ARGREG[i++]);
                gen_simple_load(n->rhs, RAX);
            } else {
                for (Node *arg = n->rhs->next; arg; arg = arg->next) { gen_expr(arg); emit("push rax"); }
                gen_expr(n->rhs);
                for (int i = n->nargs - 1; i-- > 0;) emit("pop %s", reg64[ARGREG[i]]);
            }
            emit("syscall");
            int end1 = label_id++;
            emit("test rax, rax");
            emit_jmp(n, "jge", end1);
            emit("neg rax");
            emit("mov [rip + errno], rax");
            emit_label(end1);
            return ty_long();
        case ID_ABS:
        case ID_LABS:
            if (n->nargs != 1) error(n, "missing syscall number argument");
            gen_expr(n->rhs);
            emit("mov rcx, rax");
            emit("neg rax");
            emit("cmovs rax, rcx");
            return n->rhs->type;
        case ID__RDTSC:
            emit("rdtsc"); emit("shl rdx,32"); emit("or rax, rdx");
            return ty_long();
        case ID__RDTSCP:
            emit("rdtscp"); emit("shl rdx,32"); emit("or rax, rdx");
            return ty_long();
        }
        // XXX: should look up symbol instead of global function to handle function pointers
        Func *fn = find_func(n->name);
        if (fn && fn->nparams != n->nargs && (!fn->is_variadic || fn->nparams > n->nargs)) {
            warning(n->pos, "argument count mismatch '%s' expects %d, got %d",
                    atom_str(n->name), fn->nparams, n->nargs);
        }
        // XXX: should check and convert arguments according to prototype
        // XXX: here we could support default argument values
        if (all_simple_load(n)) {
            int i = 0;
            for (Node *arg = n->rhs; arg; arg = arg->next) gen_simple_load(arg, ARGREG[i++]);
        } else {
            for (Node *arg = n->rhs; arg; arg = arg->next) { gen_expr(arg); emit("push rax"); }
            for (int i = n->nargs; i-- > 0;) emit("pop %s", reg64[ARGREG[i]]);
        }
        if (!fn || fn->is_variadic) emit("xor eax, eax"); // variadic-safe; harmless otherwise
        emit("call %s", atom_str(n->name));
        if (fn) return fn->rtype;
        warning(n->pos, "function not found '%s'", atom_str(n->name));
        return ty_long();
    }
    case N_BIN: {
        if (n->op == T_COMMA) {
            n->lhs->flags |= DISCARD; gen_expr(n->lhs);
            n->rhs->flags |= n->flags & DISCARD; return gen_expr(n->rhs);
        }
        Type *lt = gen_expr(n->lhs);
        Type *rt = gen_expr2(n->rhs, true);
        return gen_bin(n, n->op, lt, rt);
    }
    default: error(n, "cannot generate expression"); return 0;
    }
}

static Type *gen_expr2(Node *n, bool save_rax) {
    // evaluate an expression into rcx
    switch (n->kind) {
    case N_NUM:  emit("mov rcx, %ld", n->ival); return n->type ? n->type : ty_long();
    case N_STR:  load_string(n, RCX); return n->type;
    case N_VAR:  return load_var(n, RCX);
    case N_MEMBER: {
        Type *mt = gen_addr2(n, save_rax);          // rcx = &member
        if (mt->kind == TY_ARRAY || mt->kind == TY_STRUCT || mt->kind == TY_UNION) return mt;   // decay
        load_ind(mt, RCX, RCX);
        return mt;
    }
    case N_SIZEOF: {
        Type *t = n->type ? n->type : static_typeof(n->lhs);
        emit("mov rcx, %d", ty_size(t));
        return ty_ulong();
    }
    case N_CAST: gen_expr2(n->lhs, save_rax); return n->type;
    case N_DEREF: {
        Type *t = gen_expr2(n->lhs, save_rax);
        Type *pt = is_ptrish(t) ? t->ptr : ty_long();
        load_ind(pt, RCX, RCX);
        return pt;
    }
    case N_ADDR: {
        Type *t = gen_addr2(n->lhs, save_rax);
        return ptr_to(t);
    }
    case N_UNARY:
        gen_expr2(n->lhs, save_rax);
        if      (n->op == T_MINUS)  emit("neg rcx");
        else if (n->op == T_BITNOT) emit("not rcx");
        else { emit("test rcx, rcx"); emit("sete cl"); emit("movzx rcx, cl"); }
        return ty_long();
    case N_BIN:
        if (n->op == T_COMMA) {
            n->lhs->flags |= DISCARD; gen_expr(n->lhs);
            n->rhs->flags |= n->flags & DISCARD; return gen_expr(n->rhs);
        }
        break;
    }
    if (save_rax) emit("push rax");
    Type *t = gen_expr(n); emit("mov rcx, rax");
    if (save_rax) emit("pop rax");
    return t;
}

static void gen_asm(Node *n) {
    // split decoded asm text on newlines and ';' — emit each instruction line
    const char *p = atom_str(n->str);
    char line[512]; size_t i = 0;
    for (;; p++) {
        char c = *p;
        if (c == '\n' || c == ';' || c == 0) {
            line[i] = 0;
            // trim leading spaces
            char *q = line; q += skip_blanks(q);
            if (*q) emit("%s", q);
            i = 0;
            if (c == 0) break;
        } else if (i < 511) line[i++] = c;
    }
}

// break/continue target stack
#define MAX_LOOP 32
static int brk_lbl[MAX_LOOP], cont_lbl[MAX_LOOP], loop_sp;
static void loop_push(Node *n, int b, int c) {
    loop_sp++;
    if (loop_sp >= MAX_LOOP) error(n, "too many nested loops or switch statements");
    brk_lbl[loop_sp] = b; cont_lbl[loop_sp] = c;
}
static void loop_pop(void) { loop_sp--; }

static void gen_case_comment(Node *n) {
    if (!out_comments) return;
    char buf[20];
    size_t pos = pstrcpy(buf, sizeof(buf), "case ");
    Node *e = n->cond; long val = e->ival;
    switch (e->kind) {
    case N_VAR: pstrcpy(buf + pos, sizeof(buf) - pos, atom_str(e->name)); break;
    default:
        if (val >= ' ' && val < 127) snprintf(buf + pos, sizeof(buf) - pos, "'%c'", (int)val);
        else snprintf(buf + pos, sizeof(buf) - pos, "%ld", val);
        break;
    }
    emit_comment(buf);
}

static void gen_switch(Node *n) {
    gen_expr(n->cond);
    // Emit "if (switch_value == case_value) goto case_label" for every case.
    // XXX: should check for duplicates
    int def = 0, end = 0;
    for (Node *e = n->lhs; e; e = e->lhs) {
        long val;
        e->ival = label_id++;
        if (e->kind == N_CASE) {
            if (!eval_expr(e->cond, &val)) error(e->cond, "'case' expression is not constant");
            e->cond->ival = val;
            gen_case_comment(e);
            emit("cmp rax, %ld", val);
            emit_jmp(n, "jz", (int)e->ival);
        } else {
            def = (int)e->ival;
        }
    }
    if ((n->ival & HAS_BREAK) || !def) end = label_id++;
    emit_jmp(n, "jmp", def ? def : end);   // no match -> default or end

    // break exits the switch; continue passes through to the enclosing loop
    loop_push(n, end, cont_lbl[loop_sp]);
    gen_stmt(n->rhs);          // the body places the case labels inline
    loop_pop();
    emit_label(end);
}

static void gen_stmt(Node *n) {
    switch (n->kind) {
    case N_BLOCK: for (Node *e = n->rhs; e; e = e->next) gen_stmt(e); break;
    case N_EMPTY: break;
    case N_DECLIST:
        for (Node *e = n->rhs; e; e = e->next) {
            if (e->init) {
                Sym *s = lookup(e->name, e);
                // XXX: should optimize if value is constant
                gen_expr(e->init);
                // XXX: should since destination is local
                emit_comment(atom_str(e->name)); emit("lea rcx, [rbp - %d]", s->offset);
                store_rcx(s->type, RAX);
            }
        }
        break;
    case N_EXPR:   n->lhs->flags |= DISCARD; gen_expr(n->lhs); break;
    case N_RETURN:
        if (n->lhs) gen_expr(n->lhs);
        emit("leave"); emit("ret");
        break;
    case N_IF: {
        int els = label_id++;
        gen_expr(n->cond); emit("test rax, rax"); emit_jmp(n, "jz", els);
        gen_stmt(n->lhs);
        if (n->rhs) {
            int end = label_id++; emit_jmp(n, "jmp", end);
            emit_label(els); gen_stmt(n->rhs); els = end;
        }
        emit_label(els);
        break;
    }
    case N_WHILE: {
        int top = label_id++, end = label_id++;
        emit_label(top);
        gen_expr(n->cond); emit("test rax, rax"); emit_jmp(n, "jz", end);
        loop_push(n, end, top); gen_stmt(n->lhs); loop_pop();
        // XXX: should duplicate test
        emit_jmp(n, "jmp", top);
        emit_label(end);
        break;
    }
    case N_DOWHILE: {
        int top = label_id++, cont = 0, end = 0;
        if (n->ival & HAS_CONTINUE) cont = label_id++;
        if (n->ival & HAS_BREAK) end = label_id++;
        emit_label(top);
        loop_push(n, end, cont); gen_stmt(n->lhs); loop_pop();
        emit_label(cont);
        gen_expr(n->cond); emit("test rax, rax"); emit_jmp(n, "jnz", top);
        emit_label(end);
        break;
    }
    case N_FOR: {
        int top = label_id++, cont = top, end = 0;
        if (n->rhs && (n->ival & HAS_CONTINUE)) cont = label_id++;
        if (n->cond || (n->ival & HAS_BREAK)) end = label_id++;
        if (n->init) gen_stmt(n->init);
        emit_label(top);
        if (n->cond) { gen_expr(n->cond); emit("test rax, rax"); emit_jmp(n, "jz", end); }
        loop_push(n, end, cont); gen_stmt(n->lhs); loop_pop();
        if (n->rhs) {
            if (cont != top) emit_label(cont);
            n->rhs->flags |= DISCARD; gen_expr(n->rhs);  // step
        }
        // XXX: should duplicate test
        emit_jmp(n, "jmp", top);
        emit_label(end);
        break;
    }
    case N_BREAK:    emit_jmp(n, "jmp", brk_lbl[loop_sp]);  break;
    case N_CONTINUE: emit_jmp(n, "jmp", cont_lbl[loop_sp]); break;
    case N_SWITCH:   gen_switch(n); break;
    case N_CASE:     gen_case_comment(n); emit_label((int)n->ival); break;
    case N_DEFAULT:
    case N_LABEL:    emit_comment(atom_str(n->name)); emit_label((int)n->ival); break;
    case N_GOTO: {
        Label *lab = find_label(n->name);
        if (!lab) {
            error(n, "label '%s' not found", atom_str(n->name));
            break;
        }
        lab->used = true;
        emit_comment(atom_str(n->name)); emit_jmp(n, "jmp", (int)lab->n->ival);
        break;
    }
    case N_ASM: gen_asm(n);  break;
    default:    gen_expr(n); break;    // node is part of an expression
    }
}

static void gen_func(Func *fn) {
    if (!fn->body) return;  // external function prototype
    this_fn = fn;
    for (Label *lab = fn->labels; lab; lab = lab->next) { lab->n->ival = label_id++; }
    locals = NULL; frame_size = 0;
    // params first (so they get the lowest offsets, in declared order)
    for (Node *param = fn->param; param; param = param->next) add_local(param->name, param->pos, param->type);
    collect_locals(fn->body);
    // reserve a 48-byte register save area for variadic functions
    int va_off = 0;
    if (fn->is_variadic) { frame_size += 48; va_off = frame_size; }
    cur_va_off = va_off; cur_named = fn->nparams;
    int fs = (frame_size + 15) & ~15;

    emit_entry(fn->name, !(fn->flags & HAS_STATIC), true);
    emit("push rbp");
    emit("mov rbp, rsp");
    if (fs > 0) emit("sub rsp, %d", fs);
    int i = 0;
    for (Node *param = fn->param; param && i < 6; param = param->next, i++) {
        Sym *s = sym_find(locals, param->name);
        emit_comment(atom_str(param->name));
        emit("mov [rbp - %d], %s", s->offset, reg64[ARGREG[i]]);
    }
    if (fn->is_variadic) {
        // spill all six integer arg registers so va_arg can walk them
        // XXX: should only spill arg registers beyond the 'last' named parameter
        for (int i = 0; i < 6; i++) emit("mov [rbp - %d], %s", va_off - i * 8, reg64[ARGREG[i]]);
    }
    gen_stmt(fn->body);
    if (has_flow(fn->body)) {
        if (fn->name == ID_MAIN) {
            emit("xor rax, rax");   // main returns 0 by default
        } else
        if (fn->rtype != ty_void()) {
            // should flag non void functions with out flow
            // disabled for now because loops and switches are not full analysed
            warning(fn->endpos, "function '%s': missing return statement", atom_str(fn->name));
        }
        emit("leave"); emit("ret"); // safety epilogue
    }
}

static void gen_init(Type *t, atom_t name, Node *init, const char *mname) {
    if (init) {
        long ival;
        switch (t->kind) {
        case TY_VOID:
            break;
        case TY_INT:
        case TY_CHAR:
        case TY_SHORT:
        case TY_LONG:
        case TY_ENUM:
            if (eval_expr(init, &ival)) {
                emit_comment(mname);
                switch (t->align) {
                case 1: emit(".byte %ld", ival); return;
                case 2: emit(".short %ld", ival); return;
                case 4: emit(".long %ld", ival); return;
                case 8: emit(".quad %ld", ival); return;
                default: warning(t->pos, "invalid alignment %d for type %d", t->align, t->kind); exit(1);
                }
            }
            break;
        case TY_PTR:
            if (t->ptr->kind == TY_CHAR) {
                if (init->kind == N_STR) {
                    atom_flags(init->str) |= ATOM_USED;
                    emit_comment(mname); emit(".quad .LC%d", init->str);
                    return;
                }
            }
            if (init->kind == N_NUM) {
                if (eval_expr(init, &ival)) {
                    emit_comment(mname); emit(".quad %ld", ival);
                    return;
                }
            }
            // XXX: support other initializers: char buf[100], *p = buf;
            break;
        case TY_ARRAY:
            if (init->kind == N_BLOCK) {
                Node *e = init->rhs;
                for (int i = 0; i < t->arr_len; i++) {
                    gen_init(t->ptr, 0, e, NULL);
                    if (e) e = e->next;
                }
                return;
            }
            // XXX: support other initializers: char digits[] = "0123456789abcdef";
            break;
        case TY_STRUCT:
        case TY_UNION:
            if (init->kind != N_BLOCK) break;
            Node *e = init->rhs;
            for (Member *m = t->members; m; m = m->next) {
                gen_init(m->type, 0, e, atom_str(m->name));
                if (e) e = e->next;
                if (m->pad) emit(".zero %d", m->pad);
                if (t->kind == TY_UNION) break;
            }
            return;
        }
        if (name) { warning(init->pos, "unsupported initializer for '%s'", atom_str(name)); }
        else if (mname) { warning(init->pos, "unsupported initializer for member '%s'", mname); }
        else { warning(init->pos, "unsupported initializer"); }
    }
    emit_comment(mname);
    int sz = ty_size(t); if (sz < 1) sz = 8;
    emit(".zero %d", sz);
}

// Pretty print the token list
static bool separate_tokens(Token *t) {
    // XXX need a fix for pointers in declarations
    int t1 = t[-1].kind, t2 = t->kind, t3;
    switch (t1) {
    case T_LP:  case T_LBRK:   case T_DOT: case T_ARROW:
    case T_NOT: case T_BITNOT: case T_ELLIPSIS:
        return false;
    case K_IF:  case K_WHILE: case K_FOR: case K_SWITCH:
    case T_COMMA:
        return true;
    case T_SEMI:
        return t2 != T_SEMI && t2 != T_RP;
    case T_PLUS: case T_MINUS: case T_STAR: case T_AMP:
    case T_INC:  case T_DEC:
        t3 = t[-2].kind;
        if (t3 >= T_PLUS && t3 <= K_GOTO && t3 != T_RBRK) return false;
        break;
    case T_RP:
        t3 = t[-2].kind;
        if (IS_TYPE(t3) || t3 == T_STAR) return false;
        break;
    }
    switch (t2) {
    case T_RP:   case T_LBRK:  case T_RBRK:
    case T_SEMI: case T_COMMA: case T_DOT:  case T_ARROW:
        return false;
    case T_INC:  case T_DEC:
        return t1 >= T_PLUS && t1 <= T_SHREQ;
//    case T_NUM:  case T_STR:   case T_ID:   case K_SIZEOF:
//        return (t1 != T_RP);
    case T_LP:
        return (t1 != T_RP && t1 != T_ID && t1 != K_SIZEOF);
    }
    return true;
}

static int output_token(FILE *fp, Token *t) {
    char buf[8192], c;
    const char *s = buf;
    switch (t->kind) {
    case T_NUM: {
            char *p = buf + 68; *--p = '\0';
            unsigned long val = (unsigned long)t->ival;
            unsigned char base = t->base;
            if (t->is_unsigned) *--p = 'U';
            if (t->is_long) *--p = 'L';
            do { *--p = "0123456789abcdef"[val % base]; } while (val /= base);
            switch (base) {
            case 8:   if (*p != '0') *--p = '0'; break;
            case 2:   *--p = 'b';    *--p = '0'; break;
            case 16:  *--p = 'x';    *--p = '0'; break;
            }
            s = p; break;
        }
    case T_ID:    s = atom_str(t->text); break;
    case T_STR:   gen_quoted_string(buf, sizeof(buf), atom_str(t->text), atom_len(t->text), '"'); break;
    case T_CHAR:  c = (char)t->ival; gen_quoted_string(buf, sizeof(buf), &c, 1, '\''); break;
    default:      s = atom_str((atom_t)t->kind); break;
    }
    size_t len = strlen(s); fwrite(s, 1, len, fp); return (int)len;
}

static int output_tokens(FILE *fp, Token *t, size_t n) {
    int indent = 0, indent_col = 0, col = 0;
    int paren_level = 0, last_pos = 0;
    bool has_label = false, bol = true;
    for (size_t i = 0; i < n; i++, t++) {
        int pos = t->pos;
        if (pos) {
            if ((last_pos >> 24 != pos >> 24) || pos < last_pos || pos - last_pos > 5) {
                if (last_pos) fputc('\n', fp);
                fprintf(fp, "#line %d \"%s\"\n", pos & 0xffffff, src_name[pos >> 24]);
                last_pos = pos;
                bol = true;
            } else {
                while (last_pos < pos) { fputc('\n', fp); last_pos++; bol = true; }
            }
        }
        int kind = t->kind;
        if (kind == T_EOF) break;
        if (bol) {
            bol = false;
            has_label = false;
            if (kind == T_RBRACE) {
                col = indent - 4;
            } else
            if ((kind == T_ID && t[1].kind == T_COLON)
            ||  kind == K_CASE || kind == K_DEFAULT) {
                col = indent - 2;
            } else
            if (paren_level && kind != T_ANDAND && kind != T_OROR) {
                col = indent_col;
            } else {
                col = indent;
            }
            int i = 0;
            while (i++ < col) fputc(' ', fp);
            col = i;
        } else {
            bool use_space = separate_tokens(t);
            if (kind == T_COLON && has_label) has_label = use_space = false;
            if (use_space) { fputc(' ', fp); col++; }
        }
        if (kind == K_CASE || kind == K_DEFAULT) has_label = true;
        if (kind == T_LBRACE) indent += 4;
        if (kind == T_RBRACE && indent) indent -= 4;
        if (kind == T_LP || kind == T_LBRK) { if (!paren_level++) indent_col = col; }
        if (kind == T_RP || kind == T_RBRK) { if (!--paren_level) indent_col = 0; }
        col += output_token(fp, t);
    }
    return 0;
}

// =====================================================================
// 8. MAIN
// =====================================================================

static _Noreturn void usage(bool full) {
    fprintf(stderr, "Usage: %s [OPTIONS] <input.c> [<output.s]\n", progname);
    if (full) {
        fprintf(stderr,
                "  --kernel       kernel mode (no bss, no _start/exit stub)\n"
                "  --libc         hosted mode (no _start, link to the C library\n"
                "  -g             add debug info in assembly source code\n"
                "  -memory        show memory stats\n"
                "  -time          show timings\n"
                "  -E             output preprocessed test\n"
                "  -ET            output preprocessed tokens\n"
                "  -O             perform optimizations\n"
                "  -o <output>    set the output filename\n"
                "  -v  --verbose  output progress messages\n");
    }
    exit(1);
}

static _Noreturn void arg_error(const char *msg, const char *arg) {
    fprintf(stderr, "%s: %s%s\n", progname, msg, arg);
    usage(false);
}

int main(int argc, char **argv) {
    // Optional flags may precede the file names.  --kernel suppresses the
    // Linux _start/exit stub so a bare-metal boot stub can provide the entry
    // point and simply call main() (no Linux syscalls exist in a kernel).
    long t0 = now();
    progname = argv[0];
    if (argc == 1) usage(true);
    int preprocess_mode = 0, timings = 0;
    bool kernel_mode = false, libc_mode = false, mem_stats = false;
    const char *inpath = NULL, *outpath = NULL;
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        if      (!strcmp(arg, "--kernel") || !strcmp(arg, "-k")) kernel_mode = true;
        else if (!strcmp(arg, "--help") || !strcmp(arg, "-?")) usage(true);
        else if (!strcmp(arg, "--libc")) libc_mode = true;
        else if (!strcmp(arg, "-E")) preprocess_mode = 1;
        else if (!strcmp(arg, "-ET")) preprocess_mode = 2;
        else if (!strcmp(arg, "-O")) optimize++;
        else if (!strcmp(arg, "-g")) debug = out_comments = true;
        else if (!strcmp(arg, "-time")) timings++;
        else if (!strcmp(arg, "-memory")) mem_stats = true;
        else if (!strcmp(arg, "--verbose") || !strcmp(arg, "-v")) verbose = true;
        else if (!strcmp(arg, "-o")) { if (!argv[i+1]) arg_error("missing output filename", ""); outpath = argv[++i]; }
        else if (*arg == '-') arg_error("invalid option: ", arg);
        else if (!inpath)  inpath  = arg;
        else if (!outpath) outpath = arg;
        else arg_error("too many arguments", "");
    }
    if (!inpath) arg_error("missing filename", "");

    lex_init();
    sbuf_t src[1]; sbuf_init(src, 128 * 1024);
    preprocess(inpath, src);
    if (preprocess_mode == 1) { fputs(sbuf_getptr(src), stdout); return 0; }
    lex(sbuf_getptr(src));
    sbuf_deinit(src);

    if (!preprocess_mode) { while (!at(T_EOF)) parse_toplevel(); }
    if (!outpath) {
        if (preprocess_mode) { fout = stdout; }
        else {
            size_t len = strlen(inpath);
            if (len > 2 && !memcmp(inpath + len - 2, ".c", 2)) len -= 2;
            char *p = alloc(len + 3, 1);
            memcpy(p, inpath, len); strcpy(p + len, ".s"); outpath = p;
        }
    }
    if (!fout) {
        if (!strcmp(outpath, "-")) fout = stdout;
        else fout = fopen(outpath, "w");
        if (!fout) { die("cannot open %s: %s", outpath, strerror(errno)); }
    }
    if (preprocess_mode) {
        int status = output_tokens(fout, toks, ntok);
        if (fout != stdout) fclose(fout);
        return status;
    }

    check_used_func(ID_MAIN);

    emit(".intel_syntax noprefix");
    emit_section(".text", 0);

    if (!kernel_mode && !libc_mode) {
        // hosted freestanding entry point: run main, then exit(rax)
        emit_entry(ID_START, true, true);
        emit("cld");
        emit("xor rbp, rbp");
        emit_comment("argc"); emit("mov rdi, [rsp]");
        emit_comment("argv"); emit("lea rsi, [rsp+8]");
        emit_comment("envp"); emit("lea rdx, [rsi+8*rdi+8]");
        emit("call main");
        emit("mov rdi, rax");
        emit_comment("SYS_exit"); emit("mov rax, 60");
        emit("syscall");
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
    emit_section(".data", 8);
    for (Sym *g = globals; g; g = g->next) {
        if (g->is_constant) continue;
        if (!kernel_mode && !g->init) {
            if (g->flags & HAS_STATIC) emit(".local %s", atom_str(g->name));
            int sz = ty_size(g->type); if (sz < 1) sz = 8;
            emit(".comm %s, %d", atom_str(g->name), sz);
        } else {
            emit_entry(g->name, !(g->flags & HAS_STATIC), false);
            gen_init(g->type, g->name, g->init, NULL);
        }
    }
    emit_section(".rodata", 0);
    for (atom_t s = 0; s < natoms; s++) {
        if (atom_flags(s) & ATOM_USED) gen_string_def(s);
    }

    fclose(fout);
    if (verbose) printf("Compiled %s -> %s%s\n", inpath, outpath, kernel_mode ? " (kernel mode)" : "");
    t0 = now() - t0;
    if (timings) printf("total time: %ld ms\n", (t0 + 500) / 1000);
    if (mem_stats) malloc_stats();
    return 0;
}
