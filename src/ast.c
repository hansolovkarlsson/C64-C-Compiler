/*
 * ast.c - the one function that creates AST nodes.
 *
 * The Node type itself is defined in cc64.h (not here), because it
 * needs to be visible to parser.c (which builds trees of them),
 * codegen_expr.c/codegen_stmt.c (which walk those trees to generate
 * code), and symtab.c (whose infer_type() walks expression nodes to
 * figure out their type). This file just holds the single small
 * function that allocates and zero-initializes one.
 *
 * Why a separate file for one tiny function? Partly to give the AST
 * its own clearly-named home in the source tree - "ast.c" is where
 * you'd look for it - and partly so it's obvious this is the *only*
 * place Node objects get allocated, which makes the tree-construction
 * logic easy to audit.
 */

#include "cc64.h"

/* Every field not explicitly set here starts at zero/NULL, which
 * matters: a lot of code relies on unused child pointers (a, b, c, d,
 * next) being NULL rather than garbage, e.g. "if (n->c) gen_stmt(n->c)"
 * to handle an optional else-branch. */
Node *node_new(NodeKind k, int line) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(*n));
    n->kind = k;
    n->line = line;
    return n;
}
