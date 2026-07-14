/*
 * cc64.c - A minimal C compiler targeting the 6502/6510 (Commodore 64),
 *          emitting assembly source compatible with c64asm.c.
 *
 * Portable C99. Builds with clang or gcc:
 *     cc -std=c99 -O2 -o cc64 cc64.c
 *
 * Usage:
 *     ./cc64 input.c -o output.asm
 *     ./c64asm output.asm -o output.prg
 *
 * ---------------------------------------------------------------------
 * SUPPORTED LANGUAGE (step 1 / "minimal" subset of C)
 * ---------------------------------------------------------------------
 *   Types:        int (16-bit), char (8-bit, UNSIGNED), void
 *   Declarations: globals and locals, with optional 1-D array form
 *                 (int a[10]; char buf[40];), optional simple constant
 *                 initializer for scalar globals (int x = 5;)
 *   Functions:    return type + name + typed parameter list; NO
 *                 recursion (direct or indirect) - see LIMITATIONS.
 *   Statements:   if/else, while, for, break, continue, return,
 *                 expression statements, local declarations (anywhere
 *                 in a block), blocks, empty statement.
 *   Operators:    + - * / % & | ^ ~ ! << >> && || == != < > <= >=
 *                 = += -= *= /= %= &= |= ^= <<= >>= ++ -- (pre/post)
 *   Literals:     decimal ints, 0x hex, 'c' char literals (with
 *                 \n \t \\ \' \" \0 escapes), "string" literals
 *                 (ONLY as the sole argument to the builtin puts()).
 *   Builtins:     putchar(x), puts("literal"), peek(addr), poke(addr,val)
 *
 * NOT supported yet (planned for later steps):
 *   pointers, &, *deref, structs/unions, typedefs, function recursion,
 *   passing arrays to functions, multi-dimensional arrays, floats,
 *   the preprocessor, do/while, switch, multiple source files, printf.
 *
 * ---------------------------------------------------------------------
 * CODE GENERATION MODEL
 * ---------------------------------------------------------------------
 * Because C-level recursion is disallowed in this step, every function's
 * parameters and local variables are given FIXED, statically-allocated
 * storage (like old-style non-reentrant compilers) - no runtime call
 * stack for locals is needed. Arguments are copied into the callee's
 * fixed parameter slots before a plain JSR; a function's return value is
 * left in the primary register R at RTS time.
 *
 * A small software operand stack (in a reserved memory buffer) is used
 * only for evaluating nested expressions (e.g. a*(b+c)), via __rt_push/
 * __rt_pop2. This is independent of the "no recursion" restriction and
 * will remain useful once function recursion is added later.
 *
 * Zero-page usage (documented in the emitted asm header too):
 *   $02/$03  __zpSP  - soft operand-stack pointer
 *   $F3/$F4  __zpAP  - address pointer (array/peek/poke effective addr)
 *   $F5/$F6  __zpAP2 - saved-AP scratch (assign/compound-assign to array elems)
 *   $F7/$F8  __zpT1  - scratch (runtime lib internal)
 *   $F9/$FA  __zpT0  - scratch (runtime lib internal)
 *   $FB/$FC  __zpR   - primary "register" (expression result / retval)
 *   $FD/$FE  __zpR2  - secondary operand register
 * These are the classic "free for machine code" zero-page bytes used
 * when a program takes over via SYS and isn't relying on BASIC floating
 * point or RS-232 routines concurrently.
 *
 * All compiler-generated assembly symbols use a leading-double-underscore
 * or underscore+letter reserved namespace (__fn_, __g_, __L, __rt_, __zp,
 * __str) per C's own reserved-identifier rules, so they cannot collide
 * with any valid user identifier.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* ===================================================================
 * Small utilities
 * =================================================================== */

static void fatal(int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "cc64: error: ");
    if (line > 0) fprintf(stderr, "line %d: ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "cc64: out of memory\n"); exit(1); }
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { fprintf(stderr, "cc64: out of memory\n"); exit(1); }
    return q;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ===================================================================
 * Lexer
 * =================================================================== */

typedef enum {
    T_EOF, T_IDENT, T_NUM, T_CHARLIT, T_STRLIT,
    T_INT, T_CHAR, T_VOID, T_IF, T_ELSE, T_WHILE, T_FOR, T_RETURN,
    T_BREAK, T_CONTINUE,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_SEMI, T_COMMA,
    T_ASSIGN, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE,
    T_ANDAND, T_OROR, T_NOT, T_AMP, T_PIPE, T_CARET, T_TILDE,
    T_SHL, T_SHR,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_AMPEQ, T_PIPEEQ, T_CARETEQ, T_SHLEQ, T_SHREQ,
    T_INC, T_DEC
} TokKind;

typedef struct {
    TokKind kind;
    char *text;   /* for T_IDENT, T_STRLIT (already unescaped) */
    long ival;    /* for T_NUM, T_CHARLIT */
    int line;
} Token;

static Token *g_toks = NULL;
static int g_ntoks = 0, g_tokcap = 0;

static void tok_push(Token t) {
    if (g_ntoks >= g_tokcap) {
        g_tokcap = g_tokcap ? g_tokcap * 2 : 4096;
        g_toks = xrealloc(g_toks, g_tokcap * sizeof(Token));
    }
    g_toks[g_ntoks++] = t;
}

static int is_id_start(int c) { return isalpha(c) || c == '_'; }
static int is_id_char(int c)  { return isalnum(c) || c == '_'; }

static struct { const char *kw; TokKind k; } keywords[] = {
    {"int", T_INT}, {"char", T_CHAR}, {"void", T_VOID},
    {"if", T_IF}, {"else", T_ELSE}, {"while", T_WHILE}, {"for", T_FOR},
    {"return", T_RETURN}, {"break", T_BREAK}, {"continue", T_CONTINUE},
    {NULL, T_EOF}
};

/* escape processing shared by char & string literals */
static int read_escape(const char *s, size_t *i, size_t n, int line) {
    /* s[*i] == '\\' on entry */
    (*i)++;
    if (*i >= n) fatal(line, "unterminated escape sequence");
    char c = s[*i]; (*i)++;
    switch (c) {
        case 'n': return 13;  /* C64 CHROUT newline is carriage return */
        case 't': return 9;
        case '0': return 0;
        case '\\': return '\\';
        case '\'': return '\'';
        case '"': return '"';
        default: fatal(line, "unsupported escape sequence '\\%c'", c);
    }
    return 0;
}

static void lex(const char *src) {
    size_t n = strlen(src);
    size_t i = 0;
    int line = 1;
    while (i < n) {
        char c = src[i];
        if (c == '\n') { line++; i++; continue; }
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '/' && i + 1 < n && src[i+1] == '/') {
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i+1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i+1] == '/')) {
                if (src[i] == '\n') line++;
                i++;
            }
            if (i + 1 >= n) fatal(line, "unterminated block comment");
            i += 2;
            continue;
        }
        Token t; memset(&t, 0, sizeof(t)); t.line = line;

        if (is_id_start((unsigned char)c)) {
            size_t j = i;
            while (j < n && is_id_char((unsigned char)src[j])) j++;
            size_t len = j - i;
            char *buf = xmalloc(len + 1);
            memcpy(buf, src + i, len); buf[len] = '\0';
            TokKind kk = T_IDENT;
            for (int k = 0; keywords[k].kw; k++)
                if (strcmp(keywords[k].kw, buf) == 0) { kk = keywords[k].k; break; }
            t.kind = kk;
            t.text = buf;
            tok_push(t);
            i = j;
            continue;
        }
        if (isdigit((unsigned char)c)) {
            size_t j = i;
            long v;
            if (c == '0' && j + 1 < n && (src[j+1] == 'x' || src[j+1] == 'X')) {
                j += 2;
                size_t start = j;
                while (j < n && isxdigit((unsigned char)src[j])) j++;
                if (j == start) fatal(line, "bad hex literal");
                v = strtol(src + start, NULL, 16);
            } else {
                while (j < n && isdigit((unsigned char)src[j])) j++;
                v = strtol(src + i, NULL, 10);
            }
            t.kind = T_NUM; t.ival = v;
            tok_push(t);
            i = j;
            continue;
        }
        if (c == '\'') {
            i++;
            int v;
            if (i < n && src[i] == '\\') v = read_escape(src, &i, n, line);
            else if (i < n) { v = (unsigned char)src[i]; i++; }
            else { fatal(line, "unterminated char literal"); v = 0; }
            if (i >= n || src[i] != '\'') fatal(line, "unterminated char literal");
            i++;
            t.kind = T_CHARLIT; t.ival = v;
            tok_push(t);
            continue;
        }
        if (c == '"') {
            i++;
            char buf[4096]; size_t bl = 0;
            while (i < n && src[i] != '"') {
                int v;
                if (src[i] == '\\') v = read_escape(src, &i, n, line);
                else { v = (unsigned char)src[i]; i++; }
                if (bl + 1 >= sizeof(buf)) fatal(line, "string literal too long");
                buf[bl++] = (char)v;
            }
            if (i >= n) fatal(line, "unterminated string literal");
            i++;
            buf[bl] = '\0';
            t.kind = T_STRLIT; t.text = xstrdup(buf);
            tok_push(t);
            continue;
        }

