/*
 * parser.c - turns the token array from lexer.c into an AST (parser.c
 * builds trees of the Node type from cc64.h), and drives the whole
 * two-pass compilation strategy described in cc64.h's overview.
 *
 * ===================================================================
 * RECURSIVE DESCENT, IN ONE PARAGRAPH
 * ===================================================================
 * This parser is "recursive descent": one function per grammar rule,
 * each calling the functions for the rules nested inside it. `if (E)
 * S` is parsed by parse_stmt() calling parse_expr() for E and
 * parse_stmt() (yes, itself) for S. There's no parser-generator, no
 * table of states - the call graph of these functions directly
 * mirrors the grammar, which is exactly what makes hand-written
 * recursive descent easy to read: to see how `while` loops parse,
 * just go read the T_WHILE branch of parse_stmt() below.
 *
 * ===================================================================
 * EXPRESSIONS: PRECEDENCE CLIMBING
 * ===================================================================
 * C has about 15 levels of operator precedence (`*` binds tighter
 * than `+`, which binds tighter than `<<`, and so on). The standard
 * recursive-descent trick for this is one function per precedence
 * level, each calling the *next tighter* level for its operands:
 *
 *   parse_logor -> parse_logand -> parse_bitor -> parse_bitxor ->
 *   parse_bitand -> parse_equality -> parse_relational ->
 *   parse_shift -> parse_additive -> parse_term -> parse_unary ->
 *   parse_postfix -> parse_primary
 *
 * Each level looks like: "parse one thing at the next tighter level,
 * then while the next token is an operator at *this* level, consume
 * it and parse another thing at the next tighter level, combining
 * them into a bigger node." That loop is what makes `a + b + c` parse
 * as left-associative ((a+b)+c) rather than right-associative. Look
 * at parse_additive() below for the simplest example of the pattern;
 * every other precedence level is the same shape with different
 * tokens.
 *
 * parse_primary(), at the bottom, is where the recursion actually
 * stops: numbers, identifiers, calls, parenthesized sub-expressions.
 *
 * ===================================================================
 * THE TWO PASSES
 * ===================================================================
 * pass_a() walks every token ONCE, recognizing just enough structure
 * to record each function's signature (name, return type, parameter
 * types) and each global's type into the symbol tables from symtab.c
 * - but it never parses a function body, it just skips over the
 * balanced { } with skip_balanced() and moves on. This is deliberately
 * shallow and fast.
 *
 * pass_b() then rewinds to the start of the token array and does the
 * real work: for each function, it re-parses the signature (already
 * validated by pass_a, so this time it's just being skipped past) and
 * then genuinely parses the body with parse_block(), immediately
 * calling emit_function() (codegen_stmt.c) to generate code for it
 * before moving to the next function. Because pass_a already recorded
 * every function and global that exists anywhere in the file, code in
 * one function can reference another function or global no matter
 * which one appears first in the source text.
 */

#include "cc64.h"

/* ===================================================================
 * Shared token cursor
 * ===================================================================
 * g_pos is the parser's read position into g_toks[]. It's deliberately
 * NOT in cc64.h / exposed to other files: every other file that needs
 * to inspect tokens could read g_toks[] directly with its own
 * index, and nothing outside this file ever needs to know "where the
 * parser currently is". Keeping it file-static is a small example of
 * information hiding - the rest of the compiler simply has no way to
 * accidentally interfere with the parser's cursor.
 * =================================================================== */

static int g_pos = 0;

static Token *cur(void) { return &g_toks[g_pos]; }
static Token *peekAt(int off) { return &g_toks[g_pos + off]; }
static Token *advance(void) { return &g_toks[g_pos++]; }
static int check(TokKind k) { return cur()->kind == k; }
/* Consumes the current token if it matches `k`, or fails with a clear
 * error naming what was expected (`what`, e.g. "';'") - this is the
 * workhorse used everywhere the grammar requires a specific token,
 * like the closing ')' of an if-condition. */
