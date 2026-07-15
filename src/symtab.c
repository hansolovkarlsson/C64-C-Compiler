/*
 * symtab.c - symbol tables, and a small type-inference helper built on
 * top of them.
 *
 * cc64 keeps three flat, fixed-size arrays instead of anything fancier
 * like a hash table or a scoped stack of scopes:
 *
 *   g_globals[]  every global variable (declared at file scope)
 *   g_funcs[]    every function (signature only - see FnSym in cc64.h)
 *   g_locals[]   every local variable/parameter of *the function
 *                currently being parsed*, reset to empty at the start
 *                of each function (see pass_b() in parser.c)
 *
 * Linear search through these (find_global/find_func/find_local) is
 * plenty fast here: real C64 programs compiled with a "minimal C"
 * subset like this one have at most a few hundred symbols, so an
 * O(n) scan costs nothing worth optimizing away. A production
 * compiler would use a hash table; a teaching one gets to skip that
 * complexity because it doesn't change any of the *interesting* parts
 * of how the compiler works.
 *
 * Because there's no C-level recursion (see cc64.h's architecture
 * overview), g_locals only ever needs to hold ONE function's worth of
 * locals at a time - there's no call stack of "which function's
 * locals are currently in scope" to maintain, which is a nice
 * simplification this compiler gets almost for free from its storage
 * model.
 */

#include "cc64.h"

GSym g_globals[1024];
int g_nglobals = 0;

FnSym g_funcs[512];
int g_nfuncs = 0;

LSym g_locals[256];
int g_nlocals = 0;
FnSym *g_curfn = NULL; /* NULL at file scope; set by pass_b() while parsing one function */

GSym *find_global(const char *name) {
    for (int i = 0; i < g_nglobals; i++)
        if (strcmp(g_globals[i].name, name) == 0) return &g_globals[i];
    return NULL;
}
FnSym *find_func(const char *name) {
    for (int i = 0; i < g_nfuncs; i++)
        if (strcmp(g_funcs[i].name, name) == 0) return &g_funcs[i];
    return NULL;
}
LSym *find_local(const char *name) {
    for (int i = 0; i < g_nlocals; i++)
        if (strcmp(g_locals[i].name, name) == 0) return &g_locals[i];
    return NULL;
}

/* putchar/puts/peek/poke are handled directly in codegen_expr.c's
 * gen_call() rather than being real, callable FnSym entries - they're
 * "magic" the compiler knows about, not user-definable functions. This
 * check exists so a user can't accidentally (or deliberately) declare
 * their own function with one of these names and have it silently
 * shadowed or conflict with the builtin - pass_a() calls this and
 * rejects the redefinition with a clear error instead. */
int is_builtin(const char *name) {
    return strcmp(name, "putchar") == 0 || strcmp(name, "puts") == 0 ||
           strcmp(name, "peek") == 0 || strcmp(name, "poke") == 0;
}

void register_local(const char *name, int type, int isPointer, int isArray,
                     int arrLen, int isParam, int line) {
    if (find_local(name)) fatal(line, "redefinition of '%s'", name);
    if (g_nlocals >= (int)(sizeof(g_locals)/sizeof(g_locals[0])))
        fatal(line, "too many locals in function '%s'", g_curfn->name);
    LSym *l = &g_locals[g_nlocals++];
    memset(l, 0, sizeof(*l));
    strncpy(l->name, name, sizeof(l->name)-1);
    l->type = type; l->isPointer = isPointer; l->isArray = isArray; l->arrLen = arrLen; l->isParam = isParam;
}

/* ===================================================================
 * Minimal expression type inference
 * ===================================================================
 * cc64 doesn't have a general type checker - internally, almost every
 * expression's *value* is just treated as a 16-bit quantity in a
 * register, and the compiler doesn't track a full type for every AST
 * node the way a production C compiler would. infer_type() exists
 * because a few things genuinely can't be done correctly without
 * knowing whether a value is a pointer, and if so, a pointer to what:
 *
 *   - Pointer arithmetic needs to scale by the pointee's size
 *     (`p + 1` on an int* must move 2 bytes, not 1).
 *   - Dereferencing something that isn't a pointer is a real bug in
 *     the user's program, worth catching at compile time.
 *   - Passing a plain int where a function expects a pointer (or vice
 *     versa) is almost always a mistake, and cheap to catch here.
 *
 * So infer_type() answers exactly one question - "is this expression
 * a pointer, and to what base type?" - by recursively looking at how
 * a node's value would be produced. It does NOT check things like
 * whether both sides of a `+` make sense together in general, or
 * anything about `char` vs `int` promotion. Everything not explicitly
 * pointer-related just falls through to "plain int" in the default
 * case, which is safe because the only thing infer_type()'s callers
 * ever ask is "isPointer?".
 * =================================================================== */

CType infer_type(Node *n) {
    switch (n->kind) {
        case N_NUM: return (CType){ TY_INT, 0 };
        case N_STR: return (CType){ TY_CHAR, 1 }; /* string literals are char* values */
        case N_IDENT: {
            /* Locals shadow globals, so check the current function's
             * locals first - same lookup order gen_expr_to_R() and
             * resolve_lvalue_base() use when actually generating code
             * for a variable reference, and it has to match exactly or
             * this function would report the wrong type for a shadowed
             * global. */
            LSym *l = g_curfn ? find_local(n->name) : NULL;
            if (l) {
                if (l->isArray) return (CType){ l->type, 1 }; /* array decays to pointer */
                return (CType){ l->type, l->isPointer };
            }
            GSym *g = find_global(n->name);
            if (g) {
                if (g->isArray) return (CType){ g->type, 1 };
                return (CType){ g->type, g->isPointer };
            }
            fatal(n->line, "undeclared identifier '%s'", n->name);
            return (CType){ TY_INT, 0 }; /* unreachable; fatal() doesn't return */
        }
        case N_INDEX: {
            /* Indexing always yields a plain scalar in this version -
             * there are no arrays-of-pointers and no pointer-to-pointer,
             * so `p[i]`'s result can never itself be a pointer. */
            CType base = infer_type(n->a);
            return (CType){ base.base, 0 };
        }
        case N_DEREF: {
            CType base = infer_type(n->a);
            if (!base.isPointer) fatal(n->line, "cannot dereference a non-pointer value");
            return (CType){ base.base, 0 };
        }
        case N_ADDR:
            return (CType){ infer_type(n->a).base, 1 };
        case N_CALL: {
            if (is_builtin(n->name)) return (CType){ TY_INT, 0 };
            FnSym *fn = find_func(n->name);
            if (!fn) fatal(n->line, "call to undeclared function '%s'", n->name);
            return (CType){ fn->retType < 0 ? TY_INT : fn->retType, fn->retIsPointer };
        }
        /* Assignment and inc/dec expressions have the type of their
         * target (e.g. `p = q` and `p++` both have p's type). */
        case N_ASSIGN: case N_COMPOUND_ASSIGN:
        case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC:
            return infer_type(n->a);
        default:
            return (CType){ TY_INT, 0 }; /* binops, logicals, literals, unary -/!/~ */
    }
}

/* How many bytes a value of this shape occupies in memory. A pointer
 * is always 2 bytes (a 16-bit address) no matter what it points to -
 * that's the one case this can't be answered from `type` alone, which
 * is why isPointer is checked first. */
int var_width(int type, int isPointer) { return (isPointer || type == TY_INT) ? 2 : 1; }
