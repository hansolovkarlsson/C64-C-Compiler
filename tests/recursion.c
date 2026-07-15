/* recursion feature test for cc64 */

void print_uint(int n) {
    char buf[6];
    int i;
    if (n == 0) { putchar('0'); return; }
    i = 0;
    while (n > 0) {
        buf[i] = n % 10;
        n = n / 10;
        i = i + 1;
    }
    while (i > 0) {
        i = i - 1;
        putchar(buf[i] + 48);
    }
}
void print_int(int n) {
    if (n < 0) { putchar('-'); print_uint(-n); } else { print_uint(n); }
}
void nl(void) { putchar('\n'); }

/* the classic: direct recursion, one recursive call per level */
int fact(int n) {
    if (n <= 1) return 1;
    return n * fact(n - 1);
}

/* two recursive calls per level: exercises an operand held on the
 * expression stack across a recursive call (the fib(n-1) result must
 * survive the entire fib(n-2) subtree's execution) */
int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

/* mutual (indirect) recursion */
int is_even(int n);
int is_odd(int n) {
    if (n == 0) return 0;
    return is_even(n - 1);
}
int is_even(int n) {
    if (n == 0) return 1;
    return is_odd(n - 1);
}

/* recursion where each level has its own LOCAL working data that must
 * survive the recursive call: classic recursive decimal printer */
void rprint_uint(int n) {
    int digit;
    digit = n % 10;
    if (n >= 10) rprint_uint(n / 10);
    putchar(digit + 48);
}

/* recursion with a pointer parameter: recursive strlen */
int rstrlen(char *s) {
    if (*s == 0) return 0;
    return 1 + rstrlen(s + 1);
}

/* recursion mutating shared global state through a pointer, to check
 * pointers still point at the ONE real variable (not a per-frame
 * copy) across recursive frames */
int g_calls = 0;
int depth_count(int n, int *counter) {
    *counter = *counter + 1;
    if (n <= 1) return 1;
    return 1 + depth_count(n - 1, counter);
}

/* recursion with a local array per level: each level fills its own
 * copy and verifies it after the inner levels ran (would fail if
 * frames weren't saved/restored, since all levels share addresses) */
int arr_check(int n) {
    int a[4];
    int i;
    for (i = 0; i < 4; i = i + 1) a[i] = n * 10 + i;
    if (n > 1) {
        if (arr_check(n - 1) == 0) return 0;
    }
    for (i = 0; i < 4; i = i + 1) {
        if (a[i] != n * 10 + i) return 0;
    }
    return 1;
}

/* deep-ish recursion: sums 1..n recursively; n=80 means 80 frames of
 * (2-byte n) on the call stack and 80 return addresses on the
 * hardware stack - comfortably inside both limits, but far beyond
 * anything that would work by accident if frames weren't real */
int rsum(int n) {
    if (n == 0) return 0;
    return n + rsum(n - 1);
}

int garr[4];
int gother[4];
int identity(int x) { return x; }

void main(void) {
    int c;

    print_int(fact(5)); nl();            /* 120 */
    print_int(fact(7)); nl();            /* 5040 */
    print_int(fib(10)); nl();            /* 55 */
    print_int(fib(15)); nl();            /* 610 */
    print_int(is_even(10)); nl();        /* 1 */
    print_int(is_even(7)); nl();         /* 0 */
    print_int(is_odd(9)); nl();          /* 1 */

    rprint_uint(9); nl();                /* 9 */
    rprint_uint(408); nl();              /* 408 */
    rprint_uint(31337); nl();            /* 31337 */

    print_int(rstrlen("hello")); nl();   /* 5 */
    print_int(rstrlen("")); nl();        /* 0 */

    c = 0;
    print_int(depth_count(6, &c)); nl(); /* 6 */
    print_int(c); nl();                  /* 6 */

    print_int(arr_check(5)); nl();       /* 1 */

    print_int(rsum(80)); nl();           /* 3240 */

    /* regression check for the nested array-target assignment bug
     * found (and fixed) while building recursion support: the outer
     * assignment's saved target address must survive the inner
     * assignment saving ITS target address. 7 flows through both
     * assignments; both elements and the result must all be 7. */
    garr[2] = (gother[1] = identity(7)) + 0;
    print_int(garr[2]); nl();            /* 7 */
    print_int(gother[1]); nl();          /* 7 */

    puts("DONE.");
}