static Token *expect(TokKind k, const char *what) {
    if (!check(k)) fatal(cur()->line, "expected %s", what);
    return advance();
}

static int is_type_kw(TokKind k) { return k == T_INT || k == T_CHAR || k == T_VOID; }

/* Several parsing functions call each other in a cycle that doesn't
 * follow a strict "top to bottom" order (parse_primary() needs
 * parse_assign() for call arguments and parse_expr() for parenthesized
 * sub-expressions, both of which are defined much further down, in
 * terms of parse_primary() itself via the precedence chain) - these
 * forward declarations are what make that legal C. None of these four
 * need to be visible outside this file, so they stay `static` rather
 * than moving to cc64.h. */
static Node *parse_expr(void);
static Node *parse_assign(void);
static Node *parse_stmt(void);
static Node *parse_block(void);

/* ===================================================================
 * Pass A: scan top-level declarations (signatures only, no bodies)
 * =================================================================== */

/* Consumes a `{ ... }` block without looking at what's inside it,
 * correctly handling nested braces by tracking depth. This is how
 * pass_a() skips a function body cheaply: it doesn't need to
 * understand the body yet, just find where it ends. */
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

/* One iteration of this loop handles exactly one top-level
 * declaration: either `type [*] name ( params ) { body }` (a
 * function - body skipped, not parsed) or `type [*] name [size];`
 * (a global variable, optionally an array, optionally with a simple
 * constant initializer). Everything this function records goes
 * straight into g_funcs[]/g_globals[] via find_func()/g_globals[]. */
