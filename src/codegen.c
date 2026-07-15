/*
 * codegen.c - small utilities shared by every code-generating file
 * (codegen_runtime.c, codegen_expr.c, codegen_stmt.c): writing a line
 * of assembly, minting unique labels, and formatting the label names
 * this compiler uses for variables and functions.
 *
 * Keeping these in their own file (rather than duplicating them, or
 * putting them in whichever codegen file happened to need them
 * first) means codegen_expr.c and codegen_stmt.c can both call emit()
 * without either one "owning" it - a small but real example of why
 * you'd factor out shared infrastructure into its own module.
 */

#include "cc64.h"

/* The currently-open output file. Opened and closed by main.c, but
 * written to from every codegen file via emit() below - and, in one
 * special case (the string-literal data section), written to
 * directly by codegen_runtime.c's emit_string_literals() when it
 * needs to build a single line character-by-character rather than
 * with one emit() call per line. */
FILE *g_out;

static int g_labelctr = 0;

/* Every compiler-internal label cc64 invents (for if/else branches,
 * loop tops/bottoms, short-circuit evaluation, ...) is one of these:
 * "__L0", "__L1", "__L2", .... The leading double underscore matters -
 * see cc64.h's note on the reserved-identifier namespace these all
 * live in, which is what guarantees a generated label can never
 * collide with a name the user's C source could possibly have chosen. */
char *newlabel(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "__L%d", g_labelctr++);
    return xstrdup(buf);
}

void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
    fprintf(g_out, "\n");
}

/* ===================================================================
 * The "far branch" workaround
 * ===================================================================
 * The 6502's conditional branch instructions (BEQ, BNE, BMI, ...) are
 * *relative* branches: the instruction only encodes an 8-bit signed
 * offset, so they can only jump up to 127 bytes forward or 128 bytes
 * backward. That's easily too short to reach past a long if-body or
 * loop-body once real C programs are being compiled - codegen_stmt.c
 * has no way to know in advance how much code a branch will need to
 * jump over.
 *
 * The standard fix (used by real assemblers and compilers targeting
 * this kind of CPU) is: never emit a long-range conditional branch
 * directly. Instead, branch on the OPPOSITE condition to a label
 * right after an unconditional JMP (which, being absolute, can reach
 * anywhere in memory), and put the real target after that JMP:
 *
 *     BNE skip      ; opposite of BEQ - skip the jump if NOT equal
 *     JMP target    ; unconditional, unlimited range
 *   skip:
 *
 * This costs a few extra bytes over a direct branch, but means
 * codegen never has to reason about distances at all - every
 * structural branch (if/while/for, short-circuit && / ||) goes
 * through emit_far_branch() and is correct regardless of how much
 * code sits between the branch and its target.
 * =================================================================== */

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
    return NULL; /* unreachable */
}
void emit_far_branch(const char *mnem, const char *target) {
    char *skip = newlabel();
    emit("    %s %s", invert_branch_mnem(mnem), skip);
    emit("    JMP %s", target);
    emit("%s:", skip);
}

/* ===================================================================
 * Label formatting for user-visible symbols (globals, locals, and
 * function entry points). All follow the same __g_/__fn_ reserved-
 * namespace convention described in cc64.h.
 * =================================================================== */

void global_label(char *out, size_t outsz, const char *name) {
    snprintf(out, outsz, "__g_%s", name);
}
/* A local or parameter's label is qualified by its owning function's
 * name, since (unlike globals) two different functions can have a
 * local variable with the same name without colliding - e.g. both
 * `add` and `subtract` can have a local called `result`. */
void local_label(char *out, size_t outsz, const char *fn, const char *name) {
    snprintf(out, outsz, "__fn_%s_v_%s", fn, name);
}
void func_label(char *out, size_t outsz, const char *name) {
    snprintf(out, outsz, "__fn_%s", name);
}
