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

// ---------------------------------------------------------------------
// Output / errors
// ---------------------------------------------------------------------
static FILE *fout;
static void die(const char *m) { fprintf(stderr, "nano_cc: error: %s\n", m); exit(1); }
static void emit(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(fout, fmt, a); va_end(a);
    fputc('\n', fout);
}

// =====================================================================
// 1. PREPROCESSOR  ->  produces a single clean source buffer in SRC
// =====================================================================
#define MAX_SRC   400000
static char SRC[MAX_SRC];
static int  SRC_LEN = 0;

typedef struct { char name[64]; char value[512]; } Macro;
#define MAX_MACROS 1024
static Macro macros[MAX_MACROS];
static int   macro_cnt = 0;

static Macro *macro_find(const char *name) {
    for (int i = 0; i < macro_cnt; i++)
        if (!strcmp(macros[i].name, name)) return &macros[i];
    return NULL;
}
static void macro_define(const char *name, const char *value) {
    Macro *m = macro_find(name);
    if (!m) { m = &macros[macro_cnt++]; snprintf(m->name, sizeof(m->name), "%s", name); }
    snprintf(m->value, sizeof(m->value), "%s", value);
}
static void src_putc(char c) {
    if (SRC_LEN >= MAX_SRC - 1) die("source too large");
    SRC[SRC_LEN++] = c;
}
static void src_puts(const char *s) { while (*s) src_putc(*s++); }

// Expand object-like macros inside a single logical line of code.
static void expand_line(const char *in) {
    char work[8192];
    snprintf(work, sizeof(work), "%s", in);
    for (int pass = 0; pass < 8; pass++) {
        char out[8192]; int j = 0, changed = 0;
        for (int i = 0; work[i]; ) {
            char c = work[i];
            if (c == '"' || c == '\'') {          // copy string/char literal verbatim
                char q = c; out[j++] = work[i++];
                while (work[i] && work[i] != q) {
                    if (work[i] == '\\' && work[i+1]) out[j++] = work[i++];
                    out[j++] = work[i++];
                }
                if (work[i]) out[j++] = work[i++];
            } else if (isalpha((unsigned char)c) || c == '_') {
                char word[64]; int k = 0;
                while (work[i] && (isalnum((unsigned char)work[i]) || work[i] == '_') && k < 63)
                    word[k++] = work[i++];
                word[k] = 0;
                Macro *m = macro_find(word);
                if (m) { for (const char *p = m->value; *p; p++) out[j++] = *p; changed = 1; }
                else   { for (int t = 0; word[t]; t++) out[j++] = word[t]; }
            } else {
                out[j++] = work[i++];
            }
            if (j >= (int)sizeof(out) - 2) break;
        }
        out[j] = 0;
        memcpy(work, out, j + 1);
        if (!changed) break;
    }
    src_puts(work);
}

// Strip // and /* */ comments (string/char aware) into `dst`.
static void strip_comments(const char *s, char *dst) {
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
            while (s[i] && !(s[i] == '*' && s[i+1] == '/')) i++;
            if (s[i]) i += 2;
            dst[j++] = ' ';
        } else {
            dst[j++] = s[i++];
        }
    }
    dst[j] = 0;
}

static void preprocess(const char *path);   // fwd

static int cond_all_active(const int *cond, int sp) {
    for (int i = 0; i < sp; i++) if (!cond[i]) return 0;
    return 1;
}

// Process already-comment-stripped text of one file.
// NB: iterate lines manually (no strtok) so nested #include recursion is safe.
static void process_text(const char *text) {
    int cond[64]; int cond_sp = 0;
    #define ACTIVE() cond_all_active(cond, cond_sp)

    const char *cursor = text;
    while (*cursor) {
        char line[8192]; int li = 0;
        while (*cursor && *cursor != '\n' && li < 8191) line[li++] = *cursor++;
        line[li] = 0;
        if (*cursor == '\n') cursor++;

        // find first non-space
        const char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            char dir[32]; int di = 0;
            while (*p && (isalpha((unsigned char)*p)) && di < 31) dir[di++] = *p++;
            dir[di] = 0;
            while (*p == ' ' || *p == '\t') p++;
            if (!strcmp(dir, "ifndef") || !strcmp(dir, "ifdef")) {
                char nm[64]; int k = 0;
                while (*p && (isalnum((unsigned char)*p) || *p == '_') && k < 63) nm[k++] = *p++;
                nm[k] = 0;
                int defined = macro_find(nm) != NULL;
                int take = (dir[2] == 'n') ? !defined : defined;   // ifndef vs ifdef
                int active_now = ACTIVE();
                cond[cond_sp++] = active_now ? take : 0;
            } else if (!strcmp(dir, "if")) {
                cond[cond_sp++] = 0;                 // unsupported #if -> skip body
            } else if (!strcmp(dir, "else")) {
                if (cond_sp > 0) cond[cond_sp-1] = !cond[cond_sp-1];
            } else if (!strcmp(dir, "endif")) {
                if (cond_sp > 0) cond_sp--;
            } else if (!strcmp(dir, "define")) {
                if (ACTIVE()) {
                    char nm[64]; int k = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && k < 63) nm[k++] = *p++;
                    nm[k] = 0;
                    while (*p == ' ' || *p == '\t') p++;
                    char val[512]; int v = 0;
                    while (*p && v < 511) val[v++] = *p++;
                    while (v > 0 && (val[v-1] == ' ' || val[v-1] == '\t' || val[v-1] == '\r')) v--;
                    val[v] = 0;
                    macro_define(nm, val);
                }
            } else if (!strcmp(dir, "undef")) {
                // leave defined (rare); no-op is safe for our inputs
            } else if (!strcmp(dir, "include")) {
                if (ACTIVE() && *p == '"') {
                    p++;
                    char fn[256]; int k = 0;
                    while (*p && *p != '"' && k < 255) fn[k++] = *p++;
                    fn[k] = 0;
                    preprocess(fn);
                }
                // <...> system includes are ignored (freestanding)
            }
        } else {
            if (ACTIVE()) { expand_line(line); src_putc('\n'); }
        }
    }
    #undef ACTIVE
}