#define TWO(a,b,k) (c==(a) && i+1<n && src[i+1]==(b) && (t.kind=(k), i+=2, 1))
#define THREE(a,b,cc,k) (c==(a) && i+2<n && src[i+1]==(b) && src[i+2]==(cc) && (t.kind=(k), i+=3, 1))
        if (THREE('<','<','=',T_SHLEQ)) { tok_push(t); continue; }
        if (THREE('>','>','=',T_SHREQ)) { tok_push(t); continue; }
        if (TWO('=','=',T_EQ)) { tok_push(t); continue; }
        if (TWO('!','=',T_NE)) { tok_push(t); continue; }
        if (TWO('<','=',T_LE)) { tok_push(t); continue; }
        if (TWO('>','=',T_GE)) { tok_push(t); continue; }
        if (TWO('&','&',T_ANDAND)) { tok_push(t); continue; }
        if (TWO('|','|',T_OROR)) { tok_push(t); continue; }
        if (TWO('<','<',T_SHL)) { tok_push(t); continue; }
        if (TWO('>','>',T_SHR)) { tok_push(t); continue; }
        if (TWO('+','=',T_PLUSEQ)) { tok_push(t); continue; }
        if (TWO('-','=',T_MINUSEQ)) { tok_push(t); continue; }
        if (TWO('*','=',T_STAREQ)) { tok_push(t); continue; }
        if (TWO('/','=',T_SLASHEQ)) { tok_push(t); continue; }
        if (TWO('%','=',T_PERCENTEQ)) { tok_push(t); continue; }
        if (TWO('&','=',T_AMPEQ)) { tok_push(t); continue; }
        if (TWO('|','=',T_PIPEEQ)) { tok_push(t); continue; }
        if (TWO('^','=',T_CARETEQ)) { tok_push(t); continue; }
        if (TWO('+','+',T_INC)) { tok_push(t); continue; }
        if (TWO('-','-',T_DEC)) { tok_push(t); continue; }
#undef TWO
#undef THREE
        switch (c) {
            case '(': t.kind = T_LPAREN; break;
            case ')': t.kind = T_RPAREN; break;
            case '{': t.kind = T_LBRACE; break;
            case '}': t.kind = T_RBRACE; break;
            case '[': t.kind = T_LBRACKET; break;
            case ']': t.kind = T_RBRACKET; break;
            case ';': t.kind = T_SEMI; break;
            case ',': t.kind = T_COMMA; break;
            case '=': t.kind = T_ASSIGN; break;
            case '+': t.kind = T_PLUS; break;
            case '-': t.kind = T_MINUS; break;
            case '*': t.kind = T_STAR; break;
            case '/': t.kind = T_SLASH; break;
            case '%': t.kind = T_PERCENT; break;
            case '<': t.kind = T_LT; break;
            case '>': t.kind = T_GT; break;
            case '!': t.kind = T_NOT; break;
            case '&': t.kind = T_AMP; break;
            case '|': t.kind = T_PIPE; break;
            case '^': t.kind = T_CARET; break;
            case '~': t.kind = T_TILDE; break;
            default: fatal(line, "unexpected character '%c'", c); break;
        }
        i++;
        tok_push(t);
    }
    Token e; memset(&e, 0, sizeof(e)); e.kind = T_EOF; e.line = line;
    tok_push(e);
}

/* ===================================================================
 * AST
 * =================================================================== */

typedef enum {
    N_NUM, N_STR, N_IDENT, N_CALL, N_INDEX,
    N_ASSIGN, N_COMPOUND_ASSIGN, N_BINOP, N_LOGAND, N_LOGOR,
    N_UNARY, N_PREINC, N_PREDEC, N_POSTINC, N_POSTDEC,
    N_BLOCK, N_IF, N_WHILE, N_FOR, N_RETURN, N_BREAK, N_CONTINUE,
    N_EXPRSTMT, N_VARDECL, N_EMPTY
} NodeKind;

typedef struct Node {
    NodeKind kind;
    char op[3];
    char *name;
    long ival;
    char *sval;
    struct Node *a, *b, *c, *d;
    struct Node *next;
    int declType;     /* 0=char, 1=int */
    int declArrLen;   /* 0 = scalar */
    int line;
} Node;

static Node *node_new(NodeKind k, int line) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(*n));
    n->kind = k;
    n->line = line;
    return n;
}

/* ===================================================================
 * Symbol tables
 * =================================================================== */

#define TY_CHAR 0
#define TY_INT  1

typedef struct {
    char name[64];
    int type;
    int isArray;
    int arrLen;
    long initVal;   /* for globals with constant scalar initializer */
    int hasInit;
} GSym;

static GSym g_globals[1024];
static int g_nglobals = 0;

typedef struct {
    char name[64];
    int retType;      /* -1 = void */
    int nparams;
    int paramTypes[32];
    char paramNames[32][64];
    int defined;
    int bodyStart, bodyEnd; /* token index range of '{' ... '}' body, for call-graph scan */
} FnSym;

static FnSym g_funcs[512];
static int g_nfuncs = 0;

typedef struct {
    char name[64];
    int type;
    int isArray;
    int arrLen;
    int isParam;
} LSym;

static LSym g_locals[256];
static int g_nlocals = 0;
static FnSym *g_curfn = NULL;

static GSym *find_global(const char *name) {
    for (int i = 0; i < g_nglobals; i++)
        if (strcmp(g_globals[i].name, name) == 0) return &g_globals[i];
    return NULL;
}
static FnSym *find_func(const char *name) {
    for (int i = 0; i < g_nfuncs; i++)
        if (strcmp(g_funcs[i].name, name) == 0) return &g_funcs[i];
    return NULL;
}
static LSym *find_local(const char *name) {
    for (int i = 0; i < g_nlocals; i++)
        if (strcmp(g_locals[i].name, name) == 0) return &g_locals[i];
    return NULL;
}

static int is_builtin(const char *name) {
    return strcmp(name, "putchar") == 0 || strcmp(name, "puts") == 0 ||
           strcmp(name, "peek") == 0 || strcmp(name, "poke") == 0;
}

/* ===================================================================
 * Parser - shared token cursor
 * =================================================================== */

static int g_pos = 0;

static Token *cur(void) { return &g_toks[g_pos]; }
static Token *peekAt(int off) { return &g_toks[g_pos + off]; }
static Token *advance(void) { return &g_toks[g_pos++]; }
static int check(TokKind k) { return cur()->kind == k; }
static Token *expect(TokKind k, const char *what) {
    if (!check(k)) fatal(cur()->line, "expected %s", what);
    return advance();
}

static int is_type_kw(TokKind k) { return k == T_INT || k == T_CHAR || k == T_VOID; }

/* ===================================================================
 * Pass A: scan top-level declarations (signatures only, no bodies)
 * =================================================================== */

static void skip_balanced(TokKind open, TokKind close) {
    int depth = 1;
    advance(); /* consume the opening token already checked by caller */
    while (depth > 0) {
        if (check(T_EOF)) fatal(cur()->line, "unexpected end of file (unbalanced braces)");
        if (cur()->kind == open) depth++;
        else if (cur()->kind == close) depth--;
        advance();
    }
}

static int type_from_tok(TokKind k) {
    if (k == T_CHAR) return TY_CHAR;
    if (k == T_INT) return TY_INT;
    return -1; /* void */
}

static void pass_a(void) {
    g_pos = 0;
    while (!check(T_EOF)) {
        if (!is_type_kw(cur()->kind))
            fatal(cur()->line, "expected type at top level");
        TokKind tk = advance()->kind;
        if (check(T_STAR))
            fatal(cur()->line, "pointer types are not supported in this version");
        int rtype = type_from_tok(tk);
        if (!check(T_IDENT)) fatal(cur()->line, "expected identifier after type");
        char *name = advance()->text;
        if (is_builtin(name))
            fatal(cur()->line, "'%s' is a reserved builtin name", name);

        if (check(T_LPAREN)) {
            /* function */
            advance();
            FnSym *fn = find_func(name);
            if (!fn) {
                if (g_nfuncs >= (int)(sizeof(g_funcs)/sizeof(g_funcs[0])))
                    fatal(cur()->line, "too many functions");
                fn = &g_funcs[g_nfuncs++];
                memset(fn, 0, sizeof(*fn));
                strncpy(fn->name, name, sizeof(fn->name)-1);
                fn->retType = rtype;
            }
            fn->nparams = 0;
            if (check(T_VOID) && peekAt(1)->kind == T_RPAREN) {
                advance();
            } else if (!check(T_RPAREN)) {
                for (;;) {
                    if (!is_type_kw(cur()->kind))
                        fatal(cur()->line, "expected parameter type");
                    TokKind ptk = advance()->kind;
                    if (check(T_STAR))
                        fatal(cur()->line, "pointer parameters are not supported in this version");
                    if (ptk == T_VOID) fatal(cur()->line, "void is not a valid parameter type");
                    if (!check(T_IDENT)) fatal(cur()->line, "expected parameter name");
                    char *pname = advance()->text;
                    if (fn->nparams >= 32) fatal(cur()->line, "too many parameters");
                    fn->paramTypes[fn->nparams] = type_from_tok(ptk);
                    strncpy(fn->paramNames[fn->nparams], pname, 63);
                    fn->nparams++;
                    if (check(T_COMMA)) { advance(); continue; }
                    break;
                }
            }
            expect(T_RPAREN, "')'");
            if (check(T_SEMI)) { advance(); continue; } /* prototype only */
            if (!check(T_LBRACE)) fatal(cur()->line, "expected '{' or ';'");
            fn->defined = 1;
            fn->bodyStart = g_pos;
            skip_balanced(T_LBRACE, T_RBRACE);
            fn->bodyEnd = g_pos;
            continue;
        }

        /* global variable */
        if (rtype < 0) fatal(cur()->line, "'void' is not a valid variable type");
        GSym g; memset(&g, 0, sizeof(g));
        strncpy(g.name, name, sizeof(g.name)-1);
        g.type = rtype;
        if (check(T_LBRACKET)) {
            advance();
            if (!check(T_NUM)) fatal(cur()->line, "expected array size");
            g.isArray = 1;
            g.arrLen = (int)advance()->ival;
            expect(T_RBRACKET, "']'");
        }
        if (check(T_ASSIGN)) {
            advance();
            int neg = 0;
            if (check(T_MINUS)) { neg = 1; advance(); }
            long v;
            if (check(T_NUM) || check(T_CHARLIT)) v = advance()->ival;
            else { fatal(cur()->line, "global initializers must be a constant literal"); v = 0; }
            g.hasInit = 1;
            g.initVal = neg ? -v : v;
        }
        expect(T_SEMI, "';'");
        if (find_global(g.name)) fatal(cur()->line, "redefinition of global '%s'", g.name);
        if (g_nglobals >= (int)(sizeof(g_globals)/sizeof(g_globals[0])))
            fatal(cur()->line, "too many globals");
        g_globals[g_nglobals++] = g;
    }
}

