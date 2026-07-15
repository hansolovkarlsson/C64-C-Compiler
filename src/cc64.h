/*
 * cc64.h - shared type definitions and cross-module declarations.
 *
 * =======================================================================
 * HOW THIS COMPILER IS ORGANIZED
 * =======================================================================
 * cc64 is a small, textbook-style compiler, split into files that each
 * correspond to one classic "phase" you'd see named in a compilers
 * course. If you're new to how compilers are built, this is a
 * reasonable order to read the source in:
 *
 *   1. lexer.c        Source text -> a flat array of Tokens.
 *   2. ast.c           Defines the tree shape parsing builds (Node).
 *   3. symtab.c        Symbol tables (what's declared, and as what
 *                      type) plus a tiny type inference helper used
 *                      for pointer arithmetic and light type-checking.
 *   4. parser.c        Tokens -> an AST, using recursive descent. Also
 *                      drives codegen function-by-function (see the
 *                      "two-pass" note below).
 *   5. codegen.c       Small shared codegen utilities used by both
 *                      codegen_expr.c and codegen_stmt.c.
 *   6. codegen_runtime.c  The fixed 6502 runtime library (multiply,
 *                      divide, comparisons, string printing, ...)
 *                      that every compiled program links against.
 *   7. codegen_expr.c  Turns an expression AST node into 6502
 *                      instructions that leave the result in a
 *                      "register" (see ARCHITECTURE below).
 *   8. codegen_stmt.c  Turns a statement AST node into 6502
 *                      instructions; also emits the storage layout
 *                      for globals and whole functions, and the
 *                      per-function frame save/restore routines that
 *                      make recursion work (see below).
 *   9. main.c          Command-line driver: read file -> lex -> parse
 *                      (which also triggers codegen) -> write output.
 *
 * WHY TWO PASSES OVER THE SOURCE
 * -------------------------------
 * pass_a() (in parser.c) does a *fast, shallow* scan of the whole file,
 * recording every function's signature and every global's type, but
 * without parsing any function body or generating any code. pass_b()
 * then does a *real* recursive-descent parse of each function body,
 * generating code as it goes. Splitting it this way means a function
 * can call another function that's defined later in the file, or a
 * global can be used before its declaration textually appears - pass_a
 * already knows about it by the time pass_b gets there. Compilers for
 * languages that require forward declarations (like older C) usually
 * do this in one pass; this two-pass design trades a bit of extra
 * scanning for a friendlier language to write in.
 *
 * ARCHITECTURE: WHY EVERYTHING FLOWS THROUGH "REGISTERS"
 * --------------------------------------------------------
 * The 6502 has no general-purpose registers beyond three 8-bit ones
 * (A, X, Y), so this compiler defines its own convention: __zpR is
 * the "primary register" - a 16-bit value living in two bytes of
 * zero page RAM - and every expression's value ends up there when
 * codegen_expr.c is done with it. Binary operators combine __zpR
 * (the right operand) with __zpR2 (the left operand, saved on a
 * small software stack while the right operand was being computed).
 * See the comment above emit_runtime() in codegen_runtime.c, and the
 * comment above gen_expr_to_R() in codegen_expr.c, for the details.
 *
 * HOW RECURSION WORKS (GIVEN FIXED-ADDRESS STORAGE)
 * ---------------------------------------------------
 * Every function's parameters and locals get FIXED, statically-
 * allocated memory (like very old non-reentrant compilers used)
 * instead of a conventional per-call stack frame. That keeps the
 * common case simple and fast: a function reading or writing its own
 * variable is a plain absolute load/store, with no frame pointer and
 * no stack-relative addressing anywhere in expression codegen.
 * Recursion is layered ON TOP of that model rather than replacing
 * it: every call site saves the callee's current frame contents to a
 * software call stack before overwriting them with new arguments,
 * and restores them after the call returns - so when a recursive
 * chain unwinds, each level finds its variables exactly as it left
 * them, even though every level used the same addresses. The full
 * design (and a worked factorial example) is in the long comment
 * above emit_function() in codegen_stmt.c; the runtime pieces it
 * relies on are described above __rt_cstack_check in
 * codegen_runtime.c.
 * =======================================================================
 */

#ifndef CC64_H
#define CC64_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* ===================================================================
 * Tokens (produced by lexer.c, consumed by parser.c)
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

/* ===================================================================
 * AST (built by parser.c, walked by codegen_expr.c / codegen_stmt.c
 * and by the read-only scans in symtab.c's infer_type() and
 * infer_type())
 * =================================================================== */

typedef enum {
    N_NUM, N_STR, N_IDENT, N_CALL, N_INDEX,
    N_ASSIGN, N_COMPOUND_ASSIGN, N_BINOP, N_LOGAND, N_LOGOR,
    N_UNARY, N_PREINC, N_PREDEC, N_POSTINC, N_POSTDEC,
    N_ADDR, N_DEREF,
    N_BLOCK, N_IF, N_WHILE, N_FOR, N_RETURN, N_BREAK, N_CONTINUE,
    N_EXPRSTMT, N_VARDECL, N_EMPTY
} NodeKind;

/* One node type for every kind of expression and statement, rather
 * than a separate struct per kind. This "everything is a Node" style
 * costs a little memory (every node has fields it doesn't use) but
 * keeps the parser and codegen simple - no need for a tagged union or
 * a family of related struct types. `a`/`b`/`c`/`d` are a node's
 * children (their meaning depends on `kind` - e.g. for N_IF, a=cond,
 * b=then-branch, c=else-branch); `next` chains siblings in a list
 * (statements in a block, arguments in a call, ...). */