static void preprocess(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "nano_cc: cannot open %s\n", path); exit(1); }
    char *raw   = malloc(MAX_SRC);
    char *nocmt = malloc(MAX_SRC);
    if (!raw || !nocmt) die("out of memory");
    int n = 0, c;
    while ((c = fgetc(f)) != EOF && n < MAX_SRC - 1) raw[n++] = (char)c;
    raw[n] = 0;
    fclose(f);
    strip_comments(raw, nocmt);          // reads raw -> writes nocmt
    process_text(nocmt);                  // may recurse (with its own buffers)
    free(raw); free(nocmt);
}

// =====================================================================
// 2. LEXER
// =====================================================================
enum {
    T_NUM, T_ID, T_STR, T_CHAR,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_ASSIGN, T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE,
    T_ANDAND, T_OROR, T_NOT, T_AMP,
    T_INC, T_DEC,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_LP, T_RP, T_LBRK, T_RBRK, T_LBRACE, T_RBRACE,
    T_SEMI, T_COMMA, T_QUESTION, T_COLON, T_DOT, T_ARROW,
    T_KINT, T_KLONG, T_KCHAR, T_KVOID,
    T_KIF, T_KELSE, T_KWHILE, T_KRETURN, T_KASM,
    T_KFOR, T_KDO, T_KBREAK, T_KCONTINUE,
    T_KSTRUCT, T_KUNION, T_KSIZEOF,
    T_EOF
};

typedef struct {
    int   kind;
    long  ival;          // T_NUM / T_CHAR
    char  text[256];     // T_ID
    char *str;           // T_STR (decoded bytes)
    int   slen;          // T_STR length
} Token;

#define MAX_TOK 60000
static Token toks[MAX_TOK];
static int   ntok = 0;

static void add_tok(int kind) { toks[ntok].kind = kind; ntok++; }

static int  sp = 0;                       // scan position in SRC
static void skip_space(void) { while (sp < SRC_LEN && isspace((unsigned char)SRC[sp])) sp++; }

static int read_escape(void) {            // SRC[sp] points just past a backslash
    char c = SRC[sp++];
    switch (c) {
        case 'n': return '\n';  case 't': return '\t';  case 'r': return '\r';
        case '0': return '\0';  case '\\': return '\\'; case '\'': return '\'';
        case '"': return '"';   case 'b': return '\b';  case 'f': return '\f';
        case 'v': return '\v';  default:  return c;
    }
}

static char strpool[200000];
static int  strpool_len = 0;