/* ===================================================================
 * Recursion detection: scan each function body's raw tokens for
 * `IDENT (` sequences naming another known function, build a call
 * graph, then DFS for cycles (including direct self-calls). This is a
 * lightweight token scan, not a full parse, so it may over-approximate
 * (e.g. it can't tell a shadowed local from a real call target) - but
 * a false positive here just means a clear compile error instead of
 * silently-corrupt runtime behavior, which is the right tradeoff since
 * recursion isn't supported by this step's static-storage model.
 * =================================================================== */

#define MAX_CALL_EDGES 4096
typedef struct { int from, to; } CallEdge;
static CallEdge g_edges[MAX_CALL_EDGES];
static int g_nedges = 0;

static int fn_index(const char *name) {
    for (int i = 0; i < g_nfuncs; i++)
        if (strcmp(g_funcs[i].name, name) == 0) return i;
    return -1;
}

static void build_call_graph(void) {
    for (int fi = 0; fi < g_nfuncs; fi++) {
        FnSym *fn = &g_funcs[fi];
        if (!fn->defined) continue;
        for (int p = fn->bodyStart; p < fn->bodyEnd; p++) {
            if (g_toks[p].kind == T_IDENT && g_toks[p+1].kind == T_LPAREN) {
                int callee = fn_index(g_toks[p].text);
                if (callee >= 0) {
                    if (g_nedges >= MAX_CALL_EDGES) fatal(0, "too many call sites to analyze");
                    g_edges[g_nedges].from = fi;
                    g_edges[g_nedges].to = callee;
                    g_nedges++;
                }
            }
        }
    }
}

static int g_dfs_state[512]; /* 0=unvisited, 1=on stack, 2=done */
static int g_dfs_path[512];
static int g_dfs_depth;

static void report_cycle(int start_at) {
    fprintf(stderr, "cc64: error: recursion is not supported in this version\n");
    fprintf(stderr, "      call cycle: ");
    for (int i = start_at; i < g_dfs_depth; i++)
        fprintf(stderr, "%s -> ", g_funcs[g_dfs_path[i]].name);
    fprintf(stderr, "%s\n", g_funcs[g_dfs_path[start_at]].name);
    exit(1);
}

static void dfs_check(int node) {
    g_dfs_state[node] = 1;
    g_dfs_path[g_dfs_depth++] = node;
    for (int e = 0; e < g_nedges; e++) {
        if (g_edges[e].from != node) continue;
        int to = g_edges[e].to;
        if (g_dfs_state[to] == 1) {
            int start_at = 0;
            for (int i = 0; i < g_dfs_depth; i++) if (g_dfs_path[i] == to) { start_at = i; break; }
            report_cycle(start_at);
        } else if (g_dfs_state[to] == 0) {
            dfs_check(to);
        }
    }
    g_dfs_depth--;
    g_dfs_state[node] = 2;
}

static void check_no_recursion(void) {
    build_call_graph();
    for (int i = 0; i < g_nfuncs; i++) g_dfs_state[i] = 0;
    for (int i = 0; i < g_nfuncs; i++) {
        if (!g_funcs[i].defined) continue;
        if (g_dfs_state[i] == 0) { g_dfs_depth = 0; dfs_check(i); }
    }
}

/* ===================================================================
 * Code generation - output & label helpers
 * =================================================================== */

static FILE *g_out;
static int g_labelctr = 0;
static char *newlabel(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "__L%d", g_labelctr++);
    return xstrdup(buf);
}

/* break/continue label stack */
typedef struct { char *breakLbl; char *contLbl; } LoopCtx;
static LoopCtx g_loops[64];
static int g_nloops = 0;

static void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
    fprintf(g_out, "\n");
}

/* 6502 relative branches only reach +-127 bytes, but an if/while/for
 * body can be arbitrarily long. Always emit the inverted-branch-plus-JMP
 * idiom for any branch that targets a structural label (loop/if end),
 * so code generation never has to know sizes in advance. */
static const char *invert_branch_mnem(const char *m) {
    if (!strcmp(m, "BEQ")) return "BNE";
    if (!strcmp(m, "BNE")) return "BEQ";
    if (!strcmp(m, "BMI")) return "BPL";
    if (!strcmp(m, "BPL")) return "BMI";
    if (!strcmp(m, "BCC")) return "BCS";
    if (!strcmp(m, "BCS")) return "BCC";
    if (!strcmp(m, "BVC")) return "BVS";
    if (!strcmp(m, "BVS")) return "BVC";
    fatal(0, "internal: unknown branch mnemonic '%s'", m);
    return NULL;
}
static void emit_far_branch(const char *mnem, const char *target) {
    char *skip = newlabel();
    emit("    %s %s", invert_branch_mnem(mnem), skip);
    emit("    JMP %s", target);
    emit("%s:", skip);
}

/* storage label for a global */
static void global_label(char *out, size_t outsz, const char *name) {
    snprintf(out, outsz, "__g_%s", name);
}
/* storage label for a local/param of the current function */
static void local_label(char *out, size_t outsz, const char *fn, const char *name) {
    snprintf(out, outsz, "__fn_%s_v_%s", fn, name);
}
static void func_label(char *out, size_t outsz, const char *name) {
    snprintf(out, outsz, "__fn_%s", name);
}

/* ===================================================================
 * Forward decls
 * =================================================================== */

static Node *parse_expr(void);
static Node *parse_assign(void);
static Node *parse_stmt(void);
static Node *parse_block(void);
static void gen_stmt(Node *n);
static void gen_expr_to_R(Node *n);

typedef struct {
    int isArray;
    int type;          /* elem type for arrays; var type for scalars */
    char label[96];    /* scalar: storage label. array: base label */
} LVInfo;

static void resolve_lvalue_base(Node *n, LVInfo *lv);

/* ===================================================================
 * Parser: expressions
 * =================================================================== */

static Node *parse_primary(void) {
    Token *t = cur();
    if (t->kind == T_NUM) {
        advance();
        Node *n = node_new(N_NUM, t->line); n->ival = t->ival; return n;
    }
    if (t->kind == T_CHARLIT) {
        advance();
        Node *n = node_new(N_NUM, t->line); n->ival = t->ival; return n;
    }
    if (t->kind == T_STRLIT) {
        advance();
        Node *n = node_new(N_STR, t->line); n->sval = t->text; return n;
    }
    if (t->kind == T_IDENT) {
        advance();
        if (check(T_LPAREN)) {
            advance();
            Node *call = node_new(N_CALL, t->line);
            call->name = t->text;
            Node *head = NULL, *tail = NULL;
            if (!check(T_RPAREN)) {
                for (;;) {
                    Node *arg = parse_assign();
                    if (!head) head = tail = arg; else { tail->next = arg; tail = arg; }
                    if (check(T_COMMA)) { advance(); continue; }
                    break;
                }
            }
            expect(T_RPAREN, "')'");
            call->a = head;
            return call;
        }
        Node *n = node_new(N_IDENT, t->line); n->name = t->text; return n;
    }
    if (t->kind == T_LPAREN) {
        advance();
        Node *e = parse_expr();
        expect(T_RPAREN, "')'");
        return e;
    }
    fatal(t->line, "unexpected token in expression");
    return NULL;
}

static Node *parse_postfix(void) {
    Node *n = parse_primary();
    for (;;) {
        if (check(T_LBRACKET)) {
            Token *lb = advance();
            Node *idx = parse_expr();
            expect(T_RBRACKET, "']'");
            Node *ix = node_new(N_INDEX, lb->line);
            ix->a = n; ix->b = idx;
            n = ix;
        } else if (check(T_INC)) {
            Token *t = advance();
            Node *p = node_new(N_POSTINC, t->line); p->a = n; n = p;
        } else if (check(T_DEC)) {
            Token *t = advance();
            Node *p = node_new(N_POSTDEC, t->line); p->a = n; n = p;
        } else break;
    }
    return n;
}

