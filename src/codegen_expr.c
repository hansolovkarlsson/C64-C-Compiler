/*
 * codegen_expr.c - turns an expression AST node into 6502 instructions
 * that leave its value in the primary register (__zpR). This is the
 * single largest file in the compiler, because C's expressions are
 * where most of the actual "compiling" work is: assignment, pointer
 * arithmetic, array/pointer indexing, and every operator all meet
 * here.
 *
 * If you're approaching this file for the first time, start at the
 * bottom with gen_expr_to_R() - it's the dispatcher that every other
 * function here ultimately serves, and its comment explains the
 * contract every case has to satisfy. Then look at resolve_lvalue_base(),
 * which is arguably the most important single function in this file:
 * assignment, compound assignment, ++/--, and address-of all funnel
 * through it to answer "where does this expression's storage live?"
 * for the three kinds of thing that can be assigned to (a plain
 * variable, an array element, or a dereferenced pointer).
 *
 * One ordering note: resolve_lvalue_base() (defined early in this
 * file) calls gen_expr_to_R() (defined at the very end of it) to
 * evaluate a dereferenced pointer's sub-expression. That's fine
 * despite the definition order - gen_expr_to_R() is declared in
 * cc64.h (it's the one function from this file other modules need to
 * call too, since codegen_stmt.c calls it for every expression
 * statement), so its prototype is already visible from the
 * `#include "cc64.h"` below, before resolve_lvalue_base() is even
 * defined. Nothing else in this file needs a similar forward
 * declaration, since every other function here is only ever called
 * from something defined later in the same file.
 */

#include "cc64.h"

/* Where a resolved lvalue's storage lives, filled in by
 * resolve_lvalue_base() and consumed by gen_lv_load_to_R()/
 * gen_lv_store_from_R() just below it. This struct is purely internal
 * plumbing for this file - nothing outside codegen_expr.c ever needs
 * to know an LVInfo exists, which is why it lives here rather than in
 * cc64.h alongside the types every file needs. */
typedef struct {
    int isArray;       /* true: access indirectly via __zpAP (array elem, *p, p[i]) */
    int type;           /* elem/pointee type for indirect access; var type for scalars */
    int isPointer;       /* only meaningful when !isArray: the scalar itself holds an address */
    char label[96];     /* scalar: storage label. (unused when isArray) */
} LVInfo;

/* ===================================================================
 * Reading and writing plain (non-array, non-pointer-indirect) scalar
 * variables: a fixed memory location, one or two bytes depending on
 * width. gen_load_scalar() zero-extends a 1-byte char into the full
 * 16-bit __zpR (so arithmetic on a char always sees a clean 16-bit
 * value); gen_store_scalar() does the reverse, simply not writing the
 * high byte for a 1-byte destination (truncation is implicit - the
 * low byte written is already the correct result either way).
 * =================================================================== */