static void lex(void) {
    for (;;) {
        skip_space();
        if (sp >= SRC_LEN) { add_tok(T_EOF); return; }
        char c = SRC[sp];

        if (isdigit((unsigned char)c)) {
            long v = 0;
            if (c == '0' && sp + 1 < SRC_LEN && (SRC[sp+1] == 'x' || SRC[sp+1] == 'X')) {
                sp += 2;
                while (sp < SRC_LEN && isxdigit((unsigned char)SRC[sp])) {
                    char d = SRC[sp++];
                    int dv = (d <= '9') ? d - '0' : (tolower(d) - 'a' + 10);
                    v = v * 16 + dv;
                }
            } else {
                while (sp < SRC_LEN && isdigit((unsigned char)SRC[sp])) v = v * 10 + (SRC[sp++] - '0');
            }
            toks[ntok].ival = v; add_tok(T_NUM); continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            char buf[256]; int i = 0;
            while (sp < SRC_LEN && (isalnum((unsigned char)SRC[sp]) || SRC[sp] == '_') && i < 255)
                buf[i++] = SRC[sp++];
            buf[i] = 0;
            int k = T_ID;
            if      (!strcmp(buf, "int"))      k = T_KINT;
            else if (!strcmp(buf, "long"))     k = T_KLONG;
            else if (!strcmp(buf, "char"))     k = T_KCHAR;
            else if (!strcmp(buf, "void"))     k = T_KVOID;
            else if (!strcmp(buf, "if"))       k = T_KIF;
            else if (!strcmp(buf, "else"))     k = T_KELSE;
            else if (!strcmp(buf, "while"))    k = T_KWHILE;
            else if (!strcmp(buf, "return"))   k = T_KRETURN;
            else if (!strcmp(buf, "for"))      k = T_KFOR;
            else if (!strcmp(buf, "do"))       k = T_KDO;
            else if (!strcmp(buf, "break"))    k = T_KBREAK;
            else if (!strcmp(buf, "continue")) k = T_KCONTINUE;
            else if (!strcmp(buf, "struct"))   k = T_KSTRUCT;
            else if (!strcmp(buf, "union"))    k = T_KUNION;
            else if (!strcmp(buf, "sizeof"))   k = T_KSIZEOF;
            else if (!strcmp(buf, "__asm__") || !strcmp(buf, "asm")) k = T_KASM;
            else if (!strcmp(buf, "const") || !strcmp(buf, "unsigned") ||
                     !strcmp(buf, "signed") || !strcmp(buf, "static") ||
                     !strcmp(buf, "inline") || !strcmp(buf, "register") ||
                     !strcmp(buf, "volatile")) continue;   // qualifier: drop
            if (k == T_ID) snprintf(toks[ntok].text, sizeof(toks[ntok].text), "%s", buf);
            add_tok(k); continue;
        }
        if (c == '"') {                              // string literal (+ adjacent concat)
            char *start = &strpool[strpool_len]; int len = 0;
            for (;;) {
                sp++;                                 // skip opening quote
                while (sp < SRC_LEN && SRC[sp] != '"') {
                    int ch;
                    if (SRC[sp] == '\\') { sp++; ch = read_escape(); }
                    else ch = SRC[sp++];
                    strpool[strpool_len++] = (char)ch; len++;
                }
                if (sp < SRC_LEN) sp++;               // skip closing quote
                skip_space();
                if (sp < SRC_LEN && SRC[sp] == '"') continue;   // concatenate
                break;
            }
            strpool[strpool_len++] = 0;
            toks[ntok].str = start; toks[ntok].slen = len; add_tok(T_STR); continue;
        }
        if (c == '\'') {
            sp++;
            int ch;
            if (SRC[sp] == '\\') { sp++; ch = read_escape(); }
            else ch = SRC[sp++];
            if (sp < SRC_LEN && SRC[sp] == '\'') sp++;
            toks[ntok].ival = ch; add_tok(T_CHAR); continue;
        }

        char c2 = (sp + 1 < SRC_LEN) ? SRC[sp+1] : 0;
        #define TWO(a,b,K)  if (c==a && c2==b) { sp += 2; add_tok(K); goto next; }
        TWO('=','=',T_EQ) TWO('!','=',T_NE) TWO('<','=',T_LE) TWO('>','=',T_GE)
        TWO('&','&',T_ANDAND) TWO('|','|',T_OROR)
        TWO('+','+',T_INC) TWO('-','-',T_DEC) TWO('-','>',T_ARROW)
        TWO('+','=',T_PLUSEQ) TWO('-','=',T_MINUSEQ) TWO('*','=',T_STAREQ)
        TWO('/','=',T_SLASHEQ) TWO('%','=',T_PERCENTEQ)
        #undef TWO
        sp++;
        switch (c) {
            case '+': add_tok(T_PLUS);   break;  case '-': add_tok(T_MINUS);  break;
            case '*': add_tok(T_STAR);   break;  case '/': add_tok(T_SLASH);  break;
            case '%': add_tok(T_PERCENT);break;  case '=': add_tok(T_ASSIGN); break;
            case '<': add_tok(T_LT);     break;  case '>': add_tok(T_GT);     break;
            case '!': add_tok(T_NOT);    break;  case '&': add_tok(T_AMP);    break;
            case '(': add_tok(T_LP);     break;  case ')': add_tok(T_RP);     break;
            case '[': add_tok(T_LBRK);   break;  case ']': add_tok(T_RBRK);   break;
            case '{': add_tok(T_LBRACE); break;  case '}': add_tok(T_RBRACE); break;
            case ';': add_tok(T_SEMI);   break;  case ',': add_tok(T_COMMA);  break;
            case '?': add_tok(T_QUESTION);break; case ':': add_tok(T_COLON);  break;
            case '.': add_tok(T_DOT);    break;
            case '|': continue;                  // stray '|' — ignore
            default:  die("unknown character in source");
        }
        next: ;
    }
}

// =====================================================================
// 3. TYPES
// =====================================================================
enum { TY_INT, TY_CHAR, TY_LONG, TY_VOID, TY_PTR, TY_STRUCT };
typedef struct Type Type;
typedef struct Member { char name[256]; Type *type; int offset; struct Member *next; } Member;
struct Type {
    int kind; Type *ptr; int is_array; int arr_len;
    Member *members; int struct_size; char tag[256];  // TY_STRUCT
};

static Type *ty_int(void)  { static Type t = { TY_INT,  0, 0, 0, 0, 0, {0} }; return &t; }
static Type *ty_char(void) { static Type t = { TY_CHAR, 0, 0, 0, 0, 0, {0} }; return &t; }
static Type *ty_long(void) { static Type t = { TY_LONG, 0, 0, 0, 0, 0, {0} }; return &t; }
static Type *ty_void(void) { static Type t = { TY_VOID, 0, 0, 0, 0, 0, {0} }; return &t; }

static Type *ptr_to(Type *base) {
    Type *t = calloc(1, sizeof(Type)); t->kind = TY_PTR; t->ptr = base; return t;
}
static int ty_size(Type *t) {
    if (t->is_array) return t->arr_len * ty_size(t->ptr);
    switch (t->kind) {
        case TY_CHAR:   return 1;
        case TY_VOID:   return 0;
        case TY_STRUCT: return t->struct_size;
        default:        return 8;
    }
}
static Member *find_member(Type *st, const char *name) {
    if (st->kind == TY_PTR) st = st->ptr;
    for (Member *m = st->members; m; m = m->next) if (!strcmp(m->name, name)) return m;
    return NULL;
}
// element size for pointer/array arithmetic (bytes per step); 0 if not a pointer
static int elem_size(Type *t) {
    if (t->is_array) return ty_size(t->ptr);
    if (t->kind == TY_PTR) { int s = ty_size(t->ptr); return s ? s : 1; }
    return 0;
}
static int is_ptrish(Type *t) { return t->is_array || t->kind == TY_PTR; }

// =====================================================================
// 4. AST
// =====================================================================
enum {
    N_NUM, N_STR, N_VAR, N_CALL, N_ASSIGN, N_BIN, N_UNARY,
    N_POST, N_CAST, N_DEREF, N_ADDR, N_LOGAND, N_LOGOR,
    N_IF, N_WHILE, N_RETURN, N_BLOCK, N_EXPR, N_DECL, N_ASM, N_EMPTY,
    N_FOR, N_DOWHILE, N_BREAK, N_CONTINUE, N_TERNARY, N_PRE,
    N_MEMBER, N_SIZEOF
};

