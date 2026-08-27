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

// A macro is either object-like (#define PI 3) or function-like
// (#define MAX(a,b) ((a)>(b)?(a):(b))).  For function-like macros we keep
// the parameter names so expand_line() can substitute call arguments.
typedef struct {
    char name[64]; char value[512];
    int  is_func; char params[8][64]; int nparams;
} Macro;
#define MAX_MACROS 1024
static Macro macros[MAX_MACROS];
static int   macro_cnt = 0;

static Macro *macro_find(const char *name) {
    for (int i = 0; i < macro_cnt; i++)
        if (!strcmp(macros[i].name, name)) return &macros[i];
    return NULL;
}
// Find-or-create a macro slot, reset to a clean object-like state.
static Macro *macro_intern(const char *name) {
    Macro *m = macro_find(name);
    if (!m) { m = &macros[macro_cnt++]; }
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->value[0] = 0; m->is_func = 0; m->nparams = 0;
    return m;
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
    for (int vi = 0; m->value[vi]; ) {
        char vc = m->value[vi];
        if (isalpha((unsigned char)vc) || vc == '_') {
            char w[64]; int k = 0;
            while (m->value[vi] && (isalnum((unsigned char)m->value[vi]) || m->value[vi] == '_') && k < 63)
                w[k++] = m->value[vi++];
            w[k] = 0;
            int pi = -1;
            for (int a = 0; a < m->nparams; a++) if (!strcmp(w, m->params[a])) { pi = a; break; }
            if (pi >= 0 && pi < argc) { for (const char *s = argv[pi]; *s; s++) PUT(*s); }
            else                      { for (int t = 0; w[t]; t++) PUT(w[t]); }
        } else {
            PUT(vc); vi++;
        }
    }
    #undef PUT
    return j;
}