void pass_a(void) {
    g_pos = 0;
    while (!check(T_EOF)) {
        if (!is_type_kw(cur()->kind))
            fatal(cur()->line, "expected type at top level");
        TokKind tk = advance()->kind;
        int rIsPointer = 0;
        if (check(T_STAR)) { rIsPointer = 1; advance(); }
        int rtype = type_from_tok(tk);
        if (rIsPointer && rtype < 0) fatal(cur()->line, "'void *' is not supported in this version");
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
                fn->retIsPointer = rIsPointer;
            }
            fn->nparams = 0;
            if (check(T_VOID) && peekAt(1)->kind == T_RPAREN) {
                advance();
            } else if (!check(T_RPAREN)) {
                for (;;) {
                    if (!is_type_kw(cur()->kind))
                        fatal(cur()->line, "expected parameter type");
                    TokKind ptk = advance()->kind;
                    int pIsPointer = 0;
                    if (check(T_STAR)) { pIsPointer = 1; advance(); }
                    if (ptk == T_VOID && !pIsPointer) fatal(cur()->line, "void is not a valid parameter type");
                    if (!check(T_IDENT)) fatal(cur()->line, "expected parameter name");
                    char *pname = advance()->text;
                    if (check(T_LBRACKET))
                        fatal(cur()->line, "array parameters are not supported; use a pointer ('%s *%s') instead", tk==T_CHAR?"char":"int", pname);
                    if (fn->nparams >= 32) fatal(cur()->line, "too many parameters");
                    fn->paramTypes[fn->nparams] = type_from_tok(ptk);
                    fn->paramIsPointer[fn->nparams] = pIsPointer;
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
            skip_balanced(T_LBRACE, T_RBRACE);
            continue;
        }

        /* global variable */
        if (rtype < 0) fatal(cur()->line, "'void' is not a valid variable type");
        GSym g; memset(&g, 0, sizeof(g));
        strncpy(g.name, name, sizeof(g.name)-1);
        g.type = rtype;
        g.isPointer = rIsPointer;
        if (check(T_LBRACKET)) {
            if (rIsPointer) fatal(cur()->line, "arrays of pointers are not supported in this version");
            advance();
            if (!check(T_NUM)) fatal(cur()->line, "expected array size");
            g.isArray = 1;
            g.arrLen = (int)advance()->ival;
            expect(T_RBRACKET, "']'");
        }
        if (check(T_ASSIGN)) {
            advance();
            if (g.isPointer) fatal(cur()->line, "pointer initializers are not supported in this version");
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
 * Parser: expressions (precedence climbing - see file header)
 * =================================================================== */

/* The bottom of the precedence chain: literals, identifiers, function
 * calls, and parenthesized sub-expressions (which "reset" back to the
 * loosest precedence, parse_expr(), since anything can appear inside
 * parentheses regardless of its own precedence). */
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
            /* function call: name ( arg, arg, ... ) */
            advance();
            Node *call = node_new(N_CALL, t->line);
            call->name = t->text;
            Node *head = NULL, *tail = NULL;
            if (!check(T_RPAREN)) {
                for (;;) {
                    Node *arg = parse_assign(); /* each arg is itself a full expression */
                    if (!head) head = tail = arg; else { tail->next = arg; tail = arg; }
                    if (check(T_COMMA)) { advance(); continue; }
                    break;
                }
            }
            expect(T_RPAREN, "')'");
            call->a = head; /* arguments, chained through ->next */
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
    return NULL; /* unreachable */
}

/* Postfix operators: array/pointer indexing `a[i]` and postfix
 * `x++`/`x--`. These bind tighter than anything else and can chain
 * (`a[i]++` first indexes, then increments the result), which is why
 * this is a loop around parse_primary() rather than a single check. */
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

/* Prefix/unary operators. Right-associative by construction: each of
 * these calls parse_unary() again for its operand (not parse_postfix()
 * directly), so `!!x` correctly parses as `!(!x)`. */
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
    if (check(T_AMP)) { Token *t = advance(); Node *n = node_new(N_ADDR, t->line); n->a = parse_unary(); return n; }
    if (check(T_STAR)) { Token *t = advance(); Node *n = node_new(N_DEREF, t->line); n->a = parse_unary(); return n; }
    return parse_postfix();
}

static Node *mkbin(const char *op, Node *a, Node *b, int line) {
    Node *n = node_new(N_BINOP, line);
    snprintf(n->op, sizeof(n->op), "%s", op);
    n->a = a; n->b = b;
    return n;
}

/* From here down is the precedence-climbing chain described in the
 * file header comment: each function is "parse one operand at the
 * next tighter level, then greedily consume same-precedence operators
 * in a loop, left-associatively combining as we go." The `for (;;)`
 * loop shape (rather than recursion) is what makes these
 * left-associative instead of right-associative. */
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
/* && and || get their own node kinds (N_LOGAND/N_LOGOR) rather than
 * being plain N_BINOP like the operators above - codegen needs to
 * treat them specially to implement short-circuit evaluation (the
 * right-hand side must not even be evaluated when the left side
 * already determines the answer), which an ordinary binary operator
 * doesn't need to worry about. */
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

/* Which AST node kinds are valid assignment targets - i.e. actually
 * refer to a storage location (a variable, an array element, or a
 * dereferenced pointer) rather than being a computed, transient
 * value. `x = 5` is fine; `(x + 1) = 5` is not, and is_lvalue_node()
 * is what parse_assign() below uses to reject it with a clear error
 * instead of generating nonsensical code for it. */
static int is_lvalue_node(Node *n) { return n->kind == N_IDENT || n->kind == N_INDEX || n->kind == N_DEREF; }

/* Assignment is the loosest-binding operator and, unlike everything
 * above, right-associative (`a = b = c` means `a = (b = c)`), which is
 * why this calls parse_assign() recursively for the right-hand side
 * instead of looping. It's also the one place a plain N_BINOP-style
 * table lookup isn't quite enough, since each compound-assignment
 * operator (`+=`, `-=`, ...) needs to remember which underlying
 * operator it stands for. */
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

/* A local variable declaration: `type [*] name [ [size] ] [= expr];`.
 * Unlike a global (see pass_a()), a local's initializer can be any
 * expression, not just a constant literal - it's evaluated at
 * runtime, when the enclosing statement actually executes (see
 * N_VARDECL's case in gen_stmt(), codegen_stmt.c). */
static Node *parse_vardecl(void) {
    Token *tt = cur();
    TokKind tk = advance()->kind; /* int|char, already known to be a type */
    int isPointer = 0;
    if (check(T_STAR)) { isPointer = 1; advance(); }
    if (!check(T_IDENT)) fatal(cur()->line, "expected identifier");
    char *name = advance()->text;
    Node *n = node_new(N_VARDECL, tt->line);
    n->name = name;
    n->declType = type_from_tok(tk);
    n->declIsPointer = isPointer;
    if (check(T_LBRACKET)) {
        if (isPointer) fatal(cur()->line, "arrays of pointers are not supported in this version");
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
    /* Registering the local here, as soon as it's parsed (rather than
     * waiting until the whole function is parsed), is what lets later
     * statements in the same function refer to it - parse_primary()'s
     * handling of a bare identifier just looks it up via find_local(),
     * which only works if register_local() has already run for it. */
    register_local(name, n->declType, n->declIsPointer, n->declArrLen != 0, n->declArrLen, 0, n->line);
    return n;
}

/* One statement. Each branch here corresponds to one statement form
 * in the grammar; the shape closely follows how you'd describe C's
 * statement grammar in prose ("an if is 'if' '(' expr ')' stmt
 * optionally followed by 'else' stmt", etc). */
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
            if (is_type_kw(cur()->kind)) init = parse_vardecl(); /* consumes its own ';' */
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
    /* Anything else is an expression statement: an expression followed
     * by ';', kept only for its side effects (e.g. a bare function
     * call, or `x = 5;`). */
    Node *n = node_new(N_EXPRSTMT, cur()->line);
    n->a = parse_expr();
    expect(T_SEMI, "';'");
    return n;
}

/* `{ stmt* }` - a block is just a list of statements, chained through
 * each Node's `next` pointer and hung off the block node's `a`. */
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
 * Pass B: real parse of each function body, immediately followed by
 * codegen for that function (see emit_function() in codegen_stmt.c).
 * By the time this runs, pass_a() has already populated g_funcs[] and
 * g_globals[] with every symbol in the file, so nothing parsed here
 * can hit an "undeclared function" surprise just because that
 * function happens to be defined later in the source text.
 * =================================================================== */

void pass_b(void) {
    g_pos = 0;
    while (!check(T_EOF)) {
        advance(); /* type keyword - already recorded by pass_a, just skip it */
        if (check(T_STAR)) advance(); /* optional pointer '*'; already validated in pass A */
        char *name = advance()->text; /* ident */

        if (check(T_LPAREN)) {
            advance();
            FnSym *fn = find_func(name);
            /* Skip the parameter list tokens - pass_a already recorded
             * their types/names into fn->paramTypes[]/paramNames[]. */
            int depth = 1;
            while (depth > 0) {
                if (check(T_LPAREN)) depth++;
                else if (check(T_RPAREN)) depth--;
                advance();
            }
            if (check(T_SEMI)) { advance(); continue; } /* prototype only, no body to compile */
            /* Function body: reset the per-function locals table, seed
             * it with the parameters (so the body can refer to them
             * immediately), then really parse the body and hand it to
             * codegen. g_curfn is set for the duration so codegen and
             * infer_type() know which function's locals are in scope. */
            g_nlocals = 0;
            g_curfn = fn;
            for (int i = 0; i < fn->nparams; i++)
                register_local(fn->paramNames[i], fn->paramTypes[i], fn->paramIsPointer[i], 0, 0, 1, 0);
            Node *body = parse_block();
            emit_function(fn, body);
            g_curfn = NULL;
            continue;
        }

        /* Global variable: already recorded by pass_a(), so just skip
         * its tokens here rather than re-parsing and re-validating it. */
        if (check(T_LBRACKET)) { advance(); advance(); expect(T_RBRACKET, "']'"); }
        if (check(T_ASSIGN)) { advance(); if (check(T_MINUS)) advance(); advance(); }
        expect(T_SEMI, "';'");
    }
}