typedef struct Node Node;
struct Node {
    int   kind;
    int   op;               // token kind for N_BIN / N_POST (T_INC/T_DEC)
    long  ival;             // N_NUM
    char *str; int slen;    // N_STR
    char  name[256];        // N_VAR / N_CALL / N_DECL
    Type *type;             // result / declared type
    Node *lhs, *rhs, *cond, *els, *init;
    Node *args[8]; int nargs;
    Node *body[512]; int nbody;   // N_BLOCK statements
    char *asmtext;                // N_ASM decoded text
};

static Node *new_node(int k) { Node *n = calloc(1, sizeof(Node)); n->kind = k; return n; }

// ---- symbols ----
typedef struct Sym { char name[256]; Type *type; int is_global; int offset; struct Sym *next; } Sym;
static Sym *globals = NULL;

// per-function local table (params + locals), built during offset pass
static Sym *locals = NULL;
static int  frame_size = 0;

static Sym *sym_find(Sym *list, const char *name) {
    for (; list; list = list->next) if (!strcmp(list->name, name)) return list;
    return NULL;
}
static Sym *add_global(const char *name, Type *t) {
    Sym *s = calloc(1, sizeof(Sym)); snprintf(s->name, sizeof(s->name), "%s", name);
    s->type = t; s->is_global = 1; s->next = globals; globals = s; return s;
}

// =====================================================================
// 5. PARSER
// =====================================================================
static int  P = 0;                       // token cursor
static Token *cur(void) { return &toks[P]; }
static int  at(int k)   { return toks[P].kind == k; }
static int  eat(int k)  { if (toks[P].kind == k) { P++; return 1; } return 0; }
static void expect(int k) { if (!eat(k)) { fprintf(stderr, "nano_cc: parse error near token %d (kind %d)\n", P, toks[P].kind); exit(1); } }

static int is_type_start(int k) {
    return k == T_KINT || k == T_KLONG || k == T_KCHAR || k == T_KVOID ||
           k == T_KSTRUCT || k == T_KUNION;
}

// ---- struct/union tag table ----
typedef struct Tag { char name[256]; Type *type; struct Tag *next; } Tag;
static Tag *tags = NULL;
static Type *tag_find(const char *name) {
    for (Tag *t = tags; t; t = t->next) if (!strcmp(t->name, name)) return t->type;
    return NULL;
}
static Type *tag_get(const char *name) {          // find or forward-declare
    Type *t = tag_find(name);
    if (t) return t;
    t = calloc(1, sizeof(Type)); t->kind = TY_STRUCT; snprintf(t->tag, sizeof(t->tag), "%s", name);
    Tag *e = calloc(1, sizeof(Tag)); snprintf(e->name, sizeof(e->name), "%s", name);
    e->type = t; e->next = tags; tags = e;
    return t;
}

static Node *parse_expr(void);
static Node *parse_assign(void);
static Node *parse_stmt(void);
static Type *parse_type_base_only(void);

// struct/union specifier:  (struct|union) [tag] [ { members } ]
static Type *parse_struct_specifier(void) {
    int is_union = eat(T_KUNION); if (!is_union) expect(T_KSTRUCT);
    char tag[256] = {0};
    if (at(T_ID)) { snprintf(tag, sizeof(tag), "%s", cur()->text); P++; }
    Type *st;
    if (tag[0]) st = tag_get(tag);
    else { st = calloc(1, sizeof(Type)); st->kind = TY_STRUCT; }
    if (eat(T_LBRACE)) {                           // definition
        Member *head = NULL, *tail = NULL;
        int off = 0, maxsz = 0;
        while (!at(T_RBRACE) && !at(T_EOF)) {
            Type *mbase = parse_type_base_only();
            for (;;) {
                Type *mt = mbase;
                while (eat(T_STAR)) mt = ptr_to(mt);
                char mnm[256]; snprintf(mnm, sizeof(mnm), "%s", cur()->text); expect(T_ID);
                if (eat(T_LBRK)) {                 // array member
                    long len = cur()->ival; expect(T_NUM); expect(T_RBRK);
                    Type *arr = calloc(1, sizeof(Type)); arr->kind = TY_PTR; arr->ptr = mt;
                    arr->is_array = 1; arr->arr_len = (int)len; mt = arr;
                }
                int msz = ty_size(mt);
                int align = msz < 8 ? (msz ? msz : 1) : 8;
                Member *m = calloc(1, sizeof(Member));
                snprintf(m->name, sizeof(m->name), "%s", mnm); m->type = mt;
                if (is_union) { m->offset = 0; if (msz > maxsz) maxsz = msz; }
                else { off = (off + align - 1) & ~(align - 1); m->offset = off; off += msz; }
                if (!head) head = tail = m; else { tail->next = m; tail = m; }
                if (!eat(T_COMMA)) break;
            }
            expect(T_SEMI);
        }
        expect(T_RBRACE);
        st->members = head;
        st->struct_size = is_union ? ((maxsz + 7) & ~7) : ((off + 7) & ~7);
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
        if (is_type_start(cur()->kind)) {
            Type *t = parse_type();
            expect(T_RP);
            Node *n = new_node(N_CAST); n->type = t; n->lhs = parse_assign();
            return n;
        }
        Node *n = parse_expr(); expect(T_RP); return n;
    }
    if (at(T_NUM))  { Node *n = new_node(N_NUM);  n->ival = cur()->ival; n->type = ty_long(); P++; return n; }
    if (at(T_CHAR)) { Node *n = new_node(N_NUM);  n->ival = cur()->ival; n->type = ty_int();  P++; return n; }
    if (at(T_STR))  { Node *n = new_node(N_STR);  n->str  = cur()->str;  n->slen = cur()->slen;
                      n->type = ptr_to(ty_char()); P++; return n; }
    if (at(T_ID)) {
        char nm[256]; snprintf(nm, sizeof(nm), "%s", cur()->text); P++;
        if (eat(T_LP)) {                       // function call
            Node *n = new_node(N_CALL); snprintf(n->name, sizeof(n->name), "%s", nm);
            while (!at(T_RP)) {
                n->args[n->nargs++] = parse_assign();
                if (!eat(T_COMMA)) break;
            }
            expect(T_RP); n->type = ty_long(); return n;
        }
        Node *n = new_node(N_VAR); snprintf(n->name, sizeof(n->name), "%s", nm); return n;
    }
    die("expression expected"); return 0;
}