static Node *parse_unary(void) {
    if (check(T_MINUS) || check(T_NOT) || check(T_TILDE)) {
        Token *t = advance();
        Node *n = node_new(N_UNARY, t->line);
        n->op[0] = (t->kind == T_MINUS) ? '-' : (t->kind == T_NOT) ? '!' : '~';
        n->a = parse_unary();
        return n;
    }
    if (check(T_INC)) { Token *t = advance(); Node *n = node_new(N_PREINC, t->line); n->a = parse_unary(); return n; }
    if (check(T_DEC)) { Token *t = advance(); Node *n = node_new(N_PREDEC, t->line); n->a = parse_unary(); return n; }
    if (check(T_AMP)) fatal(cur()->line, "address-of ('&') is not supported in this version");
    if (check(T_STAR)) fatal(cur()->line, "pointer dereference ('*') is not supported in this version");
    return parse_postfix();
}

static Node *mkbin(const char *op, Node *a, Node *b, int line) {
    Node *n = node_new(N_BINOP, line);
    snprintf(n->op, sizeof(n->op), "%s", op);
    n->a = a; n->b = b;
    return n;
}

static Node *parse_term(void) {
    Node *n = parse_unary();
    for (;;) {
        if (check(T_STAR)) { Token *t=advance(); n = mkbin("*", n, parse_unary(), t->line); }
        else if (check(T_SLASH)) { Token *t=advance(); n = mkbin("/", n, parse_unary(), t->line); }
        else if (check(T_PERCENT)) { Token *t=advance(); n = mkbin("%", n, parse_unary(), t->line); }
        else break;
    }
    return n;
}
static Node *parse_additive(void) {
    Node *n = parse_term();
    for (;;) {
        if (check(T_PLUS)) { Token *t=advance(); n = mkbin("+", n, parse_term(), t->line); }
        else if (check(T_MINUS)) { Token *t=advance(); n = mkbin("-", n, parse_term(), t->line); }
        else break;
    }
    return n;
}
static Node *parse_shift(void) {
    Node *n = parse_additive();
    for (;;) {
        if (check(T_SHL)) { Token *t=advance(); n = mkbin("<<", n, parse_additive(), t->line); }
        else if (check(T_SHR)) { Token *t=advance(); n = mkbin(">>", n, parse_additive(), t->line); }
        else break;
    }
    return n;
}
static Node *parse_relational(void) {
    Node *n = parse_shift();
    for (;;) {
        if (check(T_LT)) { Token *t=advance(); n = mkbin("<", n, parse_shift(), t->line); }
        else if (check(T_GT)) { Token *t=advance(); n = mkbin(">", n, parse_shift(), t->line); }
        else if (check(T_LE)) { Token *t=advance(); n = mkbin("<=", n, parse_shift(), t->line); }
        else if (check(T_GE)) { Token *t=advance(); n = mkbin(">=", n, parse_shift(), t->line); }
        else break;
    }
    return n;
}
static Node *parse_equality(void) {
    Node *n = parse_relational();
    for (;;) {
        if (check(T_EQ)) { Token *t=advance(); n = mkbin("==", n, parse_relational(), t->line); }
        else if (check(T_NE)) { Token *t=advance(); n = mkbin("!=", n, parse_relational(), t->line); }
        else break;
    }
    return n;
}
static Node *parse_bitand(void) {
    Node *n = parse_equality();
    while (check(T_AMP)) { Token *t=advance(); n = mkbin("&", n, parse_equality(), t->line); }
    return n;
}
static Node *parse_bitxor(void) {
    Node *n = parse_bitand();
    while (check(T_CARET)) { Token *t=advance(); n = mkbin("^", n, parse_bitand(), t->line); }
    return n;
}
static Node *parse_bitor(void) {
    Node *n = parse_bitxor();
    while (check(T_PIPE)) { Token *t=advance(); n = mkbin("|", n, parse_bitxor(), t->line); }
    return n;
}
static Node *parse_logand(void) {
    Node *n = parse_bitor();
    while (check(T_ANDAND)) {
        Token *t=advance();
        Node *r = node_new(N_LOGAND, t->line); r->a = n; r->b = parse_bitor(); n = r;
    }
    return n;
}
static Node *parse_logor(void) {
    Node *n = parse_logand();
    while (check(T_OROR)) {
        Token *t=advance();
        Node *r = node_new(N_LOGOR, t->line); r->a = n; r->b = parse_logand(); n = r;
    }
    return n;
}

static int is_lvalue_node(Node *n) { return n->kind == N_IDENT || n->kind == N_INDEX; }

static Node *parse_assign(void) {
    Node *n = parse_logor();
    TokKind k = cur()->kind;
    static const struct { TokKind k; const char *op; } cops[] = {
        {T_PLUSEQ,"+"},{T_MINUSEQ,"-"},{T_STAREQ,"*"},{T_SLASHEQ,"/"},{T_PERCENTEQ,"%"},
        {T_AMPEQ,"&"},{T_PIPEEQ,"|"},{T_CARETEQ,"^"},{T_SHLEQ,"<<"},{T_SHREQ,">>"}
    };
    if (k == T_ASSIGN) {
        Token *t = advance();
        if (!is_lvalue_node(n)) fatal(t->line, "left side of '=' is not assignable");
        Node *r = node_new(N_ASSIGN, t->line);
        r->a = n; r->b = parse_assign();
        return r;
    }
    for (size_t i = 0; i < sizeof(cops)/sizeof(cops[0]); i++) {
        if (k == cops[i].k) {
            Token *t = advance();
            if (!is_lvalue_node(n)) fatal(t->line, "left side of '%s' is not assignable", cops[i].op);
            Node *r = node_new(N_COMPOUND_ASSIGN, t->line);
            snprintf(r->op, sizeof(r->op), "%s", cops[i].op);
            r->a = n; r->b = parse_assign();
            return r;
        }
    }
    return n;
}

static Node *parse_expr(void) { return parse_assign(); }

/* ===================================================================
 * Parser: statements
 * =================================================================== */

static void register_local(const char *name, int type, int isArray, int arrLen, int isParam, int line) {
    if (find_local(name)) fatal(line, "redefinition of '%s'", name);
    if (g_nlocals >= (int)(sizeof(g_locals)/sizeof(g_locals[0])))
        fatal(line, "too many locals in function '%s'", g_curfn->name);
    LSym *l = &g_locals[g_nlocals++];
    memset(l, 0, sizeof(*l));
    strncpy(l->name, name, sizeof(l->name)-1);
    l->type = type; l->isArray = isArray; l->arrLen = arrLen; l->isParam = isParam;
}

static Node *parse_vardecl(void) {
    Token *tt = cur();
    TokKind tk = advance()->kind; /* int|char, already known to be a type */
    if (check(T_STAR)) fatal(cur()->line, "pointer types are not supported in this version");
    if (!check(T_IDENT)) fatal(cur()->line, "expected identifier");
    char *name = advance()->text;
    Node *n = node_new(N_VARDECL, tt->line);
    n->name = name;
    n->declType = type_from_tok(tk);
    if (check(T_LBRACKET)) {
        advance();
        if (!check(T_NUM)) fatal(cur()->line, "expected array size");
        n->declArrLen = (int)advance()->ival;
        expect(T_RBRACKET, "']'");
    }
    if (check(T_ASSIGN)) {
        advance();
        if (n->declArrLen) fatal(n->line, "array initializers are not supported in this version");
        n->a = parse_assign();
    }
    expect(T_SEMI, "';'");
    register_local(name, n->declType, n->declArrLen != 0, n->declArrLen, 0, n->line);
    return n;
}

static Node *parse_stmt(void) {
    if (check(T_LBRACE)) return parse_block();
    if (check(T_IF)) {
        Token *t = advance();
        expect(T_LPAREN, "'('");
        Node *cond = parse_expr();
        expect(T_RPAREN, "')'");
        Node *thenS = parse_stmt();
        Node *elseS = NULL;
        if (check(T_ELSE)) { advance(); elseS = parse_stmt(); }
        Node *n = node_new(N_IF, t->line);
        n->a = cond; n->b = thenS; n->c = elseS;
        return n;
    }
    if (check(T_WHILE)) {
        Token *t = advance();
        expect(T_LPAREN, "'('");
        Node *cond = parse_expr();
        expect(T_RPAREN, "')'");
        Node *body = parse_stmt();
        Node *n = node_new(N_WHILE, t->line);
        n->a = cond; n->b = body;
        return n;
    }
    if (check(T_FOR)) {
        Token *t = advance();
        expect(T_LPAREN, "'('");
        Node *init = NULL;
        if (!check(T_SEMI)) {
            if (is_type_kw(cur()->kind)) init = parse_vardecl();
            else { init = node_new(N_EXPRSTMT, cur()->line); init->a = parse_expr(); expect(T_SEMI, "';'"); }
        } else advance();
        Node *cond = NULL;
        if (!check(T_SEMI)) cond = parse_expr();
        expect(T_SEMI, "';'");
        Node *incr = NULL;
        if (!check(T_RPAREN)) incr = parse_expr();
        expect(T_RPAREN, "')'");
        Node *body = parse_stmt();
        Node *n = node_new(N_FOR, t->line);
        n->a = init; n->b = cond; n->c = incr; n->d = body;
        return n;
    }
    if (check(T_RETURN)) {
        Token *t = advance();
        Node *n = node_new(N_RETURN, t->line);
        if (!check(T_SEMI)) n->a = parse_expr();
        expect(T_SEMI, "';'");
        return n;
    }
    if (check(T_BREAK)) { Token *t = advance(); expect(T_SEMI, "';'"); return node_new(N_BREAK, t->line); }
    if (check(T_CONTINUE)) { Token *t = advance(); expect(T_SEMI, "';'"); return node_new(N_CONTINUE, t->line); }
    if (check(T_SEMI)) { Token *t = advance(); return node_new(N_EMPTY, t->line); }
    if (is_type_kw(cur()->kind)) return parse_vardecl();
    Node *n = node_new(N_EXPRSTMT, cur()->line);
    n->a = parse_expr();
    expect(T_SEMI, "';'");
    return n;
}

