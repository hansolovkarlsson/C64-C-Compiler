/*
 * symtab.c - symbol tables, struct definitions, and a small type-
 * inference helper built on top of them.
 *
 * cc64 keeps flat, fixed-size arrays instead of anything fancier like
 * a hash table or a scoped stack of scopes:
 *
 *   g_globals[]  every global variable (declared at file scope)
 *   g_funcs[]    every function (signature only - see FnSym in cc64.h)
 *   g_locals[]   every local variable/parameter of *the function
 *                currently being parsed*, reset to empty at the start
 *                of each function (see pass_b() in parser.c)
 *   g_structs[]  every `struct Tag { ... }` seen anywhere in the file
 *
 * Linear search through these (find_global/find_func/find_local) is
 * plenty fast here: real C64 programs compiled with a "minimal C"
 * subset like this one have at most a few hundred symbols, so an
 * O(n) scan costs nothing worth optimizing away. A production
 * compiler would use a hash table; a teaching one gets to skip that
 * complexity because it doesn't change any of the *interesting* parts
 * of how the compiler works.
 *
 * g_locals only ever needs to hold ONE function's worth of locals at
 * a time, because pass_b() (parser.c) fully parses and compiles each
 * function before starting the next - there's never a moment where
 * two functions' locals are both "in scope" from the COMPILER's point
 * of view. (Note this is a fact about compile time, not run time:
 * at run time, recursion means many invocations can be live at once,
 * which the frame save/restore machinery in codegen_stmt.c handles -
 * but they all share the same set of names and addresses, which is
 * all the symbol table cares about.)
 *
 * g_structs[], unlike the others, is populated ENTIRELY by pass_a()
 * (parser.c) - a struct's body is just a list of member declarations,
 * with no expressions or statements to defer to a second pass, so
 * there's nothing pass_b() needs to do with structs at all.
 */

#include "cc64.h"

GSym g_globals[1024];
int g_nglobals = 0;

FnSym g_funcs[512];
int g_nfuncs = 0;

LSym g_locals[256];
int g_nlocals = 0;
FnSym *g_curfn = NULL; /* NULL at file scope; set by pass_b() while parsing one function */

StructDef g_structs[64];
int g_nstructs = 0;

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

void register_local(const char *name, int type, int isPointer, int structTag,
                     int isArray, int arrLen, int isParam, int line) {
    if (find_local(name)) fatal(line, "redefinition of '%s'", name);
    if (g_nlocals >= (int)(sizeof(g_locals)/sizeof(g_locals[0])))
        fatal(line, "too many locals in function '%s'", g_curfn->name);
    LSym *l = &g_locals[g_nlocals++];
    memset(l, 0, sizeof(*l));
    strncpy(l->name, name, sizeof(l->name)-1);
    l->type = type; l->isPointer = isPointer; l->structTag = structTag;
    l->isArray = isArray; l->arrLen = arrLen; l->isParam = isParam;
}

/* ===================================================================
 * Struct definitions
 * ===================================================================
 * A struct can be referenced by name (`struct Tag *p;`) before its
 * own `{ members }` body has been parsed - the classic case being a
 * struct that points to itself (`struct Node { ... struct Node
 * *next; };`), but the same mechanism just as easily supports two
 * different structs that point to each other regardless of which is
 * written first. Since every struct-typed thing in cc64 is either
 * "the struct itself" (fixed-size, needs a known size) or "a pointer
 * to it" (always 2 bytes, needs no size at all), a pointer reference
 * never actually needs the pointee to be complete yet - only USING a
 * struct BY VALUE (a variable of that type, or a member with that
 * type - which isn't allowed anyway, see StructDef's comment in
 * cc64.h) needs its size to be known, by which point parsing has
 * necessarily moved past its `{ members }` if it's ever going to.
 *
 * find_or_create_struct_tag() is how both cases - starting a new
 * definition, and merely referencing a tag - go through the same
 * lookup: if the tag doesn't exist yet, create an INCOMPLETE entry
 * for it (defined=0) rather than erroring immediately. Only code that
 * actually needs the struct's size (see require_complete_struct()
 * below) checks `defined` and fails there if it's still incomplete. */