// postfix: primary ( [expr] | ++ | -- )*
static Node *parse_postfix(void) {
    Node *n = parse_primary();
    for (;;) {
        if (eat(T_LBRK)) {                     // a[i]  ->  *(a + i)
            Node *idx = parse_expr(); expect(T_RBRK);
            Node *add = new_node(N_BIN); add->op = T_PLUS; add->lhs = n; add->rhs = idx;
            Node *d = new_node(N_DEREF); d->lhs = add; n = d;
        } else if (eat(T_DOT)) {               // a.field
            Node *m = new_node(N_MEMBER); m->lhs = n;
            snprintf(m->name, sizeof(m->name), "%s", cur()->text); expect(T_ID); n = m;
        } else if (eat(T_ARROW)) {             // p->field  ==  (*p).field
            Node *d = new_node(N_DEREF); d->lhs = n;
            Node *m = new_node(N_MEMBER); m->lhs = d;
            snprintf(m->name, sizeof(m->name), "%s", cur()->text); expect(T_ID); n = m;
        } else if (at(T_INC) || at(T_DEC)) {
            Node *p = new_node(N_POST); p->op = cur()->kind; p->lhs = n; P++; n = p;
        } else break;
    }
    return n;
}

static Node *parse_unary(void) {
    if (eat(T_KSIZEOF)) {
        Node *n = new_node(N_SIZEOF);
        if (at(T_LP) && is_type_start(toks[P+1].kind)) { P++; n->type = parse_type(); expect(T_RP); }
        else n->lhs = parse_unary();
        return n;
    }
    if (eat(T_MINUS)) { Node *n = new_node(N_UNARY); n->op = T_MINUS; n->lhs = parse_unary(); return n; }
    if (eat(T_NOT))   { Node *n = new_node(N_UNARY); n->op = T_NOT;   n->lhs = parse_unary(); return n; }
    if (eat(T_STAR))  { Node *n = new_node(N_DEREF); n->lhs = parse_unary(); return n; }
    if (eat(T_AMP))   { Node *n = new_node(N_ADDR);  n->lhs = parse_unary(); return n; }
    if (at(T_INC) || at(T_DEC)) {            // prefix ++x / --x
        Node *n = new_node(N_PRE); n->op = cur()->kind; P++; n->lhs = parse_unary(); return n;
    }
    return parse_postfix();
}

static Node *bin(int op, Node *l, Node *r) {
    Node *n = new_node(N_BIN); n->op = op; n->lhs = l; n->rhs = r; return n;
}
static Node *parse_mul(void) {
    Node *n = parse_unary();
    while (at(T_STAR) || at(T_SLASH) || at(T_PERCENT)) { int op = cur()->kind; P++; n = bin(op, n, parse_unary()); }
    return n;
}
static Node *parse_add(void) {
    Node *n = parse_mul();
    while (at(T_PLUS) || at(T_MINUS)) { int op = cur()->kind; P++; n = bin(op, n, parse_mul()); }
    return n;
}
static Node *parse_rel(void) {
    Node *n = parse_add();
    while (at(T_LT) || at(T_GT) || at(T_LE) || at(T_GE)) { int op = cur()->kind; P++; n = bin(op, n, parse_add()); }
    return n;
}
static Node *parse_eq(void) {
    Node *n = parse_rel();
    while (at(T_EQ) || at(T_NE)) { int op = cur()->kind; P++; n = bin(op, n, parse_rel()); }
    return n;
}
static Node *parse_land(void) {
    Node *n = parse_eq();
    while (eat(T_ANDAND)) { Node *a = new_node(N_LOGAND); a->lhs = n; a->rhs = parse_eq(); n = a; }
    return n;
}
static Node *parse_lor(void) {
    Node *n = parse_land();
    while (eat(T_OROR)) { Node *a = new_node(N_LOGOR); a->lhs = n; a->rhs = parse_land(); n = a; }
    return n;
}
static Node *parse_ternary(void) {
    Node *n = parse_lor();
    if (eat(T_QUESTION)) {
        Node *t = new_node(N_TERNARY);
        t->cond = n; t->lhs = parse_expr(); expect(T_COLON); t->rhs = parse_ternary();
        return t;
    }
    return n;
}
static Node *parse_assign(void) {
    Node *n = parse_ternary();
    if (eat(T_ASSIGN)) {
        Node *a = new_node(N_ASSIGN); a->lhs = n; a->rhs = parse_assign(); return a;
    }
    // compound assignment  x op= y  ->  x = x op y   (lhs reused; safe for our inputs)
    int op = 0;
    if      (eat(T_PLUSEQ))    op = T_PLUS;
    else if (eat(T_MINUSEQ))   op = T_MINUS;
    else if (eat(T_STAREQ))    op = T_STAR;
    else if (eat(T_SLASHEQ))   op = T_SLASH;
    else if (eat(T_PERCENTEQ)) op = T_PERCENT;
    if (op) {
        Node *a = new_node(N_ASSIGN); a->lhs = n; a->rhs = bin(op, n, parse_assign()); return a;
    }
    return n;
}
static Node *parse_expr(void) { return parse_assign(); }