static Node *parse_block(void) {
    Token *t = expect(T_LBRACE, "'{'");
    Node *n = node_new(N_BLOCK, t->line);
    Node *head = NULL, *tail = NULL;
    while (!check(T_RBRACE)) {
        Node *s = parse_stmt();
        if (!head) head = tail = s; else { tail->next = s; tail = s; }
    }
    expect(T_RBRACE, "'}'");
    n->a = head;
    return n;
}

/* ===================================================================
 * Codegen: runtime library & data sections
 * =================================================================== */

static void emit_zp_equates(void) {
    emit("; ---- zero page pseudo-registers -------------------------------");
    emit("__zpSP  = $02     ; soft operand-stack pointer (2 bytes)");
    emit("__zpAP  = $F3     ; effective-address pointer (2 bytes)");
    emit("__zpAP2 = $F5     ; saved-AP scratch, used across RHS evaluation of");
    emit("                  ; assignments/compound-assignments to array elements (2 bytes)");
    emit("__zpT1 = $F7      ; runtime scratch (2 bytes)");
    emit("__zpT0 = $F9      ; runtime scratch (2 bytes)");
    emit("__zpR  = $FB      ; primary register / return value (2 bytes)");
    emit("__zpR2 = $FD      ; secondary operand register (2 bytes)");
    emit("__CHROUT = $FFD2");
    emit(" ");
}

static void emit_runtime(void) {
    emit("; ---- runtime library -------------------------------------------");
    emit("__rt_push:");
    emit("    LDY #0");
    emit("    LDA __zpR");
    emit("    STA (__zpSP),Y");
    emit("    INY");
    emit("    LDA __zpR+1");
    emit("    STA (__zpSP),Y");
    emit("    CLC");
    emit("    LDA __zpSP");
    emit("    ADC #2");
    emit("    STA __zpSP");
    emit("    LDA __zpSP+1");
    emit("    ADC #0");
    emit("    STA __zpSP+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_pop2:");
    emit("    SEC");
    emit("    LDA __zpSP");
    emit("    SBC #2");
    emit("    STA __zpSP");
    emit("    LDA __zpSP+1");
    emit("    SBC #0");
    emit("    STA __zpSP+1");
    emit("    LDY #0");
    emit("    LDA (__zpSP),Y");
    emit("    STA __zpR2");
    emit("    INY");
    emit("    LDA (__zpSP),Y");
    emit("    STA __zpR2+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_mul16:"); /* __zpR2 * __zpR -> __zpR (16x16->16, truncated) */
    emit("    LDA #0");
    emit("    STA __zpT0");
    emit("    STA __zpT0+1");
    emit("    LDX #16");
    emit("__rt_mul16_loop:");
    emit("    LSR __zpR+1");
    emit("    ROR __zpR");
    emit("    BCC __rt_mul16_noadd");
    emit("    CLC");
    emit("    LDA __zpT0");
    emit("    ADC __zpR2");
    emit("    STA __zpT0");
    emit("    LDA __zpT0+1");
    emit("    ADC __zpR2+1");
    emit("    STA __zpT0+1");
    emit("__rt_mul16_noadd:");
    emit("    ASL __zpR2");
    emit("    ROL __zpR2+1");
    emit("    DEX");
    emit("    BNE __rt_mul16_loop");
    emit("    LDA __zpT0");
    emit("    STA __zpR");
    emit("    LDA __zpT0+1");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_udiv16:"); /* unsigned: __zpR2/__zpR -> quotient __zpR2, remainder __zpT0 */
    emit("    LDA #0");
    emit("    STA __zpT0");
    emit("    STA __zpT0+1");
    emit("    LDX #16");
    emit("__rt_udiv16_loop:");
    emit("    ASL __zpR2");
    emit("    ROL __zpR2+1");
    emit("    ROL __zpT0");
    emit("    ROL __zpT0+1");
    emit("    LDA __zpT0");
    emit("    SEC");
    emit("    SBC __zpR");
    emit("    TAY");
    emit("    LDA __zpT0+1");
    emit("    SBC __zpR+1");
    emit("    BCC __rt_udiv16_skip");
    emit("    STA __zpT0+1");
    emit("    STY __zpT0");
    emit("    INC __zpR2");
    emit("__rt_udiv16_skip:");
    emit("    DEX");
    emit("    BNE __rt_udiv16_loop");
    emit("    RTS");
    emit(" ");
    emit("__rt_neg_r2:");
    emit("    SEC");
    emit("    LDA #0");
    emit("    SBC __zpR2");
    emit("    TAX");
    emit("    LDA #0");
    emit("    SBC __zpR2+1");
    emit("    STA __zpR2+1");
    emit("    STX __zpR2");
    emit("    RTS");
    emit(" ");
    emit("__rt_neg_r:");
    emit("    SEC");
    emit("    LDA #0");
    emit("    SBC __zpR");
    emit("    TAX");
    emit("    LDA #0");
    emit("    SBC __zpR+1");
    emit("    STA __zpR+1");
    emit("    STX __zpR");
    emit("    RTS");
    emit(" ");
    emit("__rt_neg_t0:");
    emit("    SEC");
    emit("    LDA #0");
    emit("    SBC __zpT0");
    emit("    TAX");
    emit("    LDA #0");
    emit("    SBC __zpT0+1");
    emit("    STA __zpT0+1");
    emit("    STX __zpT0");
    emit("    RTS");
    emit(" ");
    emit("__rt_sdivmod16:"); /* signed: __zpR2=dividend,__zpR=divisor -> __zpR2=quot, __zpT0=rem */
    emit("    LDA #0");
    emit("    STA __zpT1");
    emit("    STA __zpT1+1");
    emit("    LDA __zpR2+1");
    emit("    BPL __rt_sdm_d1");
    emit("    JSR __rt_neg_r2");
    emit("    INC __zpT1");
    emit("__rt_sdm_d1:");
    emit("    LDA __zpR+1");
    emit("    BPL __rt_sdm_d2");
    emit("    JSR __rt_neg_r");
    emit("    INC __zpT1+1");
    emit("__rt_sdm_d2:");
    emit("    JSR __rt_udiv16");
    emit("    LDA __zpT1");
    emit("    EOR __zpT1+1");
    emit("    AND #1");
    emit("    BEQ __rt_sdm_qdone");
    emit("    JSR __rt_neg_r2");
    emit("__rt_sdm_qdone:");
    emit("    LDA __zpT1");
    emit("    BEQ __rt_sdm_rdone");
    emit("    JSR __rt_neg_t0");
    emit("__rt_sdm_rdone:");
    emit("    RTS");
    emit(" ");
    emit("__rt_eq16:");
    emit("    LDA __zpR2");
    emit("    CMP __zpR");
    emit("    BNE __rt_eq16_ne");
    emit("    LDA __zpR2+1");
    emit("    CMP __zpR+1");
    emit("    BNE __rt_eq16_ne");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_eq16_ne:");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_ne16:");
    emit("    LDA __zpR2");
    emit("    CMP __zpR");
    emit("    BNE __rt_ne16_ne");
    emit("    LDA __zpR2+1");
    emit("    CMP __zpR+1");
    emit("    BNE __rt_ne16_ne");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_ne16_ne:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_sub_r2r:"); /* computes R2-R (signed, overflow-corrected N flag), non-destructive */
    emit("    SEC");
    emit("    LDA __zpR2");
    emit("    SBC __zpR");
    emit("    LDA __zpR2+1");
    emit("    SBC __zpR+1");
    emit("    BVC __rt_sub_r2r_ok");
    emit("    EOR #$80");
    emit("__rt_sub_r2r_ok:");
    emit("    RTS");
    emit(" ");
    emit("__rt_sub_rr2:"); /* computes R-R2 (signed, overflow-corrected N flag) */
    emit("    SEC");
    emit("    LDA __zpR");
    emit("    SBC __zpR2");
    emit("    LDA __zpR+1");
    emit("    SBC __zpR2+1");
    emit("    BVC __rt_sub_rr2_ok");
    emit("    EOR #$80");
    emit("__rt_sub_rr2_ok:");
    emit("    RTS");
    emit(" ");
    emit("__rt_lt16:"); /* R2 < R (signed) */
    emit("    JSR __rt_sub_r2r");
    emit("    BMI __rt_lt16_t");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_lt16_t:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_ge16:"); /* R2 >= R (signed) */
    emit("    JSR __rt_sub_r2r");
    emit("    BPL __rt_ge16_t");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_ge16_t:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_gt16:"); /* R2 > R (signed) */
    emit("    JSR __rt_sub_rr2");
    emit("    BMI __rt_gt16_t");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_gt16_t:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_le16:"); /* R2 <= R (signed) */
    emit("    JSR __rt_sub_rr2");
    emit("    BPL __rt_le16_t");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_le16_t:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_shl16:"); /* __zpR2 << __zpR -> __zpR */
    emit("    LDX __zpR");
    emit("    LDA __zpR2");
    emit("    STA __zpR");
    emit("    LDA __zpR2+1");
    emit("    STA __zpR+1");
    emit("__rt_shl16_loop:");
    emit("    CPX #0");
    emit("    BEQ __rt_shl16_done");
    emit("    ASL __zpR");
    emit("    ROL __zpR+1");
    emit("    DEX");
    emit("    JMP __rt_shl16_loop");
    emit("__rt_shl16_done:");
    emit("    RTS");
    emit(" ");
    emit("__rt_shr16:"); /* __zpR2 >> __zpR -> __zpR (arithmetic) */
    emit("    LDX __zpR");
    emit("    LDA __zpR2");
    emit("    STA __zpR");
    emit("    LDA __zpR2+1");
    emit("    STA __zpR+1");
    emit("__rt_shr16_loop:");
    emit("    CPX #0");
    emit("    BEQ __rt_shr16_done");
    emit("    LDA __zpR+1");
    emit("    CMP #$80");
    emit("    ROR __zpR+1");
    emit("    ROR __zpR");
    emit("    DEX");
    emit("    JMP __rt_shr16_loop");
    emit("__rt_shr16_done:");
    emit("    RTS");
    emit(" ");
    emit("__rt_topetscii:"); /* mirrors c64asm.c's ascii_to_petscii, for runtime chars */
    emit("    LDA __zpR");
    emit("    CMP #$61");
    emit("    BCC __rt_tp_notlower");
    emit("    CMP #$7B");
    emit("    BCS __rt_tp_notlower");
    emit("    SEC");
    emit("    SBC #$20");
    emit("    STA __zpR");
    emit("    JMP __rt_tp_done");
    emit("__rt_tp_notlower:");
    emit("    CMP #$41");
    emit("    BCC __rt_tp_done");
    emit("    CMP #$5B");
    emit("    BCS __rt_tp_done");
    emit("    CLC");
    emit("    ADC #$80");
    emit("    STA __zpR");
    emit("__rt_tp_done:");
    emit("    RTS");
    emit(" ");
}