typedef struct Node {
    NodeKind kind;
    char op[3];         /* operator text for N_BINOP/N_COMPOUND_ASSIGN/N_UNARY */
    char *name;         /* identifier / function name */
    long ival;           /* literal value, for N_NUM */
    char *sval;          /* string contents, for N_STR */
    struct Node *a, *b, *c, *d;
    struct Node *next;
    int declType;        /* N_VARDECL: 0=char, 1=int (see TY_CHAR/TY_INT) */
    int declIsPointer;   /* N_VARDECL: declared as pointer-to-declType */
    int declArrLen;      /* N_VARDECL: 0 = scalar, else array length */
    int line;
} Node;

Node *node_new(NodeKind k, int line);

/* ===================================================================
 * Symbol tables (owned by symtab.c; populated by parser.c; read by
 * codegen_expr.c and codegen_stmt.c to decide how much storage a
 * variable needs and how to address it)
 * =================================================================== */

#define TY_CHAR 0
#define TY_INT  1

/* A global variable's declared shape. Globals live at a fixed,
 * compile-time-known address (a label), so - unlike locals - they
 * don't need a "which function owns this" association. */
typedef struct {
    char name[64];
    int type;         /* TY_CHAR or TY_INT */
    int isPointer;
    int isArray;
    int arrLen;
    long initVal;      /* for globals with a constant scalar initializer */
    int hasInit;
} GSym;

/* A function's signature - name, return type, and its parameters. */
typedef struct {
    char name[64];
    int retType;       /* TY_CHAR, TY_INT, or -1 for void */
    int retIsPointer;
    int nparams;
    int paramTypes[32];
    int paramIsPointer[32];
    char paramNames[32][64];
    int defined;         /* has a body been seen (not just a prototype)? */
} FnSym;

/* A local variable or parameter's declared shape, scoped to whichever
 * function is currently being parsed/compiled (see g_curfn below). */
typedef struct {
    char name[64];
    int type;
    int isPointer;
    int isArray;
    int arrLen;
    int isParam;
} LSym;

extern GSym g_globals[1024];
extern int g_nglobals;
extern FnSym g_funcs[512];
extern int g_nfuncs;
extern LSym g_locals[256];
extern int g_nlocals;
extern FnSym *g_curfn; /* NULL at file scope; set while compiling one function's body */

GSym *find_global(const char *name);
FnSym *find_func(const char *name);
LSym *find_local(const char *name);
int is_builtin(const char *name);
void register_local(const char *name, int type, int isPointer, int isArray,
                     int arrLen, int isParam, int line);

/* A minimal type descriptor: just enough to know whether an
 * expression's value is a pointer, and to what base type, since
 * that's all pointer arithmetic scaling and the light type checks
 * need. Not a full type system - see infer_type()'s own comment in
 * symtab.c for what it deliberately doesn't try to do. */
typedef struct { int base; int isPointer; } CType;

CType infer_type(Node *n);
int var_width(int type, int isPointer); /* 1 or 2 bytes, for storage sizing */

/* ===================================================================
 * Tokens produced by the lexer (owned by lexer.c; read by parser.c
 * via the shared cursor position)
 * =================================================================== */

extern Token *g_toks;
extern int g_ntoks;

void lex(const char *src); /* fills g_toks/g_ntoks from source text */

/* ===================================================================
 * Parsing and semantic checks
 * =================================================================== */

void pass_a(void);          /* shallow pre-scan: signatures & globals only */
void pass_b(void);          /* real parse of each function body + codegen */

/* ===================================================================
 * Code generation - shared infrastructure (codegen.c)
 * =================================================================== */

extern FILE *g_out; /* the currently-open .asm output file */

void emit(const char *fmt, ...);          /* write one line of assembly */
char *newlabel(void);                      /* a fresh, unique "__L<N>" label */
void emit_far_branch(const char *mnem, const char *target); /* see comment in codegen.c */
void global_label(char *out, size_t outsz, const char *name);
void local_label(char *out, size_t outsz, const char *fn, const char *name);
void func_label(char *out, size_t outsz, const char *name);

/* ===================================================================
 * Code generation - the fixed runtime library (codegen_runtime.c)
 * =================================================================== */

void emit_zp_equates(void);  /* zero-page register layout, as assembler constants */
void emit_runtime(void);     /* multiply/divide/compare/print routines, in 6502 asm */
char *intern_string(const char *s);  /* records a string literal, returns its label */
void emit_string_literals(void);      /* emits the recorded literals as raw byte data */

/* ===================================================================
 * Code generation - expressions and statements
 * =================================================================== */

void gen_expr_to_R(Node *n);  /* codegen_expr.c: evaluate an expression into __zpR */
void gen_store_scalar(const char *label, int type, int isPointer); /* codegen_expr.c */
void emit_function(FnSym *fn, Node *body);   /* codegen_stmt.c */
void emit_global_storage(void);               /* codegen_stmt.c */

/* ===================================================================
 * Generic utilities (util.c)
 * =================================================================== */

void fatal(int line, const char *fmt, ...); /* prints "cc64: error: ..." and exit(1)s */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

#endif /* CC64_H */