// a declaration inside a block: type declarator [= init] (, declarator [= init])* ;
static Node *parse_decl_stmt(void) {
    Type *base = parse_type_base_only();   // fwd-declared below
    Node *blk = new_node(N_BLOCK);
    if (eat(T_SEMI)) return blk;           // bare  struct Foo { ... };  (type only)
    for (;;) {
        Type *t = base;
        while (eat(T_STAR)) t = ptr_to(t);
        char nm[256]; snprintf(nm, sizeof(nm), "%s", cur()->text); expect(T_ID);
        if (eat(T_LBRK)) {                          // array: char buf[24];
            long len = cur()->ival; expect(T_NUM); expect(T_RBRK);
            Type *arr = calloc(1, sizeof(Type)); arr->kind = TY_PTR; arr->ptr = t;
            arr->is_array = 1; arr->arr_len = (int)len; t = arr;
        }
        Node *d = new_node(N_DECL); snprintf(d->name, sizeof(d->name), "%s", nm); d->type = t;
        if (eat(T_ASSIGN)) d->init = parse_assign();
        blk->body[blk->nbody++] = d;
        if (!eat(T_COMMA)) break;
    }
    expect(T_SEMI);
    return blk;
}
// parse just the base type (no trailing stars) — stars belong to each declarator
static Type *parse_type_base_only(void) {
    if (at(T_KSTRUCT) || at(T_KUNION)) return parse_struct_specifier();
    if      (eat(T_KINT))  return ty_int();
    if      (eat(T_KLONG)) return ty_long();
    if      (eat(T_KCHAR)) return ty_char();
    if      (eat(T_KVOID)) return ty_void();
    die("type expected"); return 0;
}

static Node *parse_block(void) {
    expect(T_LBRACE);
    Node *n = new_node(N_BLOCK);
    while (!at(T_RBRACE) && !at(T_EOF)) n->body[n->nbody++] = parse_stmt();
    expect(T_RBRACE);
    return n;
}

static Node *parse_stmt(void) {
    if (at(T_LBRACE)) return parse_block();
    if (eat(T_SEMI))  return new_node(N_EMPTY);
    if (eat(T_KIF)) {
        Node *n = new_node(N_IF);
        expect(T_LP); n->cond = parse_expr(); expect(T_RP);
        n->lhs = parse_stmt();
        if (eat(T_KELSE)) n->els = parse_stmt();
        return n;
    }
    if (eat(T_KWHILE)) {
        Node *n = new_node(N_WHILE);
        expect(T_LP); n->cond = parse_expr(); expect(T_RP);
        n->lhs = parse_stmt();
        return n;
    }
    if (eat(T_KFOR)) {
        Node *n = new_node(N_FOR);
        expect(T_LP);
        if (is_type_start(cur()->kind)) n->init = parse_decl_stmt();      // consumes ';'
        else if (!at(T_SEMI)) { Node *e = new_node(N_EXPR); e->lhs = parse_expr(); n->init = e; expect(T_SEMI); }
        else expect(T_SEMI);
        if (!at(T_SEMI)) n->cond = parse_expr();
        expect(T_SEMI);
        if (!at(T_RP)) n->rhs = parse_expr();                             // step
        expect(T_RP);
        n->lhs = parse_stmt();                                            // body
        return n;
    }
    if (eat(T_KDO)) {
        Node *n = new_node(N_DOWHILE);
        n->lhs = parse_stmt();
        expect(T_KWHILE); expect(T_LP); n->cond = parse_expr(); expect(T_RP); expect(T_SEMI);
        return n;
    }
    if (eat(T_KBREAK))    { expect(T_SEMI); return new_node(N_BREAK); }
    if (eat(T_KCONTINUE)) { expect(T_SEMI); return new_node(N_CONTINUE); }
    if (eat(T_KRETURN)) {
        Node *n = new_node(N_RETURN);
        if (!at(T_SEMI)) n->lhs = parse_expr();
        expect(T_SEMI);
        return n;
    }
    if (at(T_KASM)) {
        P++;
        expect(T_LP);
        if (!at(T_STR)) die("string expected in __asm__");
        Node *n = new_node(N_ASM); n->asmtext = cur()->str; P++;
        expect(T_RP); expect(T_SEMI);
        return n;
    }
    if (is_type_start(cur()->kind)) return parse_decl_stmt();
    Node *n = new_node(N_EXPR); n->lhs = parse_expr(); expect(T_SEMI);
    return n;
}

// ---- top level ----
typedef struct Func { char name[256]; Node *params[8]; int nparams; Type *ptype[8]; Node *body; struct Func *next; } Func;
static Func *funcs = NULL, *funcs_tail = NULL;

static void parse_toplevel(void) {
    Type *base = parse_type_base_only();
    if (eat(T_SEMI)) return;                           // bare  struct Foo { ... };
    Type *t = base;
    while (eat(T_STAR)) t = ptr_to(t);
    char nm[256]; snprintf(nm, sizeof(nm), "%s", cur()->text); expect(T_ID);

    if (eat(T_LP)) {                                   // function definition
        Func *fn = calloc(1, sizeof(Func)); snprintf(fn->name, sizeof(fn->name), "%s", nm);
        while (!at(T_RP)) {
            Type *pt = parse_type_base_only();
            while (eat(T_STAR)) pt = ptr_to(pt);
            if (pt->kind == TY_VOID && !at(T_ID)) break;   // (void)
            Node *pv = new_node(N_DECL);
            snprintf(pv->name, sizeof(pv->name), "%s", cur()->text); expect(T_ID);
            pv->type = pt;
            fn->params[fn->nparams] = pv; fn->ptype[fn->nparams] = pt; fn->nparams++;
            if (!eat(T_COMMA)) break;
        }
        expect(T_RP);
        fn->body = parse_block();
        if (!funcs) funcs = funcs_tail = fn; else { funcs_tail->next = fn; funcs_tail = fn; }
        return;
    }
    // global variable(s):  type name [= ...] (, ...) ;   (initialisers ignored -> .bss)
    for (;;) {
        if (eat(T_LBRK)) { long len = cur()->ival; expect(T_NUM); expect(T_RBRK);
            Type *arr = calloc(1, sizeof(Type)); arr->kind = TY_PTR; arr->ptr = t;
            arr->is_array = 1; arr->arr_len = (int)len; add_global(nm, arr); }
        else add_global(nm, t);
        if (eat(T_ASSIGN)) parse_assign();             // parsed & discarded (zero-init in bss)
        if (!eat(T_COMMA)) break;
        t = base; while (eat(T_STAR)) t = ptr_to(t);
        snprintf(nm, sizeof(nm), "%s", cur()->text); expect(T_ID);
    }
    expect(T_SEMI);
}