// Expand object-like and simple function-like macros in one logical line.
static void expand_line(const char *in) {
    char work[8192];
    snprintf(work, sizeof(work), "%s", in);
    for (int pass = 0; pass < 8; pass++) {
        char out[8192]; int j = 0, changed = 0;
        #define OPUT(ch) do { if (j < (int)sizeof(out) - 1) out[j++] = (ch); } while (0)
        for (int i = 0; work[i]; ) {
            char c = work[i];
            if (c == '"' || c == '\'') {          // copy string/char literal verbatim
                char q = c; OPUT(work[i++]);
                while (work[i] && work[i] != q) {
                    if (work[i] == '\\' && work[i+1]) OPUT(work[i++]);
                    OPUT(work[i++]);
                }
                if (work[i]) OPUT(work[i++]);
            } else if (isalpha((unsigned char)c) || c == '_') {
                char word[64]; int k = 0;
                while (work[i] && (isalnum((unsigned char)work[i]) || work[i] == '_') && k < 63)
                    word[k++] = work[i++];
                word[k] = 0;
                Macro *m = macro_find(word);
                if (m && m->is_func) {
                    int t = i; while (work[t] == ' ' || work[t] == '\t') t++;
                    if (work[t] == '(') {          // it's a macro invocation
                        t++;                       // past '('
                        char argv[8][512]; int argc = 0, ci = 0; argv[0][0] = 0;
                        int depth = 1;
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
                            char *s = argv[a]; int st = 0, en = (int)strlen(s);
                            while (s[st] == ' ' || s[st] == '\t') st++;
                            while (en > st && (s[en-1] == ' ' || s[en-1] == '\t')) en--;
                            memmove(s, s + st, en - st); s[en - st] = 0;
                        }
                        // empty call MACRO()  ->  zero arguments
                        if (argc == 1 && argv[0][0] == 0 && m->nparams == 0) argc = 0;
                        j = subst_macro_body(m, argv, argc, out, j, (int)sizeof(out));
                        i = t;                     // consume through ')'
                        changed = 1;
                    } else {                       // name not followed by '(' -> literal
                        // `wi`, not `q`: a `char q` is already live in this
                        // function for the quote character, and locals here
                        // are one slot per NAME per function
                        for (int wi = 0; word[wi]; wi++) OPUT(word[wi]);
                    }
                } else if (m) {                    // object-like macro
                    for (const char *p = m->value; *p; p++) OPUT(*p);
                    changed = 1;
                } else {
                    for (int t = 0; word[t]; t++) OPUT(word[t]);
                }
            } else {
                OPUT(work[i++]);
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
                    Macro *m = macro_intern(nm);
                    // function-like macro: '(' immediately after name (no space)
                    if (*p == '(') {
                        m->is_func = 1; p++;
                        while (*p && *p != ')') {
                            while (*p == ' ' || *p == '\t' || *p == ',') p++;
                            int ai = 0;
                            while (isalnum((unsigned char)*p) || *p == '_') {
                                if (ai < 63 && m->nparams < 8) m->params[m->nparams][ai++] = *p;
                                p++;
                            }
                            if (ai > 0) { if (m->nparams < 8) { m->params[m->nparams][ai] = 0; m->nparams++; } }
                            else if (*p && *p != ')' && *p != ' ' && *p != '\t' && *p != ',') p++; // progress guard
                        }
                        if (*p == ')') p++;
                    }
                    while (*p == ' ' || *p == '\t') p++;
                    char val[512]; int v = 0;
                    while (*p && v < 511) val[v++] = *p++;
                    while (v > 0 && (val[v-1] == ' ' || val[v-1] == '\t' || val[v-1] == '\r')) v--;
                    val[v] = 0;
                    snprintf(m->value, sizeof(m->value), "%s", val);
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
    T_BITOR, T_BITXOR, T_BITNOT, T_SHL, T_SHR,
    T_INC, T_DEC,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_LP, T_RP, T_LBRK, T_RBRK, T_LBRACE, T_RBRACE,
    T_SEMI, T_COMMA, T_QUESTION, T_COLON, T_DOT, T_ARROW, T_ELLIPSIS,
    T_KINT, T_KLONG, T_KCHAR, T_KVOID,
    T_KIF, T_KELSE, T_KWHILE, T_KRETURN, T_KASM,
    T_KFOR, T_KDO, T_KBREAK, T_KCONTINUE,
    T_KSTRUCT, T_KUNION, T_KSIZEOF,
    T_KSWITCH, T_KCASE, T_KDEFAULT,
    T_KTYPEDEF, T_KENUM,
    T_KGOTO, T_KSTATIC,
    T_EOF
};

typedef struct {
    int   kind;
    long  ival;          // T_NUM / T_CHAR
    char  text[256];     // T_ID
    char *str;           // T_STR (decoded bytes)
    int   slen;          // T_STR length
    int   pos;           // offset into SRC, for error messages
} Token;

#define MAX_TOK 60000
static Token toks[MAX_TOK];
static int   ntok = 0;

static int  tok_pos = 0;                  // where the token being lexed started
static void add_tok(int kind) { toks[ntok].kind = kind; toks[ntok].pos = tok_pos; ntok++; }

// Report an error against a token rather than against nothing. The line number
// is a line of the PREPROCESSED buffer, so the text of the line is printed too
// -- after #include expansion the number on its own would send you to the
// wrong file.
static void die_at(int idx, const char *msg) {
    int pos = (idx >= 0 && idx < ntok) ? toks[idx].pos : 0;
    if (pos > SRC_LEN) pos = SRC_LEN;
    int line = 1, ls = 0;
    for (int i = 0; i < pos; i++) if (SRC[i] == '\n') { line++; ls = i + 1; }
    int le = ls; while (le < SRC_LEN && SRC[le] != '\n') le++;
    while (ls < le && (SRC[ls] == ' ' || SRC[ls] == '\t')) ls++;
    fprintf(stderr, "nano_cc: error: %s\n", msg);
    fprintf(stderr, "  line %d after preprocessing:  %.*s\n", line, le - ls, SRC + ls);
    exit(1);
}

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
        tok_pos = sp;
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
            // integer suffixes: 100L, 1U, 5UL, 7ll. Everything is 64-bit
            // and signed here, so they carry no information -- but they are
            // legal C and a lexer that stops at the digits reports a syntax
            // error two tokens later, pointing at the wrong thing.
            while (sp < SRC_LEN && (SRC[sp]=='u'||SRC[sp]=='U'||SRC[sp]=='l'||SRC[sp]=='L')) sp++;
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
            else if (!strcmp(buf, "switch"))   k = T_KSWITCH;
            else if (!strcmp(buf, "case"))     k = T_KCASE;
            else if (!strcmp(buf, "default"))  k = T_KDEFAULT;
            else if (!strcmp(buf, "typedef"))  k = T_KTYPEDEF;
            else if (!strcmp(buf, "enum"))     k = T_KENUM;
            else if (!strcmp(buf, "goto"))     k = T_KGOTO;
            // `static` used to be dropped with the other qualifiers. At file
            // scope that is harmless -- it only changes linkage. Inside a
            // function it changes STORAGE DURATION, and dropping it silently
            // put the object on the stack, so `return &t;` handed back a
            // pointer into a dead frame.
            else if (!strcmp(buf, "static"))   k = T_KSTATIC;
            else if (!strcmp(buf, "__asm__") || !strcmp(buf, "asm")) k = T_KASM;
            else if (!strcmp(buf, "const") || !strcmp(buf, "unsigned") ||
                     !strcmp(buf, "signed") ||
                     !strcmp(buf, "inline") || !strcmp(buf, "register") ||
                     !strcmp(buf, "volatile") || !strcmp(buf, "extern")) continue;   // qualifier: drop
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
        if (c == '.' && c2 == '.' && sp + 2 < SRC_LEN && SRC[sp+2] == '.') {
            sp += 3; add_tok(T_ELLIPSIS); continue;      // '...' varargs marker
        }
        #define TWO(a,b,K)  if (c==a && c2==b) { sp += 2; add_tok(K); goto next; }
        TWO('=','=',T_EQ) TWO('!','=',T_NE) TWO('<','=',T_LE) TWO('>','=',T_GE)
        TWO('&','&',T_ANDAND) TWO('|','|',T_OROR)
        TWO('<','<',T_SHL) TWO('>','>',T_SHR)
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
            case '|': add_tok(T_BITOR);  break;  case '^': add_tok(T_BITXOR); break;
            case '~': add_tok(T_BITNOT); break;
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
// An array aligns like its element, not like its total size.
static int ty_align(Type *t) {
    if (t->is_array) return ty_align(t->ptr);
    return t->kind == TY_CHAR ? 1 : 8;
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
    N_MEMBER, N_SIZEOF,
    N_SWITCH, N_CASE, N_DEFAULT,
    N_GOTO, N_LABEL,
    N_INIT                      // brace initializer list; elements in body[]
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
    // N_BLOCK statements / N_INIT elements. A growable list, not a fixed
    // array: a data table like a bitmap font is thousands of initialisers, and
    // a fixed 512 also made every Node 4 KB whether it needed the room or not.
    Node **body; int nbody; int bodycap;
    char *asmtext;                // N_ASM decoded text
    int   tok;                    // token index, for error messages
};

static int P = 0;                        // token cursor (declared early: nodes record it)

// The token index is captured here so a codegen error -- which happens long
// after parsing -- can still point at a line of source instead of at nothing.
static Node *new_node(int k) { Node *n = calloc(1, sizeof(Node)); n->kind = k; n->ival = -1; n->tok = P; return n; }
static void die_node(Node *n, const char *msg) { die_at(n ? n->tok : -1, msg); }

static void node_push(Node *n, Node *child) {
    if (n->nbody >= n->bodycap) {
        int cap = n->bodycap ? n->bodycap * 2 : 8;
        Node **nb = calloc((size_t)cap, sizeof(Node *));
        for (int i = 0; i < n->nbody; i++) nb[i] = n->body[i];
        n->body = nb; n->bodycap = cap;
    }
    n->body[n->nbody++] = child;
}

// ---- symbols ----
// A global's initialiser is flattened at compile time into a byte image plus a
// list of "put this symbol's address here" fixups, because an initialiser like
//   char *ARGREG[6] = { "rdi", "rsi", ... }
// is part bytes and part relocations and the two have to be emitted in order.
typedef struct Reloc { int offset; char label[64]; struct Reloc *next; } Reloc;

typedef struct Sym { char name[256]; Type *type; int is_global; int offset;
                     unsigned char *initdata; int initlen; Reloc *relocs;
                     struct Sym *next; } Sym;
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
static Token *cur(void) { return &toks[P]; }
static int  at(int k)   { return toks[P].kind == k; }
static int  eat(int k)  { if (toks[P].kind == k) { P++; return 1; } return 0; }
static void expect(int k) {
    if (!eat(k)) {
        char m[80]; snprintf(m, sizeof m, "parse error: expected token kind %d, found kind %d", k, toks[P].kind);
        die_at(P, m);
    }
}

// ---- typedef name table ----
typedef struct TDef { char name[256]; Type *type; struct TDef *next; } TDef;
static TDef *typedefs = NULL;
static Type *typedef_find(const char *name) {
    for (TDef *t = typedefs; t; t = t->next) if (!strcmp(t->name, name)) return t->type;
    return NULL;
}
static void typedef_add(const char *name, Type *t) {
    TDef *e = calloc(1, sizeof(TDef)); snprintf(e->name, sizeof(e->name), "%s", name);
    e->type = t; e->next = typedefs; typedefs = e;
}

// ---- static locals ----
// A function-scope `static` is a GLOBAL that only one function can name. It is
// given a global of its own, called `<function>.<name>` -- the dot cannot occur
// in a C identifier, so the generated name can never collide with a real one --
// and uses of the name inside that function are rewritten to it while parsing.
// `global` is sized like Sym.name: that is the real limit on a symbol name here.
typedef struct SAlias { char name[256]; char global[256]; struct SAlias *next; } SAlias;
static SAlias *saliases = NULL;
static char cur_fn[256] = "";
static const char *salias_find(const char *name) {
    for (SAlias *a = saliases; a; a = a->next) if (!strcmp(a->name, name)) return a->global;
    return NULL;
}
static void salias_add(const char *name, const char *global) {
    SAlias *a = calloc(1, sizeof(SAlias));
    snprintf(a->name, sizeof(a->name), "%s", name);
    snprintf(a->global, sizeof(a->global), "%s", global);
    a->next = saliases; saliases = a;
}

// ---- enum constant table ----
// An enumerator is a compile-time integer constant, not a variable, so it is
// resolved in the parser and never reaches codegen as a name.
typedef struct ECon { char name[256]; long val; struct ECon *next; } ECon;
static ECon *enum_consts = NULL;
static int enum_find(const char *name, long *out) {
    for (ECon *e = enum_consts; e; e = e->next)
        if (!strcmp(e->name, name)) { *out = e->val; return 1; }
    return 0;
}
static void enum_add(const char *name, long v) {
    ECon *e = calloc(1, sizeof(ECon)); snprintf(e->name, sizeof(e->name), "%s", name);
    e->val = v; e->next = enum_consts; enum_consts = e;
}

// Does the token at `idx` begin a type? This has to look at the token itself
// and not just its kind: a typedef name is an ordinary T_ID until the typedef
// that names it has been parsed, and only the table can tell the difference.
static int is_type_start_at(int idx) {
    int k = toks[idx].kind;
    if (k == T_KINT || k == T_KLONG || k == T_KCHAR || k == T_KVOID ||
        k == T_KSTRUCT || k == T_KUNION || k == T_KENUM || k == T_KTYPEDEF ||
        k == T_KSTATIC) return 1;
    return k == T_ID && typedef_find(toks[idx].text) != NULL;
}
static int is_type_start(void) { return is_type_start_at(P); }

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

// ---- function return types ----
// Every call used to be assumed to return `long`, which is why `cur()->text`
// could not work: the member lookup had nothing to look in. Definitions AND
// prototypes both register here, so a prototype is enough to type a call.
typedef struct FnSig { char name[256]; Type *ret; struct FnSig *next; } FnSig;
static FnSig *fnsigs = NULL;
static Type *fnsig_find(const char *name) {
    for (FnSig *f = fnsigs; f; f = f->next) if (!strcmp(f->name, name)) return f->ret;
    return NULL;
}
static void fnsig_add(const char *name, Type *ret) {
    if (fnsig_find(name)) return;               // first declaration wins
    FnSig *f = calloc(1, sizeof(FnSig)); snprintf(f->name, sizeof(f->name), "%s", name);
    f->ret = ret; f->next = fnsigs; fnsigs = f;
}

static Node *parse_expr(void);
static Node *parse_assign(void);
static Node *parse_ternary(void);
static Node *parse_unary(void);
static Node *parse_stmt(void);
static Type *parse_type_base_only(void);
static Type *parse_array_suffix(Type *base);
static void  parse_typedef(void);
static void  global_init(Sym *g, Node *init);
static Sym  *add_global(const char *name, Type *t);
static long  eval_const(Node *n);

// A constant expression: the conditional-expression grammar, folded now.
// Not parse_assign -- `=` is not a constant operator -- and not parse_expr,
// which would swallow the comma separating two enumerators or two array
// dimensions.
static long parse_const_expr(void) { return eval_const(parse_ternary()); }

// enum specifier:  enum [tag] [ { NAME [= const] (, NAME [= const])* [,] } ]
// Every enum is plain `int` here, so the tag carries no type information and
// is accepted but not recorded. What matters is the enumerators.
static Type *parse_enum_specifier(void) {
    expect(T_KENUM);
    if (at(T_ID)) P++;                              // tag
    if (eat(T_LBRACE)) {
        long next = 0;                              // C: values auto-increment from 0
        while (!at(T_RBRACE) && !at(T_EOF)) {
            char nm[256]; snprintf(nm, sizeof(nm), "%s", cur()->text); expect(T_ID);
            if (eat(T_ASSIGN)) next = parse_const_expr();
            enum_add(nm, next++);
            if (!eat(T_COMMA)) break;               // a trailing comma is legal
        }
        expect(T_RBRACE);
    }
    return ty_int();
}

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
                mt = parse_array_suffix(mt);       // buf[8], grid[2][2], ...
                int msz = ty_size(mt);
                // An array aligns like its ELEMENT. Using its total size gave
                // char buf[3] an alignment of 3, which is not a power of two
                // and made the mask in the line below round to nonsense.
                int align = ty_align(mt);
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
        if (is_type_start()) {
            Type *t = parse_type();
            expect(T_RP);
            // A cast binds to a UNARY-expression, not to whatever follows.
            // With parse_assign here, `(char *)b + 40` parsed as
            // `(char *)(b + 40)` -- so the addition scaled by sizeof(*b)
            // instead of by one, and the result was 40 times too far along.
            // It only ever looked correct when the element size happened to
            // be 0 or 1.
            Node *n = new_node(N_CAST); n->type = t; n->lhs = parse_unary();
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
        long ev;
        if (enum_find(nm, &ev)) {          // an enumerator folds to its value
            Node *n = new_node(N_NUM); n->ival = ev; n->type = ty_int(); return n;
        }
        const char *sa = salias_find(nm);  // a static local lives under its own name
        Node *n = new_node(N_VAR);
        snprintf(n->name, sizeof(n->name), "%s", sa ? sa : nm);
        return n;
    }
    die_at(P, "expression expected"); return 0;
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
        if (at(T_LP) && is_type_start_at(P + 1)) { P++; n->type = parse_type(); expect(T_RP); }
        else n->lhs = parse_unary();
        return n;
    }
    if (eat(T_MINUS)) { Node *n = new_node(N_UNARY); n->op = T_MINUS;  n->lhs = parse_unary(); return n; }
    if (eat(T_NOT))   { Node *n = new_node(N_UNARY); n->op = T_NOT;    n->lhs = parse_unary(); return n; }
    if (eat(T_BITNOT)){ Node *n = new_node(N_UNARY); n->op = T_BITNOT; n->lhs = parse_unary(); return n; }
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
// C precedence:  <<  >>  bind tighter than the relational operators.
static Node *parse_shift(void) {
    Node *n = parse_add();
    while (at(T_SHL) || at(T_SHR)) { int op = cur()->kind; P++; n = bin(op, n, parse_add()); }
    return n;
}
static Node *parse_rel(void) {
    Node *n = parse_shift();
    while (at(T_LT) || at(T_GT) || at(T_LE) || at(T_GE)) { int op = cur()->kind; P++; n = bin(op, n, parse_shift()); }
    return n;
}
static Node *parse_eq(void) {
    Node *n = parse_rel();
    while (at(T_EQ) || at(T_NE)) { int op = cur()->kind; P++; n = bin(op, n, parse_rel()); }
    return n;
}
// Bitwise AND / XOR / OR sit between equality and logical-AND, in that order.
static Node *parse_band(void) {
    Node *n = parse_eq();
    while (at(T_AMP)) { P++; n = bin(T_AMP, n, parse_eq()); }
    return n;
}
static Node *parse_bxor(void) {
    Node *n = parse_band();
    while (at(T_BITXOR)) { P++; n = bin(T_BITXOR, n, parse_band()); }
    return n;
}
static Node *parse_bor(void) {
    Node *n = parse_bxor();
    while (at(T_BITOR)) { P++; n = bin(T_BITOR, n, parse_bxor()); }
    return n;
}
static Node *parse_land(void) {
    Node *n = parse_bor();
    while (eat(T_ANDAND)) { Node *a = new_node(N_LOGAND); a->lhs = n; a->rhs = parse_bor(); n = a; }
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

// An initialiser is either a brace list (possibly nested, possibly with a
// trailing comma) or an ordinary assignment-expression.
static Node *parse_initializer(void) {
    if (eat(T_LBRACE)) {
        Node *n = new_node(N_INIT);
        while (!at(T_RBRACE) && !at(T_EOF)) {
            node_push(n, parse_initializer());
            if (!eat(T_COMMA)) break;
        }
        expect(T_RBRACE);
        return n;
    }
    return parse_assign();
}

// A declarator's array suffixes: `[]`, `[3]`, `[2][3]`, ... Built innermost
// first, so `long m[2][3]` is array-2-of(array-3-of long) and ty_size falls
// out of the recursion. Only the outermost length may be left open.
static Type *parse_array_suffix(Type *base) {
    int dims[8], nd = 0;
    while (eat(T_LBRK)) {
        long len = -1;                          // -1 = infer from the initialiser
        // Any constant expression, so `char buf[MAXLEN]` with an enumerator
        // MAXLEN and `long t[2 * NSLOT]` both size correctly.
        if (!at(T_RBRK)) len = parse_const_expr();
        expect(T_RBRK);
        if (nd >= (int)(sizeof dims / sizeof dims[0])) die("too many array dimensions");
        dims[nd++] = (int)len;
    }
    for (int i = nd - 1; i >= 0; i--) {
        if (dims[i] < 0 && i != 0) die("only the first array dimension may be omitted");
        Type *arr = calloc(1, sizeof(Type));
        arr->kind = TY_PTR; arr->ptr = base;
        arr->is_array = 1; arr->arr_len = dims[i];
        base = arr;
    }
    return base;
}

// How many elements an initialiser supplies, for `int t[] = { ... }`.
static int init_extent(Node *init, Type *elem) {
    if (!init) return 0;
    if (init->kind == N_INIT) return init->nbody;
    if (init->kind == N_STR && elem && elem->kind == TY_CHAR) return init->slen + 1;
    return 1;
}

// `int t[] = {1,2,3}` / `char s[] = "hi"` — fill in the length the declarator
// left open. Only the outermost dimension can be inferred, as in C.
static void infer_array_len(Type *t, Node *init) {
    if (t && t->is_array && t->arr_len < 0) {
        t->arr_len = init_extent(init, t->ptr);
        if (t->arr_len == 0) die("array needs an explicit size or a non-empty initialiser");
    }
}

// a declaration inside a block: type declarator [= init] (, declarator [= init])* ;
static Node *parse_decl_stmt(void) {
    if (at(T_KTYPEDEF)) {                  // block-scope typedef: no code, no storage
        parse_typedef();
        return new_node(N_BLOCK);
    }
    int is_static = eat(T_KSTATIC);
    Type *base = parse_type_base_only();   // fwd-declared below
    Node *blk = new_node(N_BLOCK);

    if (is_static) {
        // Static storage, not automatic: one object for the whole program,
        // initialised once at load time from a constant expression. It becomes
        // a global under a private name and emits no code here at all.
        for (;;) {
            Type *t = base;
            while (eat(T_STAR)) t = ptr_to(t);
            char nm[256]; snprintf(nm, sizeof(nm), "%s", cur()->text); expect(T_ID);
            t = parse_array_suffix(t);
            // Half the budget each, so a pathologically long function name
            // cannot push the variable name out of the generated symbol.
            char gname[256];
            snprintf(gname, sizeof gname, "%.120s.%.120s",
                     cur_fn[0] ? cur_fn : "_file", nm);
            Sym *g = add_global(gname, t);
            if (eat(T_ASSIGN)) {
                Node *init = parse_initializer();
                infer_array_len(t, init);
                global_init(g, init);
            }
            salias_add(nm, gname);
            if (!eat(T_COMMA)) break;
        }
        expect(T_SEMI);
        return blk;
    }
    if (eat(T_SEMI)) return blk;           // bare  struct Foo { ... };  (type only)
    for (;;) {
        Type *t = base;
        while (eat(T_STAR)) t = ptr_to(t);
        char nm[256]; snprintf(nm, sizeof(nm), "%s", cur()->text); expect(T_ID);
        t = parse_array_suffix(t);                  // char buf[24]; buf[]; m[2][3];
        Node *d = new_node(N_DECL); snprintf(d->name, sizeof(d->name), "%s", nm); d->type = t;
        if (eat(T_ASSIGN)) d->init = parse_initializer();
        infer_array_len(t, d->init);
        node_push(blk, d);
        if (!eat(T_COMMA)) break;
    }
    expect(T_SEMI);
    return blk;
}
// parse just the base type (no trailing stars) — stars belong to each declarator
static Type *parse_type_base_only(void) {
    if (at(T_KSTRUCT) || at(T_KUNION)) return parse_struct_specifier();
    if (at(T_KENUM))       return parse_enum_specifier();
    if      (eat(T_KINT))  return ty_int();
    if      (eat(T_KLONG)) return ty_long();
    if      (eat(T_KCHAR)) return ty_char();
    if      (eat(T_KVOID)) return ty_void();
    if (at(T_ID)) {                        // a name introduced by typedef
        Type *td = typedef_find(cur()->text);
        if (td) { P++; return td; }
    }
    die_at(P, "type expected"); return 0;
}

// typedef <base> <declarator> (, <declarator>)* ;
//
// The type is stored by pointer, which is what makes the self-referential
// idiom work:  `typedef struct Type Type;` registers the *incomplete* struct,
// and the later `struct Type { ... }` fills in that same object, so the
// typedef name ends up naming the completed type without any second pass.
static void parse_typedef(void) {
    expect(T_KTYPEDEF);
    Type *base = parse_type_base_only();
    if (eat(T_SEMI)) return;               // `typedef enum { A, B };` -- no new name
    for (;;) {
        Type *t = base;
        while (eat(T_STAR)) t = ptr_to(t);
        char nm[256]; snprintf(nm, sizeof(nm), "%s", cur()->text); expect(T_ID);
        t = parse_array_suffix(t);         // typedef char line[80];
        typedef_add(nm, t);
        if (!eat(T_COMMA)) break;
    }
    expect(T_SEMI);
}

static Node *parse_block(void) {
    expect(T_LBRACE);
    Node *n = new_node(N_BLOCK);
    while (!at(T_RBRACE) && !at(T_EOF)) node_push(n, parse_stmt());
    expect(T_RBRACE);
    return n;
}

static Node *parse_stmt(void) {
    if (at(T_LBRACE)) return parse_block();
    if (eat(T_SEMI))  return new_node(N_EMPTY);
    if (eat(T_KGOTO)) {
        Node *n = new_node(N_GOTO);
        snprintf(n->name, sizeof(n->name), "%s", cur()->text); expect(T_ID);
        expect(T_SEMI);
        return n;
    }
    // A label: an identifier followed directly by a colon. `case X:` and
    // `default:` are their own keywords, and a ternary can never put a colon
    // straight after the first token of a statement, so this is unambiguous.
    if (at(T_ID) && toks[P+1].kind == T_COLON) {
        Node *n = new_node(N_LABEL);
        snprintf(n->name, sizeof(n->name), "%s", cur()->text); P++; P++;
        n->lhs = parse_stmt();          // C requires a statement after a label
        return n;
    }
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
    if (eat(T_KSWITCH)) {
        Node *n = new_node(N_SWITCH);
        expect(T_LP); n->cond = parse_expr(); expect(T_RP);
        n->lhs = parse_stmt();
        return n;
    }
    if (eat(T_KCASE)) {
        Node *n = new_node(N_CASE);
        n->lhs = parse_expr();
        expect(T_COLON);
        return n;
    }
    if (eat(T_KDEFAULT)) {
        Node *n = new_node(N_DEFAULT);
        expect(T_COLON);
        return n;
    }
    if (eat(T_KFOR)) {
        Node *n = new_node(N_FOR);
        expect(T_LP);
        if (is_type_start()) n->init = parse_decl_stmt();      // consumes ';'
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
    if (is_type_start()) return parse_decl_stmt();
    Node *n = new_node(N_EXPR); n->lhs = parse_expr(); expect(T_SEMI);
    return n;
}

// ---- top level ----
typedef struct Func { char name[256]; Node *params[8]; int nparams; Type *ptype[8];
                      int is_variadic; Node *body; struct Func *next; } Func;
static Func *funcs = NULL, *funcs_tail = NULL;

// =====================================================================
// 5b. GLOBAL INITIALISERS
//
// A global's initialiser has to become bytes in the object file, not code, so
// it is folded at compile time into a byte image plus a list of relocations.
// Strings referenced from a global initialiser get their own pool (.LD*) so
// they can be emitted next to the data rather than through the code path.
// =====================================================================
typedef struct DataStr { int id; char *bytes; int len; struct DataStr *next; } DataStr;
static DataStr *datastrs = NULL, *datastr_tail = NULL;
static int datastr_count = 0;

static int intern_datastr(const char *bytes, int len) {
    DataStr *d = calloc(1, sizeof(DataStr));
    d->id = datastr_count++; d->len = len;
    d->bytes = malloc(len ? (size_t)len : 1);
    memcpy(d->bytes, bytes, (size_t)len);
    if (datastr_tail) datastr_tail->next = d; else datastrs = d;
    datastr_tail = d;
    return d->id;
}

static long eval_const(Node *n) {
    if (!n) die("empty constant expression");
    switch (n->kind) {
    case N_NUM: return n->ival;
    case N_CAST: return eval_const(n->lhs);
    case N_SIZEOF:
        if (n->type) return ty_size(n->type);
        die("sizeof an expression is not allowed in a global initialiser");
        break;
    case N_UNARY:
        switch (n->op) {
        case T_MINUS:  return -eval_const(n->lhs);
        case T_NOT:    return !eval_const(n->lhs);
        case T_BITNOT: return ~eval_const(n->lhs);
        }
        break;
    case N_BIN: {
        long a = eval_const(n->lhs), b = eval_const(n->rhs);
        switch (n->op) {
        case T_PLUS:   return a + b;   case T_MINUS:  return a - b;
        case T_STAR:   return a * b;
        case T_SLASH:  return b ? a / b : 0;
        case T_PERCENT:return b ? a % b : 0;
        case T_AMP:    return a & b;   case T_BITOR:  return a | b;
        case T_BITXOR: return a ^ b;   case T_SHL:    return a << b;
        case T_SHR:    return a >> b;
        case T_EQ:     return a == b;  case T_NE:     return a != b;
        case T_LT:     return a <  b;  case T_GT:     return a >  b;
        case T_LE:     return a <= b;  case T_GE:     return a >= b;
        }
        break;
    }
    case N_LOGAND: return eval_const(n->lhs) && eval_const(n->rhs);
    case N_LOGOR:  return eval_const(n->lhs) || eval_const(n->rhs);
    case N_TERNARY:return eval_const(n->cond) ? eval_const(n->lhs) : eval_const(n->rhs);
    }
    die("a global initialiser must be a compile-time constant");
    return 0;
}

static void put_int(unsigned char *buf, int off, long v, int size) {
    for (int i = 0; i < size; i++) buf[off + i] = (unsigned char)((v >> (8 * i)) & 0xff);
}

static void add_reloc(Sym *g, int off, const char *label) {
    Reloc *r = calloc(1, sizeof(Reloc));
    r->offset = off;
    snprintf(r->label, sizeof r->label, "%s", label);
    // keep the list sorted by offset: the emitter walks the image in order
    Reloc **pp = &g->relocs;
    while (*pp && (*pp)->offset < off) pp = &(*pp)->next;
    r->next = *pp; *pp = r;
}

static void flatten_init(Sym *g, Node *init, Type *t, int off) {
    if (!init) return;

    // char buf[N] = "text"  — the bytes go straight in, NUL included if it fits
    if (init->kind == N_STR && t->is_array && t->ptr->kind == TY_CHAR) {
        int room = t->arr_len, n = init->slen < room ? init->slen : room;
        for (int i = 0; i < n; i++) g->initdata[off + i] = (unsigned char)init->str[i];
        return;                                   // the rest stays zero
    }
    // char *p = "text"  — a pointer to a pooled copy of the string
    if (init->kind == N_STR) {
        char lbl[64];
        snprintf(lbl, sizeof lbl, ".LD%d", intern_datastr(init->str, init->slen));
        add_reloc(g, off, lbl);
        return;
    }
    if (init->kind == N_INIT) {
        if (t->is_array) {
            int esz = ty_size(t->ptr);
            if (init->nbody > t->arr_len)
                die("too many initialisers for this array");
            for (int i = 0; i < init->nbody; i++)
                flatten_init(g, init->body[i], t->ptr, off + i * esz);
            return;
        }
        if (t->kind == TY_STRUCT) {
            Member *m = t->members;
            for (int i = 0; i < init->nbody; i++) {
                if (!m) die("too many initialisers for this struct");
                flatten_init(g, init->body[i], m->type, off + m->offset);
                m = m->next;
            }
            return;
        }
        // { x } initialising a scalar
        if (init->nbody == 0) return;             // { } / {0} on a scalar
        flatten_init(g, init->body[0], t, off);
        return;
    }
    // &global / a bare array or function name used as an address
    if (init->kind == N_ADDR && init->lhs && init->lhs->kind == N_VAR) {
        add_reloc(g, off, init->lhs->name);
        return;
    }
    // `struct Tok *p = toks;` -- an array name used on its own is its address,
    // the same relocation as &toks[0], with no & written
    if (init->kind == N_VAR) {
        Sym *src = sym_find(globals, init->name);
        if (src && src->type && src->type->is_array) {
            add_reloc(g, off, init->name);
            return;
        }
    }
    put_int(g->initdata, off, eval_const(init), ty_size(t));
}

static void global_init(Sym *g, Node *init) {
    int sz = ty_size(g->type); if (sz < 1) sz = 8;
    g->initdata = calloc(1, (size_t)sz);
    g->initlen = sz;
    flatten_init(g, init, g->type, 0);
}

static void parse_toplevel(void) {
    if (at(T_KTYPEDEF)) { parse_typedef(); return; }
    eat(T_KSTATIC);        // at file scope this is linkage only; everything is
                           // one translation unit here, so it changes nothing
    Type *base = parse_type_base_only();
    if (eat(T_SEMI)) return;                // bare  struct Foo { ... };  /  enum { A };
    Type *t = base;
    while (eat(T_STAR)) t = ptr_to(t);
    char nm[256]; snprintf(nm, sizeof(nm), "%s", cur()->text); expect(T_ID);

    if (eat(T_LP)) {                                   // function definition
        // Same reason as the parameter check below: a struct return value is
        // split across rax/rdx or written through a hidden pointer, and this
        // back end returns everything in rax.
        if (t->kind == TY_STRUCT && !t->is_array)
            die("returning a struct by value is not supported yet -- return a pointer");
        fnsig_add(nm, t);              // before the prototype early-return below
        Func *fn = calloc(1, sizeof(Func)); snprintf(fn->name, sizeof(fn->name), "%s", nm);
        while (!at(T_RP)) {
            if (eat(T_ELLIPSIS)) { fn->is_variadic = 1; break; }   // printf(char *fmt, ...)
            Type *pt = parse_type_base_only();
            while (eat(T_STAR)) pt = ptr_to(pt);
            if (pt->kind == TY_VOID && !at(T_ID)) break;   // (void)
            if (pt->kind == TY_STRUCT && !pt->is_array)
                die("a struct parameter by value is not supported yet -- take a pointer");
            Node *pv = new_node(N_DECL);
            snprintf(pv->name, sizeof(pv->name), "%s", cur()->text); expect(T_ID);
            // A parameter declared as an array is a pointer: C rewrites
            // `char a[8][512]` as `char (*a)[512]`. Only the OUTERMOST
            // dimension goes away -- the inner one still has to be there or
            // a[i] would not know how far to step.
            pt = parse_array_suffix(pt);
            if (pt->is_array) pt = ptr_to(pt->ptr);
            pv->type = pt;
            fn->params[fn->nparams] = pv; fn->ptype[fn->nparams] = pt; fn->nparams++;
            if (!eat(T_COMMA)) break;
        }
        expect(T_RP);
        if (eat(T_SEMI)) return;                       // prototype / extern decl — no body to emit
        saliases = NULL;                               // static locals are per function
        snprintf(cur_fn, sizeof cur_fn, "%s", nm);
        fn->body = parse_block();
        cur_fn[0] = 0;
        if (!funcs) funcs = funcs_tail = fn; else { funcs_tail->next = fn; funcs_tail = fn; }
        return;
    }
    // global variable(s):  type name [= ...] (, ...) ;
    for (;;) {
        Type *vt = parse_array_suffix(t);
        Sym *g = add_global(nm, vt);
        if (eat(T_ASSIGN)) {
            Node *init = parse_initializer();
            infer_array_len(vt, init);
            global_init(g, init);                  // flatten to bytes + relocations
        }
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
        case N_BLOCK: case N_INIT:
                      for (int i = 0; i < n->nbody; i++) collect_locals(n->body[i]);
                      break;
        case N_DECL: {
            // Locals are per FUNCTION, not per block: every declaration of a
            // name shares one stack slot. Two `int i` loop counters are fine
            // and idiomatic, but a name reused at a DIFFERENT type would alias
            // two different objects onto the same bytes -- silently. Refuse
            // that rather than miscompile it.
            Sym *prev = sym_find(locals, n->name);
            if (!prev) add_local(n->name, n->type);
            else if (ty_size(prev->type) != ty_size(n->type) ||
                     prev->type->kind != n->type->kind ||
                     prev->type->is_array != n->type->is_array) {
                char m[512];
                snprintf(m, sizeof m,
                         "local '%s' is declared twice at different types in one "
                         "function; block-scoped shadowing is not supported, "
                         "rename one of them", n->name);
                die_node(n, m);
            }
            if (n->init) collect_locals(n->init);
            break;
        }
        case N_IF: case N_TERNARY:
        // N_SWITCH was missing here, so a variable declared inside a switch
        // body never got a stack slot and lookup() later returned NULL.
        case N_SWITCH:
                      collect_locals(n->cond); collect_locals(n->lhs); collect_locals(n->rhs); collect_locals(n->els); break;
        case N_WHILE: case N_DOWHILE:
                      collect_locals(n->lhs); collect_locals(n->cond); break;
        case N_FOR:   collect_locals(n->init); collect_locals(n->cond); collect_locals(n->rhs); collect_locals(n->lhs); break;
        case N_LABEL: case N_CASE:
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

// Named goto labels, mapped to the same numbered .L labels every other jump
// uses -- emitting the C name directly would collide across functions and
// would also risk hitting an assembler keyword. Reset per function, so two
// functions may each have a label called `next`.
typedef struct GLbl { char name[256]; int id; struct GLbl *next; } GLbl;
static GLbl *glabels = NULL;
static int goto_label(const char *name) {
    for (GLbl *g = glabels; g; g = g->next) if (!strcmp(g->name, name)) return g->id;
    GLbl *g = calloc(1, sizeof(GLbl));
    snprintf(g->name, sizeof(g->name), "%s", name);
    g->id = label_id++; g->next = glabels; glabels = g;
    return g->id;                       // first mention wins, forward or back
}

// current function's varargs state (set in gen_func, read by __builtin_va_* codegen)
static int cur_va_off = 0, cur_named = 0;

static Type *gen_expr(Node *n);       // fwd
static void  gen_stmt(Node *n);

// =====================================================================
// 6b. MINIMAL-ISA EMISSION HELPERS
// =====================================================================
// With --minimal the back end restricts itself to the instruction set a small
// bootstrap assembler can encode:
//
//     mov  add  or  and  sub  xor  cmp  shl  shr  sar
//     jmp  je/jz  jne/jnz  jl  jle  jg  jge  jb  jbe  ja  jae
//     call  ret  syscall
//
// plus 64-bit and 8-bit mov to and from memory. Everything else the back end
// would normally reach for - push, pop, lea, leave, test, setcc, movzx, movsx,
// neg, not, imul, idiv, cqo - is synthesised from those. The code is longer and
// slower; that is the trade for a target any small assembler can encode.
//
// The one thing that is NOT synthesised is the 8-bit load and store. A 1-byte
// store cannot be built from 8-byte operations without a read-modify-write of
// the seven bytes around it, which may not be mapped and is not the same
// operation. char, strings and printf all depend on it.
static int g_minimal = 0;

// --nasm emits the NASM subset the bootstrap assembler reads instead of GNU-as
// Intel: `section`/`global` rather than `.section`/`.globl`, `db` rather than
// `.string`/`.zero`, no `ptr` size keywords and no `offset`.
//
// The assembler produces one flat PT_LOAD, so there is nowhere to put a .rodata
// section. String literals therefore cannot be emitted inline the way they are
// in GNU-as mode - they would sit in the instruction stream and be executed.
// They are pooled here and written out after the last function instead.
static int g_nasm = 0;

typedef struct StrLit { int id; char *bytes; int len; struct StrLit *next; } StrLit;
static StrLit *strlits = NULL, *strlit_tail = NULL;

static void strlit_add(int id, const char *bytes, int len) {
    StrLit *sl = calloc(1, sizeof(StrLit));
    sl->id = id; sl->len = len;
    sl->bytes = malloc(len ? len : 1);
    memcpy(sl->bytes, bytes, len);
    if (strlit_tail) strlit_tail->next = sl; else strlits = sl;
    strlit_tail = sl;
}

// Emit a byte run as `db`, keeping printable runs readable as quoted strings.
// The assembler's db has no escape handling, so a quote or any non-printable
// byte is emitted numerically.
// A C identifier is not necessarily a legal assembler symbol. In Intel syntax
// a global called `sp`, `ax`, `gs` or `flat` parses as a register or a keyword,
// and `[rip + sp]` is then rejected -- the compiler would emit a file that
// looks fine and will not assemble. Rename those on the way out. The prefix
// contains a dot, which no C identifier can, so the new name cannot collide
// with a real symbol.
static int asm_reserved(const char *n) {
    static const char *R[] = {
        "rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp","rip",
        "r8","r9","r10","r11","r12","r13","r14","r15",
        "eax","ebx","ecx","edx","esi","edi","ebp","esp","eip",
        "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d",
        "ax","bx","cx","dx","si","di","bp","sp","ip",
        "r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w",
        "al","bl","cl","dl","ah","bh","ch","dh","sil","dil","bpl","spl",
        "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b",
        "cs","ds","es","fs","gs","ss",
        "st","mm0","mm1","xmm0","xmm1","cr0","cr2","cr3","cr4","dr0","dr7",
        "flat","ptr","offset","byte","word","dword","qword","tbyte","xmmword",
        "short","near","far","abs","rel","seg","wrt","strict",
        "db","dw","dd","dq","resb","resw","resd","resq","equ","times",
        "section","global","extern","org","bits","align","section",
        0 };
    for (int i = 0; R[i]; i++) {
        const char *r = R[i]; int k = 0;
        while (r[k] && n[k] && (n[k] | 32) == r[k]) k++;
        if (!r[k] && !n[k]) return 1;
    }
    return 0;
}
static const char *asm_sym(const char *n) {
    static char buf[8][300]; static int slot = 0;   // several live at once per emit
    if (!asm_reserved(n)) return n;
    slot = (slot + 1) % 8;
    snprintf(buf[slot], sizeof buf[slot], "nano.%s", n);
    return buf[slot];
}

// Emit one global's storage. An initialised global is a byte image with
// relocations punched through it, so the two have to come out interleaved in
// offset order — a pointer element is `dq label`, everything else is raw bytes.
static void emit_global(Sym *g, int nasm) {
    int sz = ty_size(g->type); if (sz < 1) sz = 8;
    if (!g->initdata) {
        if (!nasm) { emit("%s: .zero %d", asm_sym(g->name), sz); return; }
        fprintf(fout, "%s:", asm_sym(g->name));
        for (int i = 0; i < sz; i++) {
            if (i % 32 == 0) fputs(i ? "\n db " : " db ", fout);
            else fputs(", ", fout);
            fputc('0', fout);
        }
        fputc('\n', fout);
        return;
    }
    fprintf(fout, "%s:\n", asm_sym(g->name));
    Reloc *r = g->relocs;
    int i = 0;
    while (i < sz) {
        while (r && r->offset < i) r = r->next;          // defensive: never walk backwards
        if (r && r->offset == i) {
            fprintf(fout, nasm ? " dq %s\n" : "    .quad %s\n", asm_sym(r->label));
            i += 8; r = r->next;
            continue;
        }
        int end = r && r->offset < sz ? r->offset : sz;
        int col = 0;
        for (int k = i; k < end; k++) {
            if (col == 0) fputs(nasm ? " db " : "    .byte ", fout);
            else          fputs(", ", fout);
            // `& 255` explicitly: this compiler accepts `unsigned` and
            // ignores it, so a self-built nano_cc reads initdata[k] as a
            // SIGNED char and would print -1 where gcc prints 255. Same
            // byte to the assembler, different text -- which breaks a
            // byte-for-byte comparison of the two compilers' output.
            fprintf(fout, "%d", g->initdata[k] & 255);
            if (++col >= 32) { fputc('\n', fout); col = 0; }
        }
        if (col) fputc('\n', fout);
        i = end;
    }
}

static void emit_db_bytes(const char *label, const unsigned char *b, int len, int nul) {
    int i = 0, col = 0, inq = 0, first = 1;
    if (label) fprintf(fout, "%s:", label);
    fprintf(fout, " db ");
    for (i = 0; i < len; i++) {
        int by = b[i] & 255;                  // see the note in emit_global
        int printable = by >= 32 && by <= 126 && by != '"';
        if (printable) {
            if (!inq) { if (!first) fputs(", ", fout); fputc('"', fout); inq = 1; }
            fputc(by, fout);
        } else {
            if (inq) { fputc('"', fout); inq = 0; }
            if (!first) fputs(", ", fout);
            fprintf(fout, "%d", by);
        }
        first = 0;
        if (++col >= 60) { if (inq) { fputc('"', fout); inq = 0; } fputs("\n db ", fout); col = 0; first = 1; }
    }
    if (inq) fputc('"', fout);
    if (nul) { if (!first) fputs(", ", fout); fputs("0", fout); }
    else if (first) fputs("0", fout);          // empty run still needs an operand
    fputc('\n', fout);
}

static void e_push(const char *r) {
    if (!g_minimal) { emit("    push %s", r); return; }
    emit("    sub rsp, 8");
    emit("    mov [rsp], %s", r);
}
static void e_pop(const char *r) {
    if (!g_minimal) { emit("    pop %s", r); return; }
    emit("    mov %s, [rsp]", r);
    emit("    add rsp, 8");
}
// test rax, rax  ->  cmp rax, 0   (same flags for the zero test)
static void e_test_rax(void) {
    if (!g_minimal) { emit("    test rax, rax"); return; }
    emit("    cmp rax, 0");
}
static void e_neg_rax(void) {
    if (!g_minimal) { emit("    neg rax"); return; }
    emit("    mov rcx, 0");
    emit("    sub rcx, rax");
    emit("    mov rax, rcx");
}
static void e_not_rax(void) {
    if (!g_minimal) { emit("    not rax"); return; }
    emit("    xor rax, -1");
}
static void e_leave(void) {
    if (!g_minimal) { emit("    leave"); return; }
    emit("    mov rsp, rbp");
    emit("    mov rbp, [rsp]");
    emit("    add rsp, 8");
}
// rax = address of a RIP-relative global. Without lea the address is taken as
// an absolute immediate, which is fine for the -no-pie image these binaries are.
static void e_lea_rip(const char *sym) {
    if (!g_minimal) { emit("    lea rax, [rip + %s]", sym); return; }
    // GAS Intel syntax reads a bare symbol as memory CONTENTS, so the address
    // has to be asked for explicitly. In NASM syntax this is plain `mov rax, sym`.
    if (g_nasm) emit("    mov rax, %s", sym);
    else        emit("    mov rax, offset %s", sym);
}
static void e_lea_local(int off) {          // rax = rbp - off
    if (!g_minimal) { emit("    lea rax, [rbp - %d]", off); return; }
    emit("    mov rax, rbp");
    emit("    sub rax, %d", off);
}
// rax = *rax as a signed char, without movsx: mask then sign-extend by shifting.
static void e_load_sbyte(void) {
    if (!g_minimal) { emit("    movsx rax, byte ptr [rax]"); return; }
    emit("    mov al, [rax]");
    emit("    and rax, 255");
    emit("    shl rax, 56");
    emit("    sar rax, 56");
}

static int g_need_divmod = 0;

// A comparison has already been emitted; leave 1 or 0 in rax.
static void e_setcc(const char *setname, const char *jcc) {
    if (!g_minimal) { emit("    %s al", setname); emit("    movzx rax, al"); return; }
    int l = label_id++;
    emit("    mov rax, 1");
    emit("    %s .L%d", jcc, l);
    emit("    mov rax, 0");
    emit(".L%d:", l);
}

// reg = reg * n, with n a positive compile-time constant. Shift-add, so the
// length is fixed by the number of set bits rather than by the value.
// reg must not be r8 or r9; the call sites use rax and rcx.
static void e_imul_const(const char *reg, int n) {
    if (!g_minimal) { emit("    imul %s, %d", reg, n); return; }
    emit("    mov r8, %s", reg);
    emit("    mov %s, 0", reg);
    for (int i = 0; i < 31; i++) {
        if (!(n & (1 << i))) continue;
        emit("    mov r9, r8");
        if (i) emit("    shl r9, %d", i);
        emit("    add %s, r9", reg);
    }
}

// rax = rax * rcx. Shift-add over the bits of rcx. The low 64 bits of a product
// are the same for signed and unsigned operands, so no sign handling is needed.
static void e_imul_rax_rcx(void) {
    if (!g_minimal) { emit("    imul rax, rcx"); return; }
    int top = label_id++, skip = label_id++, done = label_id++;
    emit("    mov rdx, rax");
    emit("    mov rax, 0");
    emit(".L%d:", top);
    emit("    cmp rcx, 0");
    emit("    je .L%d", done);
    emit("    mov r8, rcx");
    emit("    and r8, 1");
    emit("    cmp r8, 0");
    emit("    je .L%d", skip);
    emit("    add rax, rdx");
    emit(".L%d:", skip);
    emit("    shl rdx, 1");
    emit("    shr rcx, 1");
    emit("    jmp .L%d", top);
    emit(".L%d:", done);
}

// rax / rcx. Quotient in rax, remainder in rdx.
static void e_divmod(int want_rem) {
    if (!g_minimal) {
        emit("    cqo"); emit("    idiv rcx");
        if (want_rem) emit("    mov rax, rdx");
        return;
    }
    g_need_divmod = 1;
    emit("    call __nano_divmod");
    if (want_rem) emit("    mov rax, rdx");
}

// Restoring shift-subtract division, emitted once per translation unit.
// Signs are stripped first and reapplied after, because C rounds the quotient
// toward zero while an arithmetic shift rounds toward minus infinity, and the
// remainder takes the sign of the dividend.
// Clobbers rsi and r8-r11, all caller-saved and dead mid-expression here.
static void gen_divmod_routine(void) {
    emit("__nano_divmod:");
    emit("    mov r8, 0");                 // quotient negative?
    emit("    mov r9, 0");                 // remainder negative?
    emit("    cmp rax, 0");
    emit("    jge .Ldm_a");
    emit("    mov r9, 1");
    emit("    mov r8, 1");
    emit("    mov r10, 0");
    emit("    sub r10, rax");
    emit("    mov rax, r10");
    emit(".Ldm_a:");
    emit("    cmp rcx, 0");
    emit("    jge .Ldm_b");
    emit("    xor r8, 1");
    emit("    mov r10, 0");
    emit("    sub r10, rcx");
    emit("    mov rcx, r10");
    emit(".Ldm_b:");
    emit("    mov rdx, 0");                // running remainder
    emit("    mov r11, 0");                // running quotient
    emit("    cmp rcx, 0");
    emit("    je .Ldm_dz");
    emit("    mov r10, 64");
    emit(".Ldm_loop:");
    emit("    cmp r10, 0");
    emit("    je .Ldm_fin");
    emit("    shl rdx, 1");
    emit("    mov rsi, rax");
    emit("    shr rsi, 63");                // next bit of the dividend
    emit("    add rdx, rsi");
    emit("    shl rax, 1");
    emit("    shl r11, 1");
    emit("    cmp rdx, rcx");
    emit("    jb .Ldm_nosub");              // unsigned compare
    emit("    sub rdx, rcx");
    emit("    add r11, 1");
    emit(".Ldm_nosub:");
    emit("    sub r10, 1");
    emit("    jmp .Ldm_loop");
    emit(".Ldm_fin:");
    emit("    mov rax, r11");
    emit("    jmp .Ldm_fix");
    emit(".Ldm_dz:");                       // division by zero yields 0, 0
    emit("    mov rax, 0");
    emit("    mov rdx, 0");
    emit("    ret");
    emit(".Ldm_fix:");
    emit("    cmp r8, 0");
    emit("    je .Ldm_qpos");
    emit("    mov r10, 0");
    emit("    sub r10, rax");
    emit("    mov rax, r10");
    emit(".Ldm_qpos:");
    emit("    cmp r9, 0");
    emit("    je .Ldm_rpos");
    emit("    mov r10, 0");
    emit("    sub r10, rdx");
    emit("    mov rdx, r10");
    emit(".Ldm_rpos:");
    emit("    ret");
}


static Sym *lookup(const char *name) {
    Sym *s = sym_find(locals, name);
    if (!s) s = sym_find(globals, name);
    return s;
}
static void load_rax(int size) {                 // rax = *rax (size-aware, signed char)
    if (size == 1) e_load_sbyte();
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
        if (!s) die_node(n, "undeclared identifier");
        if (s->is_global) e_lea_rip(asm_sym(n->name));
        else              e_lea_local(s->offset);
        return s->type;
    }
    if (n->kind == N_DEREF) {                      // &*p  ==  p
        Type *t = gen_expr(n->lhs);
        return is_ptrish(t) ? t->ptr : ty_long();
    }
    if (n->kind == N_MEMBER) {                      // &(s.field)
        Type *st = gen_addr(n->lhs);               // rax = &struct
        Member *m = find_member(st, n->name);
        if (!m) {
            // Kept to three arguments: this compiler allows six per call, and
            // it has to be able to compile itself.
            char msg[512];
            snprintf(msg, sizeof msg, "no member '%.60s' in type kind %d tag '%.60s'",
                     n->name, st ? st->kind : -1,
                     st && st->tag[0] ? st->tag : "(untagged)");
            die_node(n, msg);
        }
        if (m->offset) emit("    add rax, %d", m->offset);
        return m->type;
    }
    die_at(P, "not an lvalue"); return 0;
}

// best-effort static type inference (used only by sizeof(expr))
static Type *static_typeof(Node *n) {
    switch (n->kind) {
        case N_NUM:    return n->type ? n->type : ty_long();
        case N_STR:    return ptr_to(ty_char());
        case N_CAST:   return n->type;
        case N_VAR:  { Sym *s = lookup(n->name);
                       if (!s) die_node(n, "sizeof an undeclared identifier");
                       return s->type; }
        case N_MEMBER: { Type *st = static_typeof(n->lhs); Member *m = find_member(st, n->name);
                         // Returning `long` here when the member is unknown is
                         // how `sizeof(toks[i].text)` quietly became 8 instead
                         // of 256: the wrong ANSWER, not an error. A wrong
                         // sizeof is far worse than a stopped build.
                         if (!m) die_node(n, "sizeof a member that does not exist");
                         return m->type; }
        case N_DEREF: { Type *t = static_typeof(n->lhs);
                        if (!is_ptrish(t)) die_node(n, "sizeof a dereference of a non-pointer");
                        return t->ptr; }
        // Pointer arithmetic keeps the pointer's type -- without this,
        // `a[i]` (which is `*(a + i)`) had no type at all, and every sizeof
        // through an indexed array silently answered 8.
        case N_BIN: { Type *lt = static_typeof(n->lhs);
                      if (is_ptrish(lt)) return lt;
                      Type *rt = static_typeof(n->rhs);
                      if (is_ptrish(rt)) return rt;
                      return ty_long(); }
        case N_ASSIGN: return static_typeof(n->lhs);
        case N_PRE: case N_POST: return static_typeof(n->lhs);
        case N_TERNARY: return static_typeof(n->lhs);
        case N_ADDR:   return ptr_to(static_typeof(n->lhs));
        case N_CALL: { Type *rt = fnsig_find(n->name); return rt ? rt : ty_long(); }
        default:       return ty_long();
    }
}

static void gen_string(Node *n) {
    int id = label_id++;
    if (g_nasm) {
        strlit_add(id, n->str, n->slen);
        char b[64]; snprintf(b, sizeof b, ".LC%d", id);
        e_lea_rip(b);
        return;
    }
    emit("    .section .rodata");
    fprintf(fout, ".LC%d: .string \"", id);
    for (int i = 0; i < n->slen; i++) {
        int ch = n->str[i] & 255;             // see the note in emit_global
        switch (ch) {
            case '\n': fputs("\\n", fout); break;  case '\t': fputs("\\t", fout); break;
            case '\r': fputs("\\r", fout); break;  case '"':  fputs("\\\"", fout); break;
            case '\\': fputs("\\\\", fout); break;
            default: if (ch < 32 || ch > 126) fprintf(fout, "\\%03o", ch); else fputc(ch, fout);
        }
    }
    fputs("\"\n", fout);
    emit("    .section .text");
    { char b[64]; snprintf(b, sizeof b, ".LC%d", id); e_lea_rip(b); }
}

static Type *gen_expr(Node *n) {
    switch (n->kind) {
    case N_NUM:  emit("    mov rax, %ld", n->ival); return n->type ? n->type : ty_long();
    case N_STR:  gen_string(n); return n->type;
    case N_VAR: {
        Sym *s = lookup(n->name);
        if (!s) die_node(n, "undeclared identifier");
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
        // An array element that is itself an array does not get loaded: for
        // `long m[2][3]`, m[0] is the ADDRESS of row 0, which the next index
        // then walks. Loading here would treat m[0][0] as a pointer.
        if (pt->is_array) return pt;
        load_rax(ty_size(pt));
        return pt;
    }
    case N_ADDR: {
        Type *t = gen_addr(n->lhs);
        return ptr_to(t);
    }
    case N_ASSIGN: {
        Type *lt = gen_addr(n->lhs);
        e_push("rax");
        gen_expr(n->rhs);
        e_pop("rcx");
        store_rcx_rax(ty_size(lt));
        return lt;
    }
    case N_POST: {                                 // x++ / x--  (returns old value)
        Type *lt = gen_addr(n->lhs);
        int step = is_ptrish(lt) ? elem_size(lt) : 1;
        int sz = ty_size(lt);
        e_push("rax");                      // save &x
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
        e_push("rax");                      // save &x
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
        gen_expr(n->cond); e_test_rax(); emit("    jz .L%d", els);
        gen_expr(n->lhs);  emit("    jmp .L%d", end);
        emit(".L%d:", els); gen_expr(n->rhs);
        emit(".L%d:", end);
        return ty_long();
    }
    case N_UNARY:
        gen_expr(n->lhs);
        if      (n->op == T_MINUS)  e_neg_rax();
        else if (n->op == T_BITNOT) e_not_rax();
        else { e_test_rax(); e_setcc("sete", "je"); }
        return ty_long();
    case N_LOGAND: {
        int f = label_id++, e = label_id++;
        gen_expr(n->lhs); e_test_rax(); emit("    jz .L%d", f);
        gen_expr(n->rhs); e_test_rax(); emit("    jz .L%d", f);
        emit("    mov rax, 1"); emit("    jmp .L%d", e);
        emit(".L%d:", f); emit("    mov rax, 0"); emit(".L%d:", e);
        return ty_long();
    }
    case N_LOGOR: {
        int tl = label_id++, e = label_id++;
        gen_expr(n->lhs); e_test_rax(); emit("    jnz .L%d", tl);
        gen_expr(n->rhs); e_test_rax(); emit("    jnz .L%d", tl);
        emit("    mov rax, 0"); emit("    jmp .L%d", e);
        emit(".L%d:", tl); emit("    mov rax, 1"); emit(".L%d:", e);
        return ty_long();
    }
    case N_CALL: {
        // ---- variadic built-ins (handled inline, not real calls) ----
        if (!strcmp(n->name, "__builtin_va_start")) {
            gen_addr(n->args[0]);                  // rax = &ap
            emit("    mov rcx, rax");
            e_lea_local(cur_va_off - cur_named * 8);                      // &save[named]
            emit("    mov [rcx], rax");            // ap = first vararg slot
            return ty_long();
        }
        if (!strcmp(n->name, "__builtin_va_arg")) {
            gen_addr(n->args[0]);                  // rax = &ap
            emit("    mov rcx, rax");              // rcx = &ap
            emit("    mov rax, [rcx]");            // rax = ap
            emit("    mov rdx, [rax]");            // rdx = *ap  (the argument value)
            emit("    add rax, 8");
            emit("    mov [rcx], rax");            // ap += 8
            emit("    mov rax, rdx");
            return ty_long();
        }
        if (!strcmp(n->name, "__builtin_va_end")) return ty_long();   // no-op

        // Arguments 7 and beyond go on the stack in the SysV ABI, which this
        // code generator does not implement — say so instead of walking off
        // the end of ARGREG.
        if (n->nargs > 6) die("more than 6 call arguments is not supported yet");
        for (int i = 0; i < n->nargs; i++) {
            Type *at = gen_expr(n->args[i]);
            // A struct used as a value decays to its ADDRESS here, so passing
            // one by value would hand the callee a pointer and let it read the
            // pointer as if it were the first field. The SysV rule -- split
            // across registers by field class, or copied onto the stack when
            // over 16 bytes -- is not implemented, so refuse rather than
            // generate a call that silently computes nonsense.
            if (at && at->kind == TY_STRUCT && !at->is_array)
                die("passing a struct by value is not supported yet -- pass &s instead");
            e_push("rax");
        }
        for (int i = n->nargs - 1; i >= 0; i--) e_pop(ARGREG[i]);
        // eax is a 32-bit register, which the minimal target has no encoding for
        if (g_minimal) emit("    mov rax, 0");
        else           emit("    xor eax, eax");   // variadic-safe; harmless otherwise
        emit("    call %s", asm_sym(n->name));
        Type *rt = fnsig_find(n->name);
        return rt ? rt : ty_long();
    }
    case N_BIN: {
        Type *lt = gen_expr(n->lhs); e_push("rax");
        Type *rt = gen_expr(n->rhs); emit("    mov rcx, rax"); e_pop("rax");
        // rax = left, rcx = right
        int op = n->op;
        if (op == T_PLUS || op == T_MINUS) {
            if (is_ptrish(lt) && !is_ptrish(rt)) {
                // `esz`, not `s`: a `Sym *s` is already live in this function
                int esz = elem_size(lt); if (esz > 1) e_imul_const("rcx", esz);
            } else if (!is_ptrish(lt) && is_ptrish(rt) && op == T_PLUS) {
                int esz = elem_size(rt); if (esz > 1) e_imul_const("rax", esz);
            }
            emit(op == T_PLUS ? "    add rax, rcx" : "    sub rax, rcx");
            return is_ptrish(lt) ? lt : (is_ptrish(rt) ? rt : ty_long());
        }
        switch (op) {
        case T_STAR:    e_imul_rax_rcx(); break;
        case T_SLASH:   e_divmod(0); break;
        case T_PERCENT: e_divmod(1); break;
        case T_AMP:     emit("    and rax, rcx"); break;
        case T_BITOR:   emit("    or rax, rcx");  break;
        case T_BITXOR:  emit("    xor rax, rcx"); break;
        case T_SHL:     emit("    shl rax, cl");  break;   // count in cl (low byte of rcx)
        case T_SHR:     emit("    sar rax, cl");  break;   // arithmetic shift right
        case T_LT: case T_GT: case T_LE: case T_GE: case T_EQ: case T_NE: {
            emit("    cmp rax, rcx");
            const char *cc = op==T_LT?"setl":op==T_GT?"setg":op==T_LE?"setle":
                             op==T_GE?"setge":op==T_EQ?"sete":"setne";
            const char *jc = op==T_LT?"jl"  :op==T_GT?"jg"  :op==T_LE?"jle" :
                             op==T_GE?"jge" :op==T_EQ?"je"  :"jne";
            e_setcc(cc, jc);
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

// ---- switch/case helpers ----
// A switch body is walked three times: to hand each case/default its own label,
// to find the default label, and to emit the compare-and-jump dispatch. All
// three stop at a nested N_SWITCH so an inner switch keeps its own cases.
static void assign_case_labels(Node *n) {
    if (!n) return;
    if (n->kind == N_SWITCH) return;
    if (n->kind == N_CASE || n->kind == N_DEFAULT) { n->ival = label_id++; return; }
    if (n->kind == N_BLOCK) {
        for (int i = 0; i < n->nbody; i++) assign_case_labels(n->body[i]);
    } else if (n->kind == N_IF) {
        assign_case_labels(n->lhs); assign_case_labels(n->els);
    } else if (n->kind == N_WHILE || n->kind == N_FOR || n->kind == N_DOWHILE ||
               n->kind == N_LABEL) {
        assign_case_labels(n->lhs);   // a case may sit under a goto label
    }
}

static int find_default_label(Node *n) {
    if (!n) return -1;
    if (n->kind == N_SWITCH) return -1;
    if (n->kind == N_DEFAULT) return (int)n->ival;
    if (n->kind == N_BLOCK) {
        for (int i = 0; i < n->nbody; i++) {
            int d = find_default_label(n->body[i]);
            if (d != -1) return d;
        }
    } else if (n->kind == N_IF) {
        int d = find_default_label(n->lhs);
        if (d != -1) return d;
        return find_default_label(n->els);
    } else if (n->kind == N_WHILE || n->kind == N_FOR || n->kind == N_DOWHILE ||
               n->kind == N_LABEL) {
        return find_default_label(n->lhs);
    }
    return -1;
}

// Emit "if (switch_value == case_value) goto case_label" for every case.
// The switch value is saved on the stack at [rsp] by the caller; gen_expr is
// stack-balanced, so we peek it (not pop) after evaluating each case value —
// that keeps it available for every comparison, not just the first.
static void emit_case_jumps(Node *n) {
    if (!n) return;
    if (n->kind == N_SWITCH) return;
    if (n->kind == N_CASE) {
        gen_expr(n->lhs);              // rax = this case's value
        emit("    mov rcx, [rsp]");    // rcx = the switch value (still on the stack)
        emit("    cmp rax, rcx");
        emit("    je .L%ld", n->ival);
        return;
    }
    if (n->kind == N_BLOCK) {
        for (int i = 0; i < n->nbody; i++) emit_case_jumps(n->body[i]);
    } else if (n->kind == N_IF) {
        emit_case_jumps(n->lhs); emit_case_jumps(n->els);
    } else if (n->kind == N_WHILE || n->kind == N_FOR || n->kind == N_DOWHILE ||
               n->kind == N_LABEL) {
        emit_case_jumps(n->lhs);
    }
}

// ---- local brace initialisers ----------------------------------------------
// The object is zeroed first and the supplied elements written over the top.
// That is what C requires for a partial initialiser ({0} on a 256-byte array
// must zero all 256), and it means the element walk never has to work out
// which holes it left behind.
//
// `off` is a distance BELOW rbp, so byte k of an object at `off` lives at
// rbp - (off - k). Sub-objects subtract their own offset the same way.

static void gen_zero_local(int off, int size) {
    if (size <= 0) return;
    int slot = (size + 7) & ~7;                 // add_local rounds the slot up
    if (slot > 128) {                           // big object: loop instead of unrolling
        int top = label_id++;
        e_lea_local(off);                       // rax = &obj[0]
        emit("    mov rcx, rax");
        emit("    mov rax, 0");
        emit("    mov rdx, %d", slot / 8);
        emit(".L%d:", top);
        emit("    mov [rcx], rax");
        emit("    add rcx, 8");
        emit("    sub rdx, 1");
        emit("    cmp rdx, 0");
        emit("    jne .L%d", top);
        return;
    }
    emit("    mov rax, 0");
    for (int i = 0; i < slot; i += 8) emit("    mov [rbp - %d], rax", off - i);
}

static void gen_init_at(Node *init, Type *t, int off) {
    if (!init) return;

    // char buf[N] = "text"
    if (init->kind == N_STR && t->is_array && t->ptr->kind == TY_CHAR) {
        int room = t->arr_len, n = init->slen < room ? init->slen : room;
        for (int i = 0; i < n; i++) {
            if (!init->str[i]) continue;        // already zeroed
            e_lea_local(off - i);
            emit("    mov rcx, rax");
            emit("    mov rax, %d", (unsigned char)init->str[i]);
            emit("    mov [rcx], al");
        }
        return;
    }
    if (init->kind == N_INIT) {
        if (t->is_array) {
            int esz = ty_size(t->ptr);
            if (init->nbody > t->arr_len) die("too many initialisers for this array");
            for (int i = 0; i < init->nbody; i++)
                gen_init_at(init->body[i], t->ptr, off - i * esz);
            return;
        }
        if (t->kind == TY_STRUCT) {
            Member *m = t->members;
            for (int i = 0; i < init->nbody; i++) {
                if (!m) die("too many initialisers for this struct");
                gen_init_at(init->body[i], m->type, off - m->offset);
                m = m->next;
            }
            return;
        }
        if (init->nbody == 0) return;           // {} / {0} on a scalar
        gen_init_at(init->body[0], t, off);
        return;
    }
    e_lea_local(off);
    e_push("rax");
    gen_expr(init);
    e_pop("rcx");
    store_rcx_rax(ty_size(t));
}

static void gen_local_init(Sym *s, Node *init) {
    gen_zero_local(s->offset, ty_size(s->type));
    gen_init_at(init, s->type, s->offset);
}

static void gen_stmt(Node *n) {
    switch (n->kind) {
    case N_BLOCK: for (int i = 0; i < n->nbody; i++) gen_stmt(n->body[i]); break;
    case N_EMPTY: break;
    case N_DECL:
        if (n->init) {
            Sym *s = lookup(n->name);
            if (n->init->kind == N_INIT ||
                (n->init->kind == N_STR && s->type->is_array)) {
                gen_local_init(s, n->init);
                break;
            }
            e_lea_local(s->offset);
            e_push("rax");
            gen_expr(n->init);
            e_pop("rcx");
            store_rcx_rax(ty_size(s->type));
        }
        break;
    case N_EXPR: gen_expr(n->lhs); break;
    case N_RETURN:
        if (n->lhs) gen_expr(n->lhs);
        e_leave(); emit("    ret");
        break;
    case N_IF: {
        int els = label_id++, end = label_id++;
        gen_expr(n->cond); e_test_rax(); emit("    jz .L%d", els);
        gen_stmt(n->lhs);  emit("    jmp .L%d", end);
        emit(".L%d:", els); if (n->els) gen_stmt(n->els);
        emit(".L%d:", end);
        break;
    }
    case N_WHILE: {
        int top = label_id++, end = label_id++;
        emit(".L%d:", top);
        gen_expr(n->cond); e_test_rax(); emit("    jz .L%d", end);
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
        gen_expr(n->cond); e_test_rax(); emit("    jnz .L%d", top);
        emit(".L%d:", end);
        break;
    }
    case N_FOR: {
        int top = label_id++, cont = label_id++, end = label_id++;
        if (n->init) gen_stmt(n->init);
        emit(".L%d:", top);
        if (n->cond) { gen_expr(n->cond); e_test_rax(); emit("    jz .L%d", end); }
        loop_push(end, cont); gen_stmt(n->lhs); loop_pop();
        emit(".L%d:", cont);
        if (n->rhs) gen_expr(n->rhs);                 // step
        emit("    jmp .L%d", top);
        emit(".L%d:", end);
        break;
    }
    case N_GOTO:
        emit("    jmp .L%d", goto_label(n->name));
        break;
    case N_LABEL:
        emit(".L%d:", goto_label(n->name));
        gen_stmt(n->lhs);
        break;
    case N_BREAK:
        if (loop_sp == 0) die("break outside loop or switch");
        emit("    jmp .L%d", brk_lbl[loop_sp-1]);
        break;
    case N_CONTINUE:
        // switch pushes cont_lbl == -1, so continue inside a bare switch is an error
        if (loop_sp == 0 || cont_lbl[loop_sp-1] == -1) die("continue outside loop");
        emit("    jmp .L%d", cont_lbl[loop_sp-1]);
        break;
    case N_SWITCH: {
        int end = label_id++;
        assign_case_labels(n->lhs);
        int def = find_default_label(n->lhs);

        gen_expr(n->cond);              // switch value in rax
        e_push("rax");           // keep it on the stack for every comparison
        emit_case_jumps(n->lhs);        // jump to the matching case label
        emit("    add rsp, 8");         // discard the saved switch value
        if (def != -1) emit("    jmp .L%d", def);   // no match -> default
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
        if (n->ival != -1) emit(".L%ld:", n->ival);
        break;
    case N_ASM: gen_asm(n); break;
    default: gen_expr(n); break;                    // expression used as statement
    }
}

static void gen_func(Func *fn) {
    locals = NULL; frame_size = 0; glabels = NULL;
    // params first (so they get the lowest offsets, in declared order)
    for (int i = 0; i < fn->nparams; i++) add_local(fn->params[i]->name, fn->ptype[i]);
    collect_locals(fn->body);
    // reserve a 48-byte register save area for variadic functions
    int va_off = 0;
    if (fn->is_variadic) { frame_size += 48; va_off = frame_size; }
    cur_va_off = va_off; cur_named = fn->nparams;
    int fs = (frame_size + 15) & ~15;

    // Every function gets external linkage, which is what C says a
    // non-static function has. Only `main` and `_start` used to be exported,
    // so a second object file could not call anything here -- the kernel's
    // assembly stubs could not reach their own C dispatcher.
    emit(g_nasm ? "global %s" : "    .globl %s", asm_sym(fn->name));
    emit("%s:", asm_sym(fn->name));
    e_push("rbp");
    emit("    mov rbp, rsp");
    if (fs > 0) emit("    sub rsp, %d", fs);
    for (int i = 0; i < fn->nparams && i < 6; i++) {
        Sym *s = sym_find(locals, fn->params[i]->name);
        int sz = ty_size(s->type);
        if (sz == 1) {
            // dil, sil and r8b..r15b all need a REX prefix to name at all. The
            // minimal target only has the four REX-free byte registers, so the
            // argument goes through rax first.
            if (g_minimal) {
                emit("    mov rax, %s", ARGREG[i]);
                emit("    mov [rbp - %d], al", s->offset);
            } else {
                emit("    mov [rbp - %d], %s", s->offset,
                     i==0?"dil":i==1?"sil":i==2?"dl":i==3?"cl":i==4?"r8b":"r9b");
            }
        }
        else emit("    mov [rbp - %d], %s", s->offset, ARGREG[i]);
    }
    if (fn->is_variadic) {
        // spill all six integer arg registers so va_arg can walk them
        const char *r[6] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
        for (int i = 0; i < 6; i++) emit("    mov [rbp - %d], %s", va_off - i * 8, r[i]);
    }
    gen_stmt(fn->body);
    e_leave(); emit("    ret");                     // safety epilogue
}

// =====================================================================
// 8. MAIN
// =====================================================================
int main(int argc, char **argv) {
    // Optional flags may precede the file names.  --kernel suppresses the
    // Linux _start/exit stub so a bare-metal boot stub can provide the entry
    // point and simply call main() (no Linux syscalls exist in a kernel).
    int kernel_mode = 0;
    const char *inpath = NULL, *outpath = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--kernel") || !strcmp(argv[i], "-k")) kernel_mode = 1;
        else if (!strcmp(argv[i], "--minimal")) g_minimal = 1;
        else if (!strcmp(argv[i], "--nasm")) g_nasm = 1;
        else if (!inpath)  inpath  = argv[i];
        else if (!outpath) outpath = argv[i];
    }
    if (!inpath || !outpath) {
        fprintf(stderr, "Usage: %s [--kernel] [--minimal] [--nasm] <input.c> <output.s>\n", argv[0]);
        return 1;
    }

    preprocess(inpath);
    SRC[SRC_LEN] = 0;
    lex();

    while (!at(T_EOF)) parse_toplevel();

    fout = fopen(outpath, "w");
    if (!fout) { perror("fopen"); return 1; }

    if (g_nasm) {
        emit("section .text");
        emit("global main");
    } else {
        emit(".intel_syntax noprefix");
        emit("    .section .text");
        emit("    .globl main");
    }

    if (!kernel_mode) {
        // hosted freestanding entry point: run main, then exit(rax)
        emit(g_nasm ? "global _start" : "    .globl _start");
        emit("_start:");
        // The kernel leaves argc at [rsp] and argv immediately above it, in
        // memory -- not in registers. Without this main() reads whatever was
        // left in rdi/rsi. No demo takes arguments, so nothing caught it, but
        // nano_cc itself reads argv. `add` rather than `lea`: the minimal
        // instruction set has no lea.
        emit("    mov rdi, [rsp]");        // argc
        emit("    mov rax, rsp");
        emit("    add rax, 8");
        emit("    mov rsi, rax");          // argv
        emit("    call main");
        emit("    mov rdi, rax");
        emit("    mov rax, 60");
        emit("    syscall");
    }

    for (Func *f = funcs; f; f = f->next) gen_func(f);
    if (g_need_divmod) gen_divmod_routine();

    // Globals.  Hosted mode puts them in .bss (zeroed by the loader).  Kernel
    // mode uses .data so the zero bytes are emitted into the object and survive
    // an objcopy to a flat binary — the bare-metal image needs no separate
    // .bss zero-fill step.
    if (g_nasm) {
        // One flat image: the pooled string literals and the globals both go
        // here, after the last function, where nothing can execute into them.
        for (StrLit *sl = strlits; sl; sl = sl->next) {
            char lbl[64]; snprintf(lbl, sizeof lbl, ".LC%d", sl->id);
            emit_db_bytes(lbl, (const unsigned char *)sl->bytes, sl->len, 1);
        }
        for (DataStr *d = datastrs; d; d = d->next) {
            char lbl[64]; snprintf(lbl, sizeof lbl, ".LD%d", d->id);
            emit_db_bytes(lbl, (const unsigned char *)d->bytes, d->len, 1);
        }
        for (Sym *g = globals; g; g = g->next) emit_global(g, 1);
    } else {
        emit(kernel_mode ? "    .section .data" : "    .section .bss");
        emit("    .align 8");
        for (Sym *g = globals; g; g = g->next) {
            // An initialised global cannot live in .bss — .bss has no contents.
            if (g->initdata && !kernel_mode) continue;
            emit_global(g, 0);
        }
    }
    if (!g_nasm) {
        // Initialised globals and the strings they point at need a section
        // that actually holds bytes.
        // In kernel mode the globals were already emitted into .data above, so
        // only the string pool is left; without this the section header came
        // out a second time with nothing under it.
        int any = datastrs != NULL;
        if (!kernel_mode)
            for (Sym *g = globals; g && !any; g = g->next) if (g->initdata) any = 1;
        if (any) {
            emit("    .section .data");
            emit("    .align 8");
            for (DataStr *d = datastrs; d; d = d->next) {
                fprintf(fout, ".LD%d: .string \"", d->id);
                for (int i = 0; i < d->len; i++) {
                    int ch = d->bytes[i] & 255;   // see the note in emit_global
                    switch (ch) {
                        case '\n': fputs("\\n", fout); break;  case '\t': fputs("\\t", fout); break;
                        case '\r': fputs("\\r", fout); break;  case '"':  fputs("\\\"", fout); break;
                        case '\\': fputs("\\\\", fout); break;
                        default: if (ch < 32 || ch > 126) fprintf(fout, "\\%03o", ch); else fputc(ch, fout);
                    }
                }
                fputs("\"\n", fout);
            }
            for (Sym *g = globals; g; g = g->next)
                if (g->initdata && !kernel_mode) emit_global(g, 0);
        }
    }

    fclose(fout);
    printf("Compiled %s -> %s%s\n", inpath, outpath, kernel_mode ? " (kernel mode)" : "");
    return 0;
}