static void gen_load_scalar(const char *label, int type, int isPointer) {
    int wide = isPointer || type == TY_INT;
    if (!wide) {
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
void gen_store_scalar(const char *label, int type, int isPointer) {
    int wide = isPointer || type == TY_INT;
    emit("    LDA __zpR");
    emit("    STA %s", label);
    if (wide) {
        emit("    LDA __zpR+1");
        emit("    STA %s+1", label);
    }
}

/* Resolve a variable, array-index, or dereference node's storage. For
 * array elements and dereferences, emits code (clobbering __zpR, and
 * __zpR2 for the general pointer-index path) that leaves the effective
 * address in __zpAP; lv->isArray tells the caller to access through
 * __zpAP rather than a fixed label. */
/* ===================================================================
 * Resolving an lvalue: where does this expression's storage live?
 * ===================================================================
 * Three different kinds of expression can appear on the left of an
 * assignment (or as the operand of ++/--, or of & for address-of):
 * a plain variable (N_IDENT), an array element or pointer index
 * (N_INDEX), or a dereferenced pointer (N_DEREF). The first has a
 * FIXED, compile-time-known address (a label); the other two need
 * RUNTIME computation to find their address, which resolve_lvalue_base()
 * does by computing the effective address into __zpAP (the one
 * register allowed to be used with (zp),Y indirect addressing - see
 * emit_zp_equates() in codegen_runtime.c) and setting lv->isArray to
 * tell the caller "read/write through __zpAP", vs. reading/writing
 * lv->label directly for the plain-variable case.
 *
 * This one function is the single place that understands all three
 * cases, which is what lets every caller (assignment, compound
 * assignment, ++/--, address-of) stay simple: resolve the lvalue
 * once, then use gen_lv_load_to_R()/gen_lv_store_from_R() below
 * without needing to know which of the three cases it actually was.
 * =================================================================== */

static void resolve_lvalue_base(Node *n, LVInfo *lv) {
    memset(lv, 0, sizeof(*lv));
    if (n->kind == N_IDENT) {
        LSym *l = g_curfn ? find_local(n->name) : NULL;
        if (l) {
            if (l->isArray) fatal(n->line, "'%s' is an array; use an index", n->name);
            local_label(lv->label, sizeof(lv->label), g_curfn->name, n->name);
            lv->type = l->type;
            lv->isPointer = l->isPointer;
            return;
        }
        GSym *g = find_global(n->name);
        if (!g) fatal(n->line, "undeclared identifier '%s'", n->name);
        if (g->isArray) fatal(n->line, "'%s' is an array; use an index", n->name);
        global_label(lv->label, sizeof(lv->label), n->name);
        lv->type = g->type;
        lv->isPointer = g->isPointer;
        return;
    }
    if (n->kind == N_DEREF) {
        CType pt = infer_type(n->a);
        if (!pt.isPointer) fatal(n->line, "cannot dereference a non-pointer value");
        gen_expr_to_R(n->a); /* pointer value -> __zpR */
        emit("    LDA __zpR");
        emit("    STA __zpAP");
        emit("    LDA __zpR+1");
        emit("    STA __zpAP+1");
        lv->isArray = 1;
        lv->type = pt.base;
        return;
    }
    if (n->kind == N_INDEX) {
        /* Fast path: n->a names a TRUE array (not a pointer) - the base
         * address is a compile-time constant, so no extra add is needed. */
        if (n->a->kind == N_IDENT) {
            LSym *l = g_curfn ? find_local(n->a->name) : NULL;
            GSym *g = l ? NULL : find_global(n->a->name);
            if ((l && l->isArray) || (g && g->isArray)) {
                char base[96]; int elemType;
                if (l) { local_label(base, sizeof(base), g_curfn->name, n->a->name); elemType = l->type; }
                else   { global_label(base, sizeof(base), n->a->name); elemType = g->type; }
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
        }
        /* General path: n->a is any pointer-valued expression (a pointer
         * variable, a dereference, a function call returning a pointer,
         * pointer arithmetic, ...) - its value is only known at runtime. */
        CType pt = infer_type(n->a);
        if (!pt.isPointer) fatal(n->line, "cannot index a non-pointer, non-array value");
        int elemType = pt.base;
        gen_expr_to_R(n->a);       /* base pointer value -> __zpR */
        emit("    JSR __rt_push"); /* save it on the operand stack */
        gen_expr_to_R(n->b);       /* index -> __zpR */
        if (elemType == TY_INT) {
            emit("    ASL __zpR");
            emit("    ROL __zpR+1");
        }
        emit("    JSR __rt_pop2"); /* __zpR2 = base pointer, __zpR = scaled index */
        emit("    CLC");
        emit("    LDA __zpR2");
        emit("    ADC __zpR");
        emit("    STA __zpAP");
        emit("    LDA __zpR2+1");
        emit("    ADC __zpR+1");
        emit("    STA __zpAP+1");
        lv->isArray = 1;
        lv->type = elemType;
        return;
    }
    fatal(n->line, "expression is not assignable");
}

/* Once resolve_lvalue_base() has filled in an LVInfo, these two
 * functions are how every caller actually reads or writes the value -
 * dispatching on lv->isArray to pick "direct label access" vs.
 * "indirect through __zpAP" without the caller needing to care which. */
static void gen_lv_load_to_R(LVInfo *lv) {
    if (!lv->isArray) { gen_load_scalar(lv->label, lv->type, lv->isPointer); return; }
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
    if (!lv->isArray) { gen_store_scalar(lv->label, lv->type, lv->isPointer); return; }
    emit("    LDY #0");
    emit("    LDA __zpR");
    emit("    STA (__zpAP),Y");
    if (lv->type == TY_INT) {
        emit("    INY");
        emit("    LDA __zpR+1");
        emit("    STA (__zpAP),Y");
    }
}

/* ===================================================================
 * gen_binop(): the actual instruction sequences for each binary
 * operator, given that both operands are ALREADY sitting in
 * __zpR2 (left) and __zpR (right) - see the calling convention note
 * at the top of codegen_runtime.c. Simple arithmetic/bitwise ops are
 * inlined directly (a handful of 6502 instructions each); anything
 * that needs a real algorithm (multiply, divide, comparisons) just
 * calls into the runtime library from codegen_runtime.c. unsignedCmp
 * picks the unsigned comparison routines instead of the signed ones
 * when the caller (gen_expr_to_R's N_BINOP case, below) has determined
 * via infer_type() that a pointer is involved.
 * =================================================================== */

static void gen_binop(const char *op, int unsignedCmp) {
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
    else if (strcmp(op, "<") == 0) { emit("    JSR %s", unsignedCmp ? "__rt_ult16" : "__rt_lt16"); }
    else if (strcmp(op, ">") == 0) { emit("    JSR %s", unsignedCmp ? "__rt_ugt16" : "__rt_gt16"); }
    else if (strcmp(op, "<=") == 0) { emit("    JSR %s", unsignedCmp ? "__rt_ule16" : "__rt_le16"); }
    else if (strcmp(op, ">=") == 0) { emit("    JSR %s", unsignedCmp ? "__rt_uge16" : "__rt_ge16"); }
    else fatal(0, "internal: unknown operator '%s'", op);
}

/* ===================================================================
 * Function calls. putchar/puts/peek/poke are handled entirely here as
 * special cases ("builtins") - they're not real, callable functions
 * with an entry in g_funcs[], just patterns this compiler recognizes
 * by name and expands directly into the right runtime-library calls.
 * Any other name falls through to the general case: evaluate each
 * argument, store it into the callee's fixed parameter slot (see
 * cc64.h's architecture note on why parameters have fixed addresses
 * rather than living on a stack), then JSR. A little light type-
 * checking happens here too (rejecting an int argument where a
 * pointer parameter was declared, or vice versa) - see infer_type()'s
 * own comment in symtab.c for the philosophy behind checks like this.
 * =================================================================== */

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
        if (cnt != 1) fatal(n->line, "puts() takes exactly 1 argument");
        CType t = infer_type(n->a);
        if (!(t.isPointer && t.base == TY_CHAR))
            fatal(n->line, "puts() requires a char* argument (a string literal or char* value)");
        gen_expr_to_R(n->a);  /* -> __zpR: pointer to a null-terminated string */
        emit("    JSR __rt_puts");
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
        CType at = infer_type(a);
        int isNullLiteral = (a->kind == N_NUM && a->ival == 0);
        if (fn->paramIsPointer[i] && !at.isPointer && !isNullLiteral)
            fatal(a->line, "argument %d to '%s' should be a pointer", i + 1, n->name);
        if (!fn->paramIsPointer[i] && at.isPointer)
            fatal(a->line, "argument %d to '%s' should not be a pointer", i + 1, n->name);
        gen_expr_to_R(a);
        char lbl[96];
        local_label(lbl, sizeof(lbl), fn->name, fn->paramNames[i]);
        gen_store_scalar(lbl, fn->paramTypes[i], fn->paramIsPointer[i]);
    }
    char flbl[96];
    func_label(flbl, sizeof(flbl), fn->name);
    emit("    JSR %s", flbl);
}

/* ===================================================================
 * ++ and -- (both prefix and postfix). The step size is scaled to
 * the pointee size for a pointer lvalue (the same rule as pointer
 * arithmetic in gen_expr_to_R's N_BINOP case below - incrementing an
 * int* moves 2 bytes, not 1), and for postfix, the ORIGINAL value is
 * saved in __zpR2 before the increment so it can be restored into
 * __zpR afterward - postfix `x++` evaluates to x's value BEFORE the
 * increment, even though the increment has already happened by the
 * time the expression is done.
 * =================================================================== */

static void gen_incdec(Node *n, int isInc, int isPost) {
    LVInfo lv;
    resolve_lvalue_base(n->a, &lv);
    int step = (lv.isPointer && lv.type == TY_INT) ? 2 : 1;
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
        emit("    ADC #%d", step);
        emit("    STA __zpR");
        emit("    LDA __zpR+1");
        emit("    ADC #0");
        emit("    STA __zpR+1");
    } else {
        emit("    SEC");
        emit("    LDA __zpR");
        emit("    SBC #%d", step);
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

/* ===================================================================
 * gen_expr_to_R(): the heart of expression codegen. Given any
 * expression AST node, generates 6502 instructions that leave its
 * value in __zpR when they're done - that's the one contract every
 * case below has to satisfy, which is what lets expressions nest
 * arbitrarily (an operand of `+` can itself be a whole `*` expression,
 * which can itself contain a function call, ...) without the codegen
 * for any one node needing to know what kind of expression contains
 * it. This is a direct match to the "everything evaluates to a value
 * in a register" model described in cc64.h's architecture overview.
 * =================================================================== */

void gen_expr_to_R(Node *n) {
    switch (n->kind) {
        case N_NUM:
            emit("    LDA #<%ld", n->ival);
            emit("    STA __zpR");
            emit("    LDA #>%ld", n->ival);
            emit("    STA __zpR+1");
            return;
        case N_STR: {
            char *lbl = intern_string(n->sval);
            emit("    LDA #<%s", lbl);
            emit("    STA __zpR");
            emit("    LDA #>%s", lbl);
            emit("    STA __zpR+1");
            return;
        }
        case N_IDENT: {
            /* a true array used as a value decays to a pointer to its
             * first element (its base address, same bytes either way) */
            LSym *l = g_curfn ? find_local(n->name) : NULL;
            GSym *g = l ? NULL : find_global(n->name);
            if (!l && !g) fatal(n->line, "undeclared identifier '%s'", n->name);
            if ((l && l->isArray) || (g && g->isArray)) {
                char lbl[96];
                if (l) local_label(lbl, sizeof(lbl), g_curfn->name, n->name);
                else global_label(lbl, sizeof(lbl), n->name);
                emit("    LDA #<%s", lbl);
                emit("    STA __zpR");
                emit("    LDA #>%s", lbl);
                emit("    STA __zpR+1");
                return;
            }
            LVInfo lv; resolve_lvalue_base(n, &lv); gen_lv_load_to_R(&lv);
            return;
        }
        case N_INDEX: {
            LVInfo lv; resolve_lvalue_base(n, &lv); gen_lv_load_to_R(&lv);
            return;
        }
        case N_ADDR: {
            Node *operand = n->a;
            if (operand->kind == N_IDENT) {
                LSym *l = g_curfn ? find_local(operand->name) : NULL;
                char lbl[96];
                if (l) local_label(lbl, sizeof(lbl), g_curfn->name, operand->name);
                else {
                    GSym *g = find_global(operand->name);
                    if (!g) fatal(operand->line, "undeclared identifier '%s'", operand->name);
                    global_label(lbl, sizeof(lbl), operand->name);
                }
                emit("    LDA #<%s", lbl);
                emit("    STA __zpR");
                emit("    LDA #>%s", lbl);
                emit("    STA __zpR+1");
                return;
            }
            if (operand->kind == N_INDEX || operand->kind == N_DEREF) {
                LVInfo lv; resolve_lvalue_base(operand, &lv); /* always sets __zpAP for these kinds */
                emit("    LDA __zpAP");
                emit("    STA __zpR");
                emit("    LDA __zpAP+1");
                emit("    STA __zpR+1");
                return;
            }
            fatal(n->line, "cannot take the address of this expression");
            return;
        }
        case N_DEREF: {
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
            int scalePtr = lv.isPointer && (strcmp(n->op, "+") == 0 || strcmp(n->op, "-") == 0);
            int esize = (lv.type == TY_INT) ? 2 : 1;
            gen_lv_load_to_R(&lv);     /* current value -> R (uses __zpAP, still valid here) */
            emit("    JSR __rt_push"); /* save current value */
            gen_expr_to_R(n->b);       /* rhs -> R; may clobber __zpAP */
            if (scalePtr && esize == 2) {
                emit("    ASL __zpR");
                emit("    ROL __zpR+1");
            }
            emit("    JSR __rt_pop2"); /* __zpR2 = old value, __zpR = rhs value */
            gen_binop(n->op, 0);
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
            const char *op = n->op;
            if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0) {
                CType ta = infer_type(n->a), tb = infer_type(n->b);
                if (ta.isPointer && tb.isPointer) {
                    if (strcmp(op, "+") == 0)
                        fatal(n->line, "invalid operands to binary '+' (pointer + pointer)");
                    int esize = (ta.base == TY_INT) ? 2 : 1;
                    gen_expr_to_R(n->a);
                    emit("    JSR __rt_push");
                    gen_expr_to_R(n->b);
                    emit("    JSR __rt_pop2");
                    gen_binop("-", 0); /* raw byte difference -> __zpR */
                    if (esize == 2) {
                        emit("    LDA __zpR+1");
                        emit("    CMP #$80");
                        emit("    ROR __zpR+1");
                        emit("    ROR __zpR");
                    }
                    return;
                }
                if (!ta.isPointer && tb.isPointer) {
                    if (strcmp(op, "-") == 0)
                        fatal(n->line, "invalid operands to binary '-' (int - pointer)");
                    int esize = (tb.base == TY_INT) ? 2 : 1;
                    gen_expr_to_R(n->a);
                    if (esize == 2) { emit("    ASL __zpR"); emit("    ROL __zpR+1"); }
                    emit("    JSR __rt_push");
                    gen_expr_to_R(n->b);
                    emit("    JSR __rt_pop2");
                    gen_binop(op, 0);
                    return;
                }
                if (ta.isPointer && !tb.isPointer) {
                    int esize = (ta.base == TY_INT) ? 2 : 1;
                    gen_expr_to_R(n->a);
                    emit("    JSR __rt_push");
                    gen_expr_to_R(n->b);
                    if (esize == 2) { emit("    ASL __zpR"); emit("    ROL __zpR+1"); }
                    emit("    JSR __rt_pop2");
                    gen_binop(op, 0);
                    return;
                }
                /* neither operand is a pointer: fall through below */
            }
            int unsignedCmp = 0;
            if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
                CType ta = infer_type(n->a), tb = infer_type(n->b);
                unsignedCmp = ta.isPointer || tb.isPointer;
            }
            gen_expr_to_R(n->a);
            emit("    JSR __rt_push");
            gen_expr_to_R(n->b);
            emit("    JSR __rt_pop2");
            gen_binop(op, unsignedCmp);
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