// =====================================================================
// 6. OFFSET ASSIGNMENT (per function)
// =====================================================================
static Sym *add_local(const char *name, Type *t) {
    Sym *s = calloc(1, sizeof(Sym)); snprintf(s->name, sizeof(s->name), "%s", name);
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
            if (!sym_find(locals, n->name)) add_local(n->name, n->type);
            if (n->init) collect_locals(n->init);
            break;
        case N_IF: case N_TERNARY:
                      collect_locals(n->cond); collect_locals(n->lhs); collect_locals(n->rhs); collect_locals(n->els); break;
        case N_WHILE: case N_DOWHILE:
                      collect_locals(n->lhs); collect_locals(n->cond); break;
        case N_FOR:   collect_locals(n->init); collect_locals(n->cond); collect_locals(n->rhs); collect_locals(n->lhs); break;
        case N_RETURN: case N_EXPR: case N_UNARY: case N_DEREF: case N_ADDR: case N_CAST: case N_POST: case N_PRE: case N_MEMBER:
                      collect_locals(n->lhs); break;
        case N_ASSIGN: case N_BIN: case N_LOGAND: case N_LOGOR:
                      collect_locals(n->lhs); collect_locals(n->rhs); break;
        case N_CALL:  for (int i = 0; i < n->nargs; i++) collect_locals(n->args[i]); break;
        default: break;
    }
}

// =====================================================================
// 7. CODE GENERATION
// =====================================================================
static int label_id = 0;
static const char *ARGREG[6] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };

static Type *gen_expr(Node *n);       // fwd
static void  gen_stmt(Node *n);

static Sym *lookup(const char *name) {
    Sym *s = sym_find(locals, name);
    if (!s) s = sym_find(globals, name);
    return s;
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
        if (!s) die("undeclared identifier");
        if (s->is_global) emit("    lea rax, [rip + %s]", n->name);
        else              emit("    lea rax, [rbp - %d]", s->offset);
        return s->type;
    }
    if (n->kind == N_DEREF) {                      // &*p  ==  p
        Type *t = gen_expr(n->lhs);
        return is_ptrish(t) ? t->ptr : ty_long();
    }
    if (n->kind == N_MEMBER) {                      // &(s.field)
        Type *st = gen_addr(n->lhs);               // rax = &struct
        Member *m = find_member(st, n->name);
        if (!m) die("no such struct member");
        if (m->offset) emit("    add rax, %d", m->offset);
        return m->type;
    }
    die("not an lvalue"); return 0;
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

static void gen_string(Node *n) {
    int id = label_id++;
    emit("    .section .rodata");
    fprintf(fout, ".LC%d: .string \"", id);
    for (int i = 0; i < n->slen; i++) {
        unsigned char ch = (unsigned char)n->str[i];
        switch (ch) {
            case '\n': fputs("\\n", fout); break;  case '\t': fputs("\\t", fout); break;
            case '\r': fputs("\\r", fout); break;  case '"':  fputs("\\\"", fout); break;
            case '\\': fputs("\\\\", fout); break;
            default: if (ch < 32 || ch > 126) fprintf(fout, "\\%03o", ch); else fputc(ch, fout);
        }
    }
    fputs("\"\n", fout);
    emit("    .section .text");
    emit("    lea rax, [rip + .LC%d]", id);
}