/* PETSCII mapping mirroring c64asm.c's ascii_to_petscii, for inlined
 * character/string builtin codegen (puts/putchar of literal text). */
static unsigned char to_petscii(unsigned char ch) {
    if (ch >= 'a' && ch <= 'z') return (unsigned char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return (unsigned char)(ch - 'A' + 0xC1);
    return ch;
}

/* ===================================================================
 * Codegen: expressions
 * =================================================================== */

static void gen_load_scalar(const char *label, int type) {
    if (type == TY_CHAR) {
        emit("    LDA %s", label);
        emit("    STA __zpR");
        emit("    LDA #0");
        emit("    STA __zpR+1");
    } else {
        emit("    LDA %s", label);
        emit("    STA __zpR");
        emit("    LDA %s+1", label);
        emit("    STA __zpR+1");
    }
}
static void gen_store_scalar(const char *label, int type) {
    emit("    LDA __zpR");
    emit("    STA %s", label);
    if (type != TY_CHAR) {
        emit("    LDA __zpR+1");
        emit("    STA %s+1", label);
    }
}

/* Resolve a simple-variable or array-index node's storage. For arrays,
 * emits code (clobbering __zpR) that leaves the effective address in
 * __zpAP. lv->isArray tells the caller which access pattern to use. */
static void resolve_lvalue_base(Node *n, LVInfo *lv) {
    memset(lv, 0, sizeof(*lv));
    if (n->kind == N_IDENT) {
        LSym *l = g_curfn ? find_local(n->name) : NULL;
        if (l) {
            if (l->isArray) fatal(n->line, "'%s' is an array; use an index", n->name);
            local_label(lv->label, sizeof(lv->label), g_curfn->name, n->name);
            lv->type = l->type;
            lv->isArray = 0;
            return;
        }
        GSym *g = find_global(n->name);
        if (!g) fatal(n->line, "undeclared identifier '%s'", n->name);
        if (g->isArray) fatal(n->line, "'%s' is an array; use an index", n->name);
        global_label(lv->label, sizeof(lv->label), n->name);
        lv->type = g->type;
        lv->isArray = 0;
        return;
    }
    if (n->kind == N_INDEX) {
        if (n->a->kind != N_IDENT) fatal(n->line, "only simple array names may be indexed");
        char base[96]; int elemType; int arrLen; int isLocalArr = 0;
        LSym *l = g_curfn ? find_local(n->a->name) : NULL;
        if (l) {
            if (!l->isArray) fatal(n->line, "'%s' is not an array", n->a->name);
            local_label(base, sizeof(base), g_curfn->name, n->a->name);
            elemType = l->type; arrLen = l->arrLen; isLocalArr = 1;
        } else {
            GSym *g = find_global(n->a->name);
            if (!g) fatal(n->line, "undeclared identifier '%s'", n->a->name);
            if (!g->isArray) fatal(n->line, "'%s' is not an array", n->a->name);
            global_label(base, sizeof(base), n->a->name);
            elemType = g->type; arrLen = g->arrLen;
        }
        (void)arrLen; (void)isLocalArr;
        gen_expr_to_R(n->b); /* index -> __zpR */
        if (elemType == TY_INT) {
            emit("    ASL __zpR");
            emit("    ROL __zpR+1");
        }
        emit("    CLC");
        emit("    LDA __zpR");
        emit("    ADC #<%s", base);
        emit("    STA __zpAP");
        emit("    LDA __zpR+1");
        emit("    ADC #>%s", base);
        emit("    STA __zpAP+1");
        lv->isArray = 1;
        lv->type = elemType;
        return;
    }
    fatal(n->line, "expression is not assignable");
}

static void gen_lv_load_to_R(LVInfo *lv) {
    if (!lv->isArray) { gen_load_scalar(lv->label, lv->type); return; }
    emit("    LDY #0");
    emit("    LDA (__zpAP),Y");
    emit("    STA __zpR");
    if (lv->type == TY_INT) {
        emit("    INY");
        emit("    LDA (__zpAP),Y");
        emit("    STA __zpR+1");
    } else {
        emit("    LDA #0");
        emit("    STA __zpR+1");
    }
}
static void gen_lv_store_from_R(LVInfo *lv) {
    if (!lv->isArray) { gen_store_scalar(lv->label, lv->type); return; }
    emit("    LDY #0");
    emit("    LDA __zpR");
    emit("    STA (__zpAP),Y");
    if (lv->type == TY_INT) {
        emit("    INY");
        emit("    LDA __zpR+1");
        emit("    STA (__zpAP),Y");
    }
}

static void gen_binop(const char *op) {
    if (strcmp(op, "+") == 0) {
        emit("    CLC");
        emit("    LDA __zpR2");
        emit("    ADC __zpR");
        emit("    STA __zpR");
        emit("    LDA __zpR2+1");
        emit("    ADC __zpR+1");
        emit("    STA __zpR+1");
    } else if (strcmp(op, "-") == 0) {
        emit("    SEC");
        emit("    LDA __zpR2");
        emit("    SBC __zpR");
        emit("    STA __zpR2"); /* stash low result */
        emit("    LDA __zpR2+1");
        emit("    SBC __zpR+1");
        emit("    STA __zpR+1");
        emit("    LDA __zpR2");
        emit("    STA __zpR");
    } else if (strcmp(op, "*") == 0) {
        emit("    JSR __rt_mul16");
    } else if (strcmp(op, "/") == 0) {
        emit("    JSR __rt_sdivmod16");
        emit("    LDA __zpR2");
        emit("    STA __zpR");
        emit("    LDA __zpR2+1");
        emit("    STA __zpR+1");
    } else if (strcmp(op, "%") == 0) {
        emit("    JSR __rt_sdivmod16");
        emit("    LDA __zpT0");
        emit("    STA __zpR");
        emit("    LDA __zpT0+1");
        emit("    STA __zpR+1");
    } else if (strcmp(op, "&") == 0) {
        emit("    LDA __zpR2");
        emit("    AND __zpR");
        emit("    STA __zpR");
        emit("    LDA __zpR2+1");
        emit("    AND __zpR+1");
        emit("    STA __zpR+1");
    } else if (strcmp(op, "|") == 0) {
        emit("    LDA __zpR2");
        emit("    ORA __zpR");
        emit("    STA __zpR");
        emit("    LDA __zpR2+1");
        emit("    ORA __zpR+1");
        emit("    STA __zpR+1");
    } else if (strcmp(op, "^") == 0) {
        emit("    LDA __zpR2");
        emit("    EOR __zpR");
        emit("    STA __zpR");
        emit("    LDA __zpR2+1");
        emit("    EOR __zpR+1");
        emit("    STA __zpR+1");
    } else if (strcmp(op, "<<") == 0) {
        emit("    JSR __rt_shl16");
    } else if (strcmp(op, ">>") == 0) {
        emit("    JSR __rt_shr16");
    } else if (strcmp(op, "==") == 0) { emit("    JSR __rt_eq16"); }
    else if (strcmp(op, "!=") == 0) { emit("    JSR __rt_ne16"); }
    else if (strcmp(op, "<") == 0) { emit("    JSR __rt_lt16"); }
    else if (strcmp(op, ">") == 0) { emit("    JSR __rt_gt16"); }
    else if (strcmp(op, "<=") == 0) { emit("    JSR __rt_le16"); }
    else if (strcmp(op, ">=") == 0) { emit("    JSR __rt_ge16"); }
    else fatal(0, "internal: unknown operator '%s'", op);
}

static void gen_call(Node *n) {
    if (strcmp(n->name, "putchar") == 0) {
        int cnt = 0; for (Node *a = n->a; a; a = a->next) cnt++;
        if (cnt != 1) fatal(n->line, "putchar() takes exactly 1 argument");
        gen_expr_to_R(n->a);
        emit("    JSR __rt_topetscii");
        emit("    LDA __zpR");
        emit("    JSR __CHROUT");
        return;
    }
    if (strcmp(n->name, "puts") == 0) {
        int cnt = 0; for (Node *a = n->a; a; a = a->next) cnt++;
        if (cnt != 1 || n->a->kind != N_STR)
            fatal(n->line, "puts() requires exactly one string-literal argument in this version");
        for (const char *p = n->a->sval; *p; p++) {
            unsigned char pc = to_petscii((unsigned char)*p);
            emit("    LDA #$%02X", pc);
            emit("    JSR __CHROUT");
        }
        emit("    LDA __zpR"); /* leave R defined (value irrelevant, puts is void) */
        return;
    }
    if (strcmp(n->name, "peek") == 0) {
        int cnt = 0; for (Node *a = n->a; a; a = a->next) cnt++;
        if (cnt != 1) fatal(n->line, "peek() takes exactly 1 argument");
        gen_expr_to_R(n->a);
        emit("    LDA __zpR");
        emit("    STA __zpAP");
        emit("    LDA __zpR+1");
        emit("    STA __zpAP+1");
        emit("    LDY #0");
        emit("    LDA (__zpAP),Y");
        emit("    STA __zpR");
        emit("    LDA #0");
        emit("    STA __zpR+1");
        return;
    }
    if (strcmp(n->name, "poke") == 0) {
        int cnt = 0; for (Node *a = n->a; a; a = a->next) cnt++;
        if (cnt != 2) fatal(n->line, "poke() takes exactly 2 arguments (addr, value)");
        gen_expr_to_R(n->a);
        emit("    LDA __zpR");
        emit("    STA __zpAP");
        emit("    LDA __zpR+1");
        emit("    STA __zpAP+1");
        emit("    LDA __zpAP");
        emit("    STA __zpAP2");
        emit("    LDA __zpAP+1");
        emit("    STA __zpAP2+1");
        gen_expr_to_R(n->a->next); /* may clobber __zpAP if it touches an array */
        emit("    LDA __zpAP2");
        emit("    STA __zpAP");
        emit("    LDA __zpAP2+1");
        emit("    STA __zpAP+1");
        emit("    LDY #0");
        emit("    LDA __zpR");
        emit("    STA (__zpAP),Y");
        return;
    }

    FnSym *fn = find_func(n->name);
    if (!fn) fatal(n->line, "call to undeclared function '%s'", n->name);
    if (!fn->defined) fatal(n->line, "call to function '%s' which is declared but never defined", n->name);
    int cnt = 0; for (Node *a = n->a; a; a = a->next) cnt++;
    if (cnt != fn->nparams)
        fatal(n->line, "function '%s' expects %d argument(s), got %d", n->name, fn->nparams, cnt);
    Node *a = n->a;
    for (int i = 0; i < fn->nparams; i++, a = a->next) {
        gen_expr_to_R(a);
        char lbl[96];
        local_label(lbl, sizeof(lbl), fn->name, fn->paramNames[i]);
        gen_store_scalar(lbl, fn->paramTypes[i]);
    }
    char flbl[96];
    func_label(flbl, sizeof(flbl), fn->name);
    emit("    JSR %s", flbl);
}

static void gen_incdec(Node *n, int isInc, int isPost) {
    LVInfo lv;
    resolve_lvalue_base(n->a, &lv);
    gen_lv_load_to_R(&lv);
    if (isPost) {
        emit("    LDA __zpR");
        emit("    STA __zpR2");
        emit("    LDA __zpR+1");
        emit("    STA __zpR2+1");
    }
    if (isInc) {
        emit("    CLC");
        emit("    LDA __zpR");
        emit("    ADC #1");
        emit("    STA __zpR");
        emit("    LDA __zpR+1");
        emit("    ADC #0");
        emit("    STA __zpR+1");
    } else {
        emit("    SEC");
        emit("    LDA __zpR");
        emit("    SBC #1");
        emit("    STA __zpR");
        emit("    LDA __zpR+1");
        emit("    SBC #0");
        emit("    STA __zpR+1");
    }
    /* if the lvalue is an array element, __zpAP is still valid (index
     * expr had no chance to run again since it was only evaluated once
     * inside resolve_lvalue_base) */
    gen_lv_store_from_R(&lv);
    if (isPost) {
        emit("    LDA __zpR2");
        emit("    STA __zpR");
        emit("    LDA __zpR2+1");
        emit("    STA __zpR+1");
    }
}

static void gen_expr_to_R(Node *n) {
    switch (n->kind) {
        case N_NUM:
            emit("    LDA #<%ld", n->ival);
            emit("    STA __zpR");
            emit("    LDA #>%ld", n->ival);
            emit("    STA __zpR+1");
            return;
        case N_STR:
            fatal(n->line, "string literals may only be used as puts()'s argument in this version");
            return;
        case N_IDENT: {
            LVInfo lv; resolve_lvalue_base(n, &lv); gen_lv_load_to_R(&lv);
            return;
        }
        case N_INDEX: {
            LVInfo lv; resolve_lvalue_base(n, &lv); gen_lv_load_to_R(&lv);
            return;
        }
        case N_CALL:
            gen_call(n);
            return;
        case N_ASSIGN: {
            LVInfo lv; resolve_lvalue_base(n->a, &lv);
            if (lv.isArray) {
                emit("    LDA __zpAP");
                emit("    STA __zpAP2");
                emit("    LDA __zpAP+1");
                emit("    STA __zpAP2+1");
            }
            gen_expr_to_R(n->b); /* rhs may itself use __zpAP; restore afterward */
            if (lv.isArray) {
                emit("    LDA __zpAP2");
                emit("    STA __zpAP");
                emit("    LDA __zpAP2+1");
                emit("    STA __zpAP+1");
            }
            gen_lv_store_from_R(&lv);
            return;
        }
        case N_COMPOUND_ASSIGN: {
            LVInfo lv; resolve_lvalue_base(n->a, &lv);
            if (lv.isArray) {
                emit("    LDA __zpAP");
                emit("    STA __zpAP2");
                emit("    LDA __zpAP+1");
                emit("    STA __zpAP2+1");
            }
            gen_lv_load_to_R(&lv);     /* current value -> R (uses __zpAP, still valid here) */
            emit("    JSR __rt_push"); /* save current value */
            gen_expr_to_R(n->b);       /* rhs -> R; may clobber __zpAP */
            emit("    JSR __rt_pop2"); /* __zpR2 = old value, __zpR = rhs value */
            gen_binop(n->op);
            if (lv.isArray) {
                emit("    LDA __zpAP2");
                emit("    STA __zpAP");
                emit("    LDA __zpAP2+1");
                emit("    STA __zpAP+1");
            }
            gen_lv_store_from_R(&lv);
            return;
        }
        case N_BINOP: {
            gen_expr_to_R(n->a);
            emit("    JSR __rt_push");
            gen_expr_to_R(n->b);
            emit("    JSR __rt_pop2");
            gen_binop(n->op);
            return;
        }
        case N_LOGAND: {
            char *Lfalse = newlabel(); char *Ldone = newlabel();
            gen_expr_to_R(n->a);
            emit("    LDA __zpR");
            emit("    ORA __zpR+1");
            emit_far_branch("BEQ", Lfalse);
            gen_expr_to_R(n->b);
            emit("    LDA __zpR");
            emit("    ORA __zpR+1");
            emit_far_branch("BEQ", Lfalse);
            emit("    LDA #1");
            emit("    STA __zpR");
            emit("    LDA #0");
            emit("    STA __zpR+1");
            emit("    JMP %s", Ldone);
            emit("%s:", Lfalse);
            emit("    LDA #0");
            emit("    STA __zpR");
            emit("    STA __zpR+1");
            emit("%s:", Ldone);
            return;
        }
        case N_LOGOR: {
            char *Ltrue = newlabel(); char *Ldone = newlabel();
            gen_expr_to_R(n->a);
            emit("    LDA __zpR");
            emit("    ORA __zpR+1");
            emit_far_branch("BNE", Ltrue);
            gen_expr_to_R(n->b);
            emit("    LDA __zpR");
            emit("    ORA __zpR+1");
            emit_far_branch("BNE", Ltrue);
            emit("    LDA #0");
            emit("    STA __zpR");
            emit("    STA __zpR+1");
            emit("    JMP %s", Ldone);
            emit("%s:", Ltrue);
            emit("    LDA #1");
            emit("    STA __zpR");
            emit("    LDA #0");
            emit("    STA __zpR+1");
            emit("%s:", Ldone);
            return;
        }
        case N_UNARY: {
            gen_expr_to_R(n->a);
            if (n->op[0] == '-') {
                emit("    SEC");
                emit("    LDA #0");
                emit("    SBC __zpR");
                emit("    TAX");
                emit("    LDA #0");
                emit("    SBC __zpR+1");
                emit("    STA __zpR+1");
                emit("    STX __zpR");
            } else if (n->op[0] == '~') {
                emit("    LDA __zpR");
                emit("    EOR #$FF");
                emit("    STA __zpR");
                emit("    LDA __zpR+1");
                emit("    EOR #$FF");
                emit("    STA __zpR+1");
            } else { /* ! */
                char *Lz = newlabel(); char *Ld = newlabel();
                emit("    LDA __zpR");
                emit("    ORA __zpR+1");
                emit_far_branch("BEQ", Lz);
                emit("    LDA #0");
                emit("    STA __zpR");
                emit("    STA __zpR+1");
                emit("    JMP %s", Ld);
                emit("%s:", Lz);
                emit("    LDA #1");
                emit("    STA __zpR");
                emit("    LDA #0");
                emit("    STA __zpR+1");
                emit("%s:", Ld);
            }
            return;
        }
        case N_PREINC: gen_incdec(n, 1, 0); return;
        case N_PREDEC: gen_incdec(n, 0, 0); return;
        case N_POSTINC: gen_incdec(n, 1, 1); return;
        case N_POSTDEC: gen_incdec(n, 0, 1); return;
        default:
            fatal(n->line, "internal: cannot generate expression node %d", n->kind);
    }
}

/* ===================================================================
 * Codegen: statements
 * =================================================================== */

static void gen_stmt(Node *n) {
    switch (n->kind) {
        case N_BLOCK:
            for (Node *s = n->a; s; s = s->next) gen_stmt(s);
            return;
        case N_VARDECL:
            if (n->a) { gen_expr_to_R(n->a);
                char lbl[96]; local_label(lbl, sizeof(lbl), g_curfn->name, n->name);
                gen_store_scalar(lbl, n->declType);
            }
            return;
        case N_EXPRSTMT:
            gen_expr_to_R(n->a);
            return;
        case N_IF: {
            char *Lelse = newlabel(); char *Lend = newlabel();
            gen_expr_to_R(n->a);
            emit("    LDA __zpR");
            emit("    ORA __zpR+1");
            emit_far_branch("BEQ", Lelse);
            gen_stmt(n->b);
            emit("    JMP %s", Lend);
            emit("%s:", Lelse);
            if (n->c) gen_stmt(n->c);
            emit("%s:", Lend);
            return;
        }
        case N_WHILE: {
            char *Ltop = newlabel(); char *Lend = newlabel();
            if (g_nloops >= 64) fatal(n->line, "loops nested too deeply");
            g_loops[g_nloops].breakLbl = Lend; g_loops[g_nloops].contLbl = Ltop; g_nloops++;
            emit("%s:", Ltop);
            gen_expr_to_R(n->a);
            emit("    LDA __zpR");
            emit("    ORA __zpR+1");
            emit_far_branch("BEQ", Lend);
            gen_stmt(n->b);
            emit("    JMP %s", Ltop);
            emit("%s:", Lend);
            g_nloops--;
            return;
        }
        case N_FOR: {
            char *Ltop = newlabel(); char *Lend = newlabel(); char *Lincr = newlabel();
            if (n->a) gen_stmt(n->a);
            if (g_nloops >= 64) fatal(n->line, "loops nested too deeply");
            g_loops[g_nloops].breakLbl = Lend; g_loops[g_nloops].contLbl = Lincr; g_nloops++;
            emit("%s:", Ltop);
            if (n->b) {
                gen_expr_to_R(n->b);
                emit("    LDA __zpR");
                emit("    ORA __zpR+1");
                emit_far_branch("BEQ", Lend);
            }
            gen_stmt(n->d);
            emit("%s:", Lincr);
            if (n->c) gen_expr_to_R(n->c);
            emit("    JMP %s", Ltop);
            emit("%s:", Lend);
            g_nloops--;
            return;
        }
        case N_RETURN:
            if (n->a) gen_expr_to_R(n->a);
            emit("    RTS");
            return;
        case N_BREAK:
            if (g_nloops == 0) fatal(n->line, "'break' outside a loop");
            emit("    JMP %s", g_loops[g_nloops-1].breakLbl);
            return;
        case N_CONTINUE:
            if (g_nloops == 0) fatal(n->line, "'continue' outside a loop");
            emit("    JMP %s", g_loops[g_nloops-1].contLbl);
            return;
        case N_EMPTY:
            return;
        default:
            fatal(n->line, "internal: cannot generate statement node %d", n->kind);
    }
}

/* ===================================================================
 * Pass B: full parse + codegen
 * =================================================================== */

static void emit_global_storage(void) {
    emit("; ---- global variables --------------------------------------------");
    for (int i = 0; i < g_nglobals; i++) {
        GSym *g = &g_globals[i];
        char lbl[96]; global_label(lbl, sizeof(lbl), g->name);
        if (g->isArray) {
            int bytes = g->arrLen * (g->type == TY_INT ? 2 : 1);
            emit("%s:", lbl);
            emit("    .fill %d, 0", bytes);
        } else if (g->hasInit) {
            emit("%s:", lbl);
            if (g->type == TY_INT) emit("    .word %ld", g->initVal);
            else emit("    .byte %ld", g->initVal & 0xFF ? (g->initVal & 0xFF) : 0);
        } else {
            emit("%s:", lbl);
            emit("    .fill %d, 0", g->type == TY_INT ? 2 : 1);
        }
    }
    emit(" ");
}

static void emit_function(FnSym *fn, Node *body) {
    char flbl[96]; func_label(flbl, sizeof(flbl), fn->name);
    emit("; ---- function %s ---------------------------------------------", fn->name);
    for (int i = 0; i < fn->nparams; i++) {
        char lbl[96]; local_label(lbl, sizeof(lbl), fn->name, fn->paramNames[i]);
        emit("%s:", lbl);
        emit("    .fill %d, 0", fn->paramTypes[i] == TY_INT ? 2 : 1);
    }
    for (int i = 0; i < g_nlocals; i++) {
        if (g_locals[i].isParam) continue;
        char lbl[96]; local_label(lbl, sizeof(lbl), fn->name, g_locals[i].name);
        int bytes = g_locals[i].isArray
            ? g_locals[i].arrLen * (g_locals[i].type == TY_INT ? 2 : 1)
            : (g_locals[i].type == TY_INT ? 2 : 1);
        emit("%s:", lbl);
        emit("    .fill %d, 0", bytes);
    }
    emit("%s:", flbl);
    gen_stmt(body);
    emit("    RTS"); /* fallback for functions that fall off the end */
    emit(" ");
}

/* pre-scan a parsed function body for local var decls so that forward
 * references within the same function (e.g. a decl used before its
 * textual point due to control flow) still resolve; since we register
 * locals as we parse (register_local is called from parse_vardecl),
 * and parsing happens fully before codegen for each function, all
 * locals are known by the time gen_stmt runs. */

static void pass_b(void) {
    g_pos = 0;
    while (!check(T_EOF)) {
        TokKind tk = advance()->kind; /* type */
        if (check(T_STAR)) fatal(cur()->line, "internal"); /* already rejected in pass A */
        char *name = advance()->text; /* ident */

        if (check(T_LPAREN)) {
            advance();
            FnSym *fn = find_func(name);
            /* skip parameter list tokens (already recorded) */
            int depth = 1;
            while (depth > 0) {
                if (check(T_LPAREN)) depth++;
                else if (check(T_RPAREN)) depth--;
                advance();
            }
            if (check(T_SEMI)) { advance(); continue; }
            /* function body */
            g_nlocals = 0;
            g_curfn = fn;
            for (int i = 0; i < fn->nparams; i++)
                register_local(fn->paramNames[i], fn->paramTypes[i], 0, 0, 1, 0);
            Node *body = parse_block();
            emit_function(fn, body);
            g_curfn = NULL;
            continue;
        }

        /* global var - already recorded by pass A; just skip its tokens */
        (void)tk;
        if (check(T_LBRACKET)) { advance(); advance(); expect(T_RBRACKET, "']'"); }
        if (check(T_ASSIGN)) { advance(); if (check(T_MINUS)) advance(); advance(); }
        expect(T_SEMI, "';'");
    }
}

/* ===================================================================
 * main
 * =================================================================== */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cc64: cannot open '%s'\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc(sz + 1);
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static void usage(const char *prog) {
    fprintf(stderr, "cc64 - a minimal C compiler for the Commodore 64 (6502 target)\n\n");
    fprintf(stderr, "Usage: %s <input.c> -o <output.asm>\n", prog);
}

int main(int argc, char **argv) {
    const char *input_path = NULL, *output_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            output_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        } else if (argv[i][0] != '-' && !input_path) {
            input_path = argv[i];
        } else {
            fprintf(stderr, "cc64: unrecognized argument '%s'\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }
    if (!input_path || !output_path) { usage(argv[0]); return 1; }

    char *src = read_file(input_path);
    lex(src);

    pass_a();
    if (!find_func("main")) fatal(0, "no 'main' function defined");
    if (find_func("main")->nparams != 0) fatal(0, "'main' must take no parameters in this version");
    check_no_recursion();

    g_out = fopen(output_path, "w");
    if (!g_out) { fprintf(stderr, "cc64: cannot open output '%s'\n", output_path); return 1; }

    emit("; Generated by cc64 from %s", input_path);
    emit("; Assemble with: c64asm %s -o program.prg", output_path);
    emit(" ");
    emit_zp_equates();
    emit(".basic");
    emit("    LDA #14           ; switch to the lower/upper character set - required");
    emit("    JSR __CHROUT      ; for the PETSCII case mapping below to render as letters");
    emit("                      ; (the default charset only has graphics past $C0)");
    emit("    LDA #<__rt_stack");
    emit("    STA __zpSP");
    emit("    LDA #>__rt_stack");
    emit("    STA __zpSP+1");
    emit("    JSR __fn_main");
    emit("    RTS");
    emit(" ");
    emit_runtime();
    emit_global_storage();

    pass_b();

    emit("; ---- operand stack (grows upward, 256 slots) ----------------------");
    emit("__rt_stack:");
    emit("    .fill 512, 0");

    fclose(g_out);
    fprintf(stderr, "cc64: wrote %s\n", output_path);
    return 0;
}
