/*
 * codegen_stmt.c - turns a statement AST node into 6502 instructions
 * (gen_stmt()), and emits the storage layout for global variables and
 * whole functions (emit_global_storage(), emit_function()).
 *
 * This file is the natural companion to codegen_expr.c: expressions
 * produce values, statements produce control flow and side effects.
 * gen_stmt() handles blocks, if/while/for, break/continue/return, and
 * local declarations - for anything that's fundamentally "evaluate an
 * expression for its value", it just calls gen_expr_to_R() from
 * codegen_expr.c and moves on.
 */

#include "cc64.h"

/* Every active loop's break/continue targets, as a simple stack -
 * entering a loop pushes its labels, leaving it pops them, and
 * break/continue just jump to whatever's on top. Purely internal to
 * this file: nothing outside gen_stmt() ever needs to know a loop is
 * currently being compiled. */
typedef struct { char *breakLbl; char *contLbl; } LoopCtx;
static LoopCtx g_loops[64];
static int g_nloops = 0;

/* ===================================================================
 * gen_stmt(): the statement-level counterpart to gen_expr_to_R() in
 * codegen_expr.c. Where an expression always produces a VALUE (in
 * __zpR), a statement produces CONTROL FLOW - it's compiled into a
 * sequence of instructions, conditional branches, and labels, with no
 * result to leave anywhere. Every control-flow statement (if/while/
 * for) follows the same shape: evaluate a condition expression with
 * gen_expr_to_R(), test whether __zpR is zero (false) or nonzero
 * (true) with the classic 6502 idiom `LDA lo : ORA hi : branch`
 * (ORing the two bytes together folds a 16-bit zero-test into one
 * flags check), and branch accordingly using emit_far_branch() from
 * codegen.c (never a raw branch instruction - see its comment for why).
 * =================================================================== */

static void gen_stmt(Node *n) {
    switch (n->kind) {
        case N_BLOCK:
            for (Node *s = n->a; s; s = s->next) gen_stmt(s);
            return;
        case N_VARDECL:
            if (n->a) { gen_expr_to_R(n->a);
                char lbl[96]; local_label(lbl, sizeof(lbl), g_curfn->name, n->name);
                gen_store_scalar(lbl, n->declType, n->declIsPointer);
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
            if (n->a) {
                CType rt = infer_type(n->a);
                int isNullLiteral = (n->a->kind == N_NUM && n->a->ival == 0);
                if (g_curfn->retIsPointer && !rt.isPointer && !isNullLiteral)
                    fatal(n->line, "function '%s' should return a pointer", g_curfn->name);
                if (!g_curfn->retIsPointer && rt.isPointer)
                    fatal(n->line, "function '%s' should not return a pointer", g_curfn->name);
                gen_expr_to_R(n->a);
            }
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

/* ===================================================================
 * Emitting storage layout
 * ===================================================================
 * The two functions below don't generate any "logic" - they emit the
 * `.fill`/`.word`/`.byte` assembler directives that reserve and
 * (optionally) initialize memory for every global variable and every
 * function's locals/parameters. This is where the symbol tables
 * built up during parsing (g_globals[], and per-function g_locals[])
 * finally turn into actual memory addresses in the output.
 * =================================================================== */

void emit_global_storage(void) {
    emit("; ---- global variables --------------------------------------------");
    for (int i = 0; i < g_nglobals; i++) {
        GSym *g = &g_globals[i];
        char lbl[96]; global_label(lbl, sizeof(lbl), g->name);
        if (g->isArray) {
            int bytes = g->arrLen * (g->type == TY_INT ? 2 : 1); /* arrays of pointers not supported */
            emit("%s:", lbl);
            emit("    .fill %d, 0", bytes);
        } else if (g->hasInit) {
            emit("%s:", lbl);
            if (g->type == TY_INT) emit("    .word %ld", g->initVal);
            else emit("    .byte %ld", g->initVal & 0xFF ? (g->initVal & 0xFF) : 0);
        } else {
            emit("%s:", lbl);
            emit("    .fill %d, 0", var_width(g->type, g->isPointer));
        }
    }
    emit(" ");
}

/* One function's storage (parameters, then locals - see cc64.h's
 * architecture note on why these get FIXED addresses instead of a
 * stack frame) followed by its actual code. Called once per function
 * by pass_b() in parser.c, right after that function's body has been
 * parsed. The trailing RTS handles a function that "falls off the
 * end" without an explicit return - falling off the end of a non-void
 * function is undefined behavior in real C, but this compiler would
 * rather emit a harmless extra RTS than generate a function that
 * doesn't return at all. */
void emit_function(FnSym *fn, Node *body) {
    char flbl[96]; func_label(flbl, sizeof(flbl), fn->name);
    emit("; ---- function %s ---------------------------------------------", fn->name);
    for (int i = 0; i < fn->nparams; i++) {
        char lbl[96]; local_label(lbl, sizeof(lbl), fn->name, fn->paramNames[i]);
        emit("%s:", lbl);
        emit("    .fill %d, 0", var_width(fn->paramTypes[i], fn->paramIsPointer[i]));
    }
    for (int i = 0; i < g_nlocals; i++) {
        if (g_locals[i].isParam) continue;
        char lbl[96]; local_label(lbl, sizeof(lbl), fn->name, g_locals[i].name);
        int bytes = g_locals[i].isArray
            ? g_locals[i].arrLen * (g_locals[i].type == TY_INT ? 2 : 1) /* arrays of pointers not supported */
            : var_width(g_locals[i].type, g_locals[i].isPointer);
        emit("%s:", lbl);
        emit("    .fill %d, 0", bytes);
    }
    emit("%s:", flbl);
    gen_stmt(body);
    emit("    RTS"); /* fallback for functions that fall off the end */
    emit(" ");
}