int find_or_create_struct_tag(const char *name) {
    for (int i = 0; i < g_nstructs; i++)
        if (strcmp(g_structs[i].name, name) == 0) return i;
    if (g_nstructs >= (int)(sizeof(g_structs)/sizeof(g_structs[0])))
        fatal(0, "too many struct types");
    StructDef *sd = &g_structs[g_nstructs++];
    memset(sd, 0, sizeof(*sd));
    strncpy(sd->name, name, sizeof(sd->name)-1);
    return g_nstructs - 1;
}

StructMember *find_struct_member(int structTag, const char *name, int line) {
    StructDef *sd = &g_structs[structTag];
    for (int i = 0; i < sd->nmembers; i++)
        if (strcmp(sd->members[i].name, name) == 0) return &sd->members[i];
    fatal(line, "struct '%s' has no member '%s'", sd->name, name);
    return NULL; /* unreachable */
}

/* Used anywhere a struct's SIZE is about to matter (a by-value
 * variable/array, sizeof-style offset math, ...) rather than just its
 * existence as a pointer target. */
void require_complete_struct(int structTag, int line) {
    if (!g_structs[structTag].defined)
        fatal(line, "struct '%s' is used here but never fully defined "
                     "(only pointers to an incomplete struct are allowed)",
                     g_structs[structTag].name);
}

/* ===================================================================
 * Minimal expression type inference
 * ===================================================================
 * cc64 doesn't have a general type checker - internally, almost every
 * expression's *value* is just treated as a 16-bit quantity in a
 * register, and the compiler doesn't track a full type for every AST
 * node the way a production C compiler would. infer_type() exists
 * because a few things genuinely can't be done correctly without
 * knowing whether a value is a pointer, and if so, a pointer to what
 * (now including "to which struct"):
 *
 *   - Pointer arithmetic needs to scale by the pointee's size
 *     (`p + 1` on an int* must move 2 bytes, not 1; on a struct
 *     pointer, by that struct's full size).
 *   - Dereferencing something that isn't a pointer is a real bug in
 *     the user's program, worth catching at compile time.
 *   - Passing a plain int where a function expects a pointer (or vice
 *     versa) is almost always a mistake, and cheap to catch here.
 *   - `a.b` needs to know which struct `a` is, to look up member `b`.
 *
 * So infer_type() answers "is this expression a pointer, to what base
 * type, and (if a struct) which one?" by recursively looking at how a
 * node's value would be produced. It does NOT check things like
 * whether both sides of a `+` make sense together in general, or
 * anything about `char` vs `int` promotion. Everything not explicitly
 * pointer- or struct-related just falls through to "plain int" in the
 * default case, which is safe because the only things infer_type()'s
 * callers ever ask are "isPointer?" and (for N_MEMBER) "which struct?".
 * =================================================================== */

