/*
 * util.c - the handful of generic helpers every other file leans on:
 * a way to report an error and stop, and allocation wrappers that
 * check for out-of-memory so nothing else in the compiler has to.
 *
 * A compiler touches a lot of dynamically-allocated memory (tokens,
 * AST nodes, symbol names, ...) and none of it is performance
 * critical, so the pattern here is simple on purpose: if malloc/
 * realloc ever fails, just print a message and exit. A "real" compiler
 * might recover more gracefully, but for a teaching compiler, treating
 * out-of-memory as fatal keeps every call site a one-liner instead of
 * needing its own error path.
 */

#include "cc64.h"

/* Every compile error in cc64 goes through this one function, which is
 * why every error message looks the same ("cc64: error: line N: ...").
 * `line` is the source line the error applies to, or 0 if there isn't
 * a meaningful one (e.g. "too many string literals" isn't really about
 * one specific line). fmt/... work exactly like printf. */
void fatal(int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "cc64: error: ");
    if (line > 0) fprintf(stderr, "line %d: ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "cc64: out of memory\n"); exit(1); }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { fprintf(stderr, "cc64: out of memory\n"); exit(1); }
    return q;
}

/* strdup() isn't part of standard C99 (it's POSIX), so cc64 rolls its
 * own rather than depend on it - keeps the build portable everywhere
 * a C99 compiler exists, not just on POSIX systems. */
char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* Reads an entire file into a fresh, NUL-terminated buffer. Shared by
 * main.c (the top-level input file) and lexer.c (every file pulled in
 * by #include) - both just want "the whole file as a string" and
 * don't care how it got there. */
char *read_file(const char *path) {
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
