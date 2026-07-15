/*
 * recursion.c - a semantic check, run after pass_a() but before any
 * code is generated: build a call graph over every function in the
 * program, and reject it if that graph has a cycle.
 *
 * WHY THIS EXISTS
 * -----------------
 * As explained in cc64.h's architecture overview, every function's
 * parameters and locals live at a single FIXED memory address (there's
 * no per-call stack frame). That's fine as long as a function's own
 * storage isn't still "in use" by an outer, unfinished call to the
 * same function - which is exactly what recursion would violate: a
 * recursive call would overwrite the outer call's parameters/locals
 * while the outer call still needs them, silently corrupting its
 * state. Rather than let that happen at runtime (where it would look
 * like nonsensical, hard-to-debug behavior), this check catches it at
 * compile time with a clear error naming the actual cycle.
 *
 * HOW IT WORKS
 * -------------
 * This is NOT a real parse of each function body - that would mean
 * doing much of the parser's work twice. Instead, build_call_graph()
 * does a cheap token-level scan: for every function's body (the
 * [bodyStart, bodyEnd) token range recorded by pass_a()), look for the
 * two-token pattern `IDENT (` and, if that identifier names a known
 * function, record a call-graph edge from the enclosing function to
 * it. This can occasionally over-approximate - e.g. it has no way to
 * know that some IDENT is actually a shadowing local variable rather
 * than a real call target - but that only risks a false-positive
 * compile error, never a missed real bug, which is the right tradeoff
 * here: a false "recursion not supported" error is a minor annoyance;
 * silently miscompiling a recursive function is a real bug in
 * someone's program.
 *
 * Once the graph is built, check_no_recursion() runs a standard
 * depth-first-search cycle detection over it (the three-color/
 * "on stack" DFS you'd see in any algorithms course), and if it finds
 * a back-edge - a call to a function that's still on the current DFS
 * path - that back-edge IS a recursive cycle, direct or indirect, and
 * report_cycle() prints the whole chain of calls that forms it.
 */

#include "cc64.h"

#define MAX_CALL_EDGES 4096
typedef struct { int from, to; } CallEdge; /* both are indices into g_funcs[] */
static CallEdge g_edges[MAX_CALL_EDGES];
static int g_nedges = 0;

/* Like find_func() in symtab.c, but returns an index into g_funcs[]
 * rather than a pointer - convenient here since the DFS below tracks
 * per-function state (g_dfs_state[]/g_dfs_path[]) in arrays indexed
 * the same way. */
static int fn_index(const char *name) {
    for (int i = 0; i < g_nfuncs; i++)
        if (strcmp(g_funcs[i].name, name) == 0) return i;
    return -1;
}

static void build_call_graph(void) {
    for (int fi = 0; fi < g_nfuncs; fi++) {
        FnSym *fn = &g_funcs[fi];
        if (!fn->defined) continue; /* prototype-only; nothing to scan */
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

/* Standard DFS cycle-detection coloring: 0 = never visited, 1 = on
 * the current DFS path (an ancestor of the node currently being
 * explored), 2 = fully explored (done, and known cycle-free). Finding
 * an edge to a node colored 1 is exactly what "found a cycle" means -
 * it's a call back to something that (directly or transitively)
 * called the current function and hasn't returned yet. */
static int g_dfs_state[512];
static int g_dfs_path[512]; /* the current DFS path, for printing the cycle */
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
            /* Found a cycle. It doesn't necessarily start at `node` -
             * e.g. a->b->c->b is a cycle among b and c, even though
             * the DFS started at a - so find where `to` first appears
             * in the current path and report from there. */
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

/* Every function is a potential DFS root, since the call graph isn't
 * necessarily connected (e.g. two independent groups of functions
 * that never call each other) - a plain single DFS from `main` alone
 * wouldn't necessarily visit every function. */
void check_no_recursion(void) {
    build_call_graph();
    for (int i = 0; i < g_nfuncs; i++) g_dfs_state[i] = 0;
    for (int i = 0; i < g_nfuncs; i++) {
        if (!g_funcs[i].defined) continue;
        if (g_dfs_state[i] == 0) { g_dfs_depth = 0; dfs_check(i); }
    }
}