static Type *gen_expr(Node *n) {
    switch (n->kind) {
    case N_NUM:  emit("    mov rax, %ld", n->ival); return n->type ? n->type : ty_long();
    case N_STR:  gen_string(n); return n->type;
    case N_VAR: {
        Sym *s = lookup(n->name);
        if (!s) die("undeclared identifier");
        // arrays and structs are used by-address (decay); scalars are loaded
        if (s->type->is_array || s->type->kind == TY_STRUCT) { gen_addr(n); return s->type; }
        gen_addr(n); load_rax(ty_size(s->type));
        return s->type;
    }
    case N_MEMBER: {
        Type *mt = gen_addr(n);                    // rax = &member
        if (mt->is_array || mt->kind == TY_STRUCT) return mt;   // decay
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
        Type *lt = gen_addr(n->lhs);
        emit("    push rax");
        gen_expr(n->rhs);
        emit("    pop rcx");
        store_rcx_rax(ty_size(lt));
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
        if (n->op == T_MINUS) emit("    neg rax");
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
        for (int i = 0; i < n->nargs; i++) { gen_expr(n->args[i]); emit("    push rax"); }
        for (int i = n->nargs - 1; i >= 0; i--) emit("    pop %s", ARGREG[i]);
        emit("    xor eax, eax");                  // variadic-safe; harmless otherwise
        emit("    call %s", n->name);
        return ty_long();
    }
    case N_BIN: {
        Type *lt = gen_expr(n->lhs); emit("    push rax");
        Type *rt = gen_expr(n->rhs); emit("    mov rcx, rax"); emit("    pop rax");
        // rax = left, rcx = right
        int op = n->op;
        if (op == T_PLUS || op == T_MINUS) {
            if (is_ptrish(lt) && !is_ptrish(rt)) {
                int s = elem_size(lt); if (s > 1) emit("    imul rcx, %d", s);
            } else if (!is_ptrish(lt) && is_ptrish(rt) && op == T_PLUS) {
                int s = elem_size(rt); if (s > 1) emit("    imul rax, %d", s);
            }
            emit(op == T_PLUS ? "    add rax, rcx" : "    sub rax, rcx");
            return is_ptrish(lt) ? lt : (is_ptrish(rt) ? rt : ty_long());
        }
        switch (op) {
        case T_STAR:    emit("    imul rax, rcx"); break;
        case T_SLASH:   emit("    cqo"); emit("    idiv rcx"); break;
        case T_PERCENT: emit("    cqo"); emit("    idiv rcx"); emit("    mov rax, rdx"); break;
        case T_LT: case T_GT: case T_LE: case T_GE: case T_EQ: case T_NE: {
            emit("    cmp rax, rcx");
            const char *cc = op==T_LT?"setl":op==T_GT?"setg":op==T_LE?"setle":
                             op==T_GE?"setge":op==T_EQ?"sete":"setne";
            emit("    %s al", cc); emit("    movzx rax, al");
            break;
        }
        default: die("bad binary operator");
        }
        return ty_long();
    }
    default: die("cannot generate expression"); return 0;
    }
}

static void gen_asm(Node *n) {
    // split decoded asm text on newlines and ';' — emit each instruction line
    const char *p = n->asmtext;
    char line[512]; int i = 0;
    for (;; p++) {
        char c = *p;
        if (c == '\n' || c == ';' || c == 0) {
            line[i] = 0;
            // trim leading spaces
            char *q = line; while (*q == ' ' || *q == '\t') q++;
            if (*q) emit("    %s", q);
            i = 0;
            if (c == 0) break;
        } else if (i < 511) line[i++] = c;
    }
}

// break/continue target stack
static int brk_lbl[64], cont_lbl[64], loop_sp = 0;
static void loop_push(int b, int c) { brk_lbl[loop_sp] = b; cont_lbl[loop_sp] = c; loop_sp++; }
static void loop_pop(void) { loop_sp--; }

static void gen_stmt(Node *n) {
    switch (n->kind) {
    case N_BLOCK: for (int i = 0; i < n->nbody; i++) gen_stmt(n->body[i]); break;
    case N_EMPTY: break;
    case N_DECL:
        if (n->init) {
            Sym *s = lookup(n->name);
            emit("    lea rax, [rbp - %d]", s->offset);
            emit("    push rax");
            gen_expr(n->init);
            emit("    pop rcx");
            store_rcx_rax(ty_size(s->type));
        }
        break;
    case N_EXPR: gen_expr(n->lhs); break;
    case N_RETURN:
        if (n->lhs) gen_expr(n->lhs);
        emit("    leave"); emit("    ret");
        break;
    case N_IF: {
        int els = label_id++, end = label_id++;
        gen_expr(n->cond); emit("    test rax, rax"); emit("    jz .L%d", els);
        gen_stmt(n->lhs);  emit("    jmp .L%d", end);
        emit(".L%d:", els); if (n->els) gen_stmt(n->els);
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
        if (n->rhs) gen_expr(n->rhs);                 // step
        emit("    jmp .L%d", top);
        emit(".L%d:", end);
        break;
    }
    case N_BREAK:
        if (loop_sp == 0) die("break outside loop");
        emit("    jmp .L%d", brk_lbl[loop_sp-1]);
        break;
    case N_CONTINUE:
        if (loop_sp == 0) die("continue outside loop");
        emit("    jmp .L%d", cont_lbl[loop_sp-1]);
        break;
    case N_ASM: gen_asm(n); break;
    default: gen_expr(n); break;                    // expression used as statement
    }
}

static void gen_func(Func *fn) {
    locals = NULL; frame_size = 0;
    // params first (so they get the lowest offsets, in declared order)
    for (int i = 0; i < fn->nparams; i++) add_local(fn->params[i]->name, fn->ptype[i]);
    collect_locals(fn->body);
    int fs = (frame_size + 15) & ~15;

    emit("%s:", fn->name);
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
    gen_stmt(fn->body);
    emit("    leave"); emit("    ret");             // safety epilogue
}

// =====================================================================
// 8. MAIN
// =====================================================================
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: %s <input.c> <output.s>\n", argv[0]); return 1; }

    preprocess(argv[1]);
    SRC[SRC_LEN] = 0;
    lex();

    while (!at(T_EOF)) parse_toplevel();

    fout = fopen(argv[2], "w");
    if (!fout) { perror("fopen"); return 1; }

    emit(".intel_syntax noprefix");
    emit("    .section .text");
    emit("    .globl _start");
    emit("    .globl main");

    // freestanding entry point: run main, then exit(rax)
    emit("_start:");
    emit("    call main");
    emit("    mov rdi, rax");
    emit("    mov rax, 60");
    emit("    syscall");

    for (Func *f = funcs; f; f = f->next) gen_func(f);

    // globals -> .bss
    emit("    .section .bss");
    emit("    .align 8");
    for (Sym *g = globals; g; g = g->next) {
        int sz = ty_size(g->type); if (sz < 1) sz = 8;
        emit("%s: .zero %d", g->name, sz);
    }

    fclose(fout);
    printf("Compiled %s -> %s\n", argv[1], argv[2]);
    return 0;
}
