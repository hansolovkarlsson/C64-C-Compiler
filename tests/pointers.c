/* pointer feature test for cc64 */

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

int garr[5];
char gcbuf[17];

/* swap via pointers - the classic test */
void swap(int *a, int *b) {
    int t;
    t = *a;
    *a = *b;
    *b = t;
}

/* sum an int array via pointer traversal (arrays decay when passed) */
int sum_via_ptr(int *p, int n) {
    int total;
    int i;
    total = 0;
    for (i = 0; i < n; i = i + 1) {
        total = total + *p;
        p = p + 1;
    }
    return total;
}

/* same, but using pointer comparison against an end pointer instead of a counter */
int sum_via_ptr_cmp(int *p, int n) {
    int total;
    int *end;
    total = 0;
    end = p + n;
    while (p < end) {
        total = total + *p;
        p++;
    }
    return total;
}

/* fill a char buffer via pointer, return count written (no string.h here) */
int copy_str(char *dst, char *src) {
    int n;
    n = 0;
    while (*src) {
        *dst = *src;
        dst++;
        src++;
        n = n + 1;
    }
    *dst = 0;
    return n;
}

int *global_ptr_to_garr0(void) {
    return &garr[0];
}

void main(void) {
    int x;
    int *p;
    int a;
    int b;
    char *s;
    int i;

    /* basic address-of / dereference on a scalar */
    x = 5;
    p = &x;
    print_int(*p); nl();        /* 5 */
    *p = 42;
    print_int(x); nl();         /* 42 */

    /* classic pointer swap */
    a = 1; b = 2;
    swap(&a, &b);
    print_int(a); putchar(' '); print_int(b); nl();  /* 2 1 */

    /* array decay + pointer traversal */
    for (i = 0; i < 5; i = i + 1) garr[i] = i + 1;   /* 1,2,3,4,5 */
    print_int(sum_via_ptr(garr, 5)); nl();            /* 15 */
    print_int(sum_via_ptr_cmp(garr, 5)); nl();        /* 15 */

    /* address of an array element, then walk with ++ */
    p = &garr[1];
    print_int(*p); nl();        /* 2 */
    p++;
    print_int(*p); nl();        /* 3 */
    p--;
    p--;
    print_int(*p); nl();        /* 1 */

    /* pointer arithmetic: p + n, p - n, ptrdiff (pointer - pointer) */
    p = garr;
    p = p + 3;
    print_int(*p); nl();        /* 4 */
    p = p - 1;
    print_int(*p); nl();        /* 3 */
    print_int((&garr[4]) - (&garr[0])); nl();  /* 4  (element count, not byte count) */

    /* char* pointer arithmetic (element size 1, no scaling) */
    gcbuf[0] = 'A'; gcbuf[1] = 'B'; gcbuf[2] = 'C'; gcbuf[3] = 0;
    s = gcbuf;
    putchar(*s); s++; putchar(*s); s++; putchar(*s); nl();  /* ABC */

    /* pointer comparisons (unsigned-correct) */
    print_int(&garr[0] < &garr[4]); nl();   /* 1 */
    print_int(&garr[4] < &garr[0]); nl();   /* 0 */
    print_int(&garr[2] == &garr[2]); nl();  /* 1 */

    /* string literal as a real char* value, and copy_str/puts with a
     * runtime-computed pointer (not the fast literal path) */
    s = "hello, pointers!";
    print_int(copy_str(gcbuf, s)); nl();    /* 16 */
    puts(gcbuf); nl();                       /* hello, pointers! */
    puts(s); nl();                           /* hello, pointers! (from the literal directly) */

    /* a function returning a pointer */
    p = global_ptr_to_garr0();
    print_int(*p); nl();        /* 1 */

    /* *p as a compound-assignment / inc-dec target */
    p = &garr[0];
    *p += 100;
    print_int(garr[0]); nl();   /* 101 */
    (*p)++;
    print_int(garr[0]); nl();   /* 102 */

    puts("DONE.");
}
