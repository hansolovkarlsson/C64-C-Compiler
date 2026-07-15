/*
 * string.h - basic string/memory helpers, matching the real C
 * standard library's names and contracts wherever cc64's type system
 * allows it (no bounds checking anywhere, exactly like the real
 * thing - the caller is responsible for making sure destinations are
 * big enough).
 *
 * This is a HEADER-ONLY library: #include splices these definitions
 * directly into your program's token stream (see cc64.h's "HOW
 * #include WORKS" if you're curious), and every function you include
 * gets fully compiled into your program whether you call it or not -
 * there's no linker to strip out the unused ones. For the handful of
 * small functions here that's a non-issue; keep it in mind if this
 * ever grows into something bigger.
 *
 * All of these are written using pointers and (for strlen at least)
 * could be written recursively, but plain loops are used throughout -
 * partly because it's the more natural way to write them, and partly
 * to avoid burning any of the 256-byte-per-call-stack-frame or
 * expression-stack budget that heavier recursive algorithms in your
 * OWN program might need (see the README's "How recursion works").
 */

/* Number of characters before the terminating 0 byte. */
int strlen(char *s) {
    int n;
    n = 0;
    while (*s) {
        n = n + 1;
        s = s + 1;
    }
    return n;
}

/* Copies src (including its terminating 0) into dst. Returns dst,
 * matching the real strcpy's contract (so calls can be chained or
 * used directly as an argument, e.g. puts(strcpy(buf, "hi"))). */
char *strcpy(char *dst, char *src) {
    char *d;
    d = dst;
    while (*src) {
        *d = *src;
        d = d + 1;
        src = src + 1;
    }
    *d = 0;
    return dst;
}

/* Appends src (including its terminating 0) onto the end of dst.
 * Returns dst. */
char *strcat(char *dst, char *src) {
    char *d;
    d = dst;
    while (*d) d = d + 1;
    while (*src) {
        *d = *src;
        d = d + 1;
        src = src + 1;
    }
    *d = 0;
    return dst;
}

/* 0 if a and b are equal; otherwise the difference between the first
 * pair of differing bytes (negative if a's byte is smaller, positive
 * if larger) - matching real strcmp, including that it compares bytes
 * as unsigned (which cc64's char already is - see the README's note
 * on char being unsigned - so this needs no special casing for that,
 * unlike a real C library where char's signedness is platform-defined). */
int strcmp(char *a, char *b) {
    while (*a && *a == *b) {
        a = a + 1;
        b = b + 1;
    }
    return *a - *b;
}

/* Pointer to the first occurrence of the byte c in s, or 0 (NULL) if
 * it isn't found. Like the real strchr, searching for 0 finds the
 * string's own terminator rather than always failing. */
char *strchr(char *s, int c) {
    while (*s) {
        if (*s == c) return s;
        s = s + 1;
    }
    if (c == 0) return s;
    return 0;
}

/* Fills the first n bytes of dst with the low byte of val. Returns
 * dst. */
char *memset(char *dst, int val, int n) {
    int i;
    for (i = 0; i < n; i = i + 1) {
        dst[i] = val;
    }
    return dst;
}

/* Copies n bytes from src to dst. Like the real memcpy, the source
 * and destination must not overlap (there's no overlap-safe fallback
 * the way memmove would provide - this library doesn't have one). */
char *memcpy(char *dst, char *src, int n) {
    int i;
    for (i = 0; i < n; i = i + 1) {
        dst[i] = src[i];
    }
    return dst;
}