CType infer_type(Node *n) {
    switch (n->kind) {
        case N_NUM: return (CType){ TY_INT, 0, -1 };
        case N_STR: return (CType){ TY_CHAR, 1, -1 }; /* string literals are char* values */
        case N_IDENT: {
            /* Locals shadow globals, so check the current function's
             * locals first - same lookup order gen_expr_to_R() and
             * resolve_lvalue_base() use when actually generating code
             * for a variable reference, and it has to match exactly or
             * this function would report the wrong type for a shadowed
             * global. */
            LSym *l = g_curfn ? find_local(n->name) : NULL;
            if (l) {
                if (l->isArray) return (CType){ l->type, 1, l->structTag }; /* array decays to pointer */
                return (CType){ l->type, l->isPointer, l->structTag };
            }
            GSym *g = find_global(n->name);
            if (g) {
                if (g->isArray) return (CType){ g->type, 1, g->structTag };
                return (CType){ g->type, g->isPointer, g->structTag };
            }
            fatal(n->line, "undeclared identifier '%s'", n->name);
            return (CType){ TY_INT, 0, -1 }; /* unreachable; fatal() doesn't return */
        }
        case N_INDEX: {
            /* Indexing always yields a plain scalar or struct in this
             * version - there are no arrays-of-pointers and no
             * pointer-to-pointer, so `p[i]`'s result can never itself
             * be a pointer (it CAN be a struct, e.g. a struct array's
             * element - handled the same way a plain struct variable
             * is by anything that consumes this CType). */
            CType base = infer_type(n->a);
            return (CType){ base.base, 0, base.structTag };
        }
        case N_MEMBER: {
            CType base = infer_type(n->a);
            if (base.base != TY_STRUCT) fatal(n->line, "'.'/'->' used on a non-struct value");
            if (base.isPointer) fatal(n->line, "internal: N_MEMBER's base should already be dereferenced");
            require_complete_struct(base.structTag, n->line);
            StructMember *m = find_struct_member(base.structTag, n->name, n->line);
            return (CType){ m->type, m->isPointer, m->structTag };
        }
        case N_DEREF: {
            CType base = infer_type(n->a);
            if (!base.isPointer) fatal(n->line, "cannot dereference a non-pointer value");
            return (CType){ base.base, 0, base.structTag };
        }
        case N_ADDR: {
            CType base = infer_type(n->a);
            return (CType){ base.base, 1, base.structTag };
        }
        case N_CALL: {
            if (is_builtin(n->name)) return (CType){ TY_INT, 0, -1 };
            FnSym *fn = find_func(n->name);
            if (!fn) fatal(n->line, "call to undeclared function '%s'", n->name);
            return (CType){ fn->retType < 0 ? TY_INT : fn->retType, fn->retIsPointer, fn->retStructTag };
        }
        /* Assignment and inc/dec expressions have the type of their
         * target (e.g. `p = q` and `p++` both have p's type). */
        case N_ASSIGN: case N_COMPOUND_ASSIGN:
        case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC:
            return infer_type(n->a);
        case N_BINOP: {
            /* Pointer arithmetic: `p + n`, `n + p`, and `p - n` are
             * all still pointers (to the same base type/struct); `p -
             * q` (pointer minus pointer) is an element COUNT - a plain
             * int - not a pointer. Everything else (both operands
             * plain) is a plain int. This has to match what
             * gen_expr_to_R()'s N_BINOP case in codegen_expr.c
             * actually generates, or expressions like f(s + 1) would
             * be misjudged by the argument type checks. */
            if (strcmp(n->op, "+") == 0 || strcmp(n->op, "-") == 0) {
                CType ta = infer_type(n->a), tb = infer_type(n->b);
                if (ta.isPointer && tb.isPointer)
                    return (CType){ TY_INT, 0, -1 }; /* ptr - ptr = element count */
                if (ta.isPointer) return ta;
                if (tb.isPointer) return tb;
            }
            return (CType){ TY_INT, 0, -1 };
        }
        default:
            return (CType){ TY_INT, 0, -1 }; /* logicals, literals, unary -/!/~ */
    }
}

/* How many bytes a value of this shape occupies in memory. A pointer
 * is always 2 bytes (a 16-bit address) no matter what it points to -
 * that's the one case this can't be answered from `type` alone, which
 * is why isPointer is checked first. A by-value struct's width is its
 * full size (and must already be complete by the time anything asks
 * this - callers needing a line number for a clearer error should
 * call require_complete_struct-style checks themselves first; this
 * function has no line to report one with). */
int var_width(int type, int isPointer, int structTag) {
    if (isPointer) return 2;
    if (type == TY_STRUCT) return g_structs[structTag].size;
    return (type == TY_INT) ? 2 : 1;
}
