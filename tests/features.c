/* comprehensive feature test for cc64 */

int g_counter = 0;
int arr[6];
char cbuf[8];

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
    if (n < 0) {
        putchar('-');
        print_uint(-n);
    } else {
        print_uint(n);
    }
}

void nl(void) {
    putchar('\n');
}

int add(int a, int b) {
    return a + b;
}

int fact(int n) {
    int result;
    int i;
    result = 1;
    for (i = 2; i <= n; i = i + 1) {
        result = result * i;
    }
    return result;
}

int side_effect(void) {
    g_counter = g_counter + 1;
    return 1;
}

void main(void) {
    int i;
    int x;

    /* basic arithmetic */
    print_int(17 + 25); nl();          /* 42 */
    print_int(100 - 37); nl();         /* 63 */
    print_int(6 * 7); nl();            /* 42 */
    print_int(50 / 7); nl();           /* 7 */
    print_int(50 % 7); nl();           /* 1 */
    print_int(-7 / 2); nl();           /* -3  (truncation toward zero) */
    print_int(-7 % 2); nl();           /* -1  (sign follows dividend) */
    print_int(7 / -2); nl();           /* -3 */
    print_int(7 % -2); nl();           /* 1 */

    /* bitwise / shifts */
    print_int(12 & 10); nl();          /* 8 */
    print_int(12 | 3); nl();           /* 15 */
    print_int(12 ^ 5); nl();           /* 9 */
    print_int(1 << 10); nl();          /* 1024 */
    print_int(1024 >> 3); nl();        /* 128 */
    print_int(~0); nl();               /* -1 */

    /* comparisons */
    print_int(3 < 5); nl();            /* 1 */
    print_int(5 < 3); nl();            /* 0 */
    print_int(-1 < 1); nl();           /* 1  (signed compare) */
    print_int(-1 > 1); nl();           /* 0 */
    print_int(5 == 5); nl();           /* 1 */
    print_int(5 != 5); nl();           /* 0 */
    print_int(5 >= 5); nl();           /* 1 */
    print_int(5 <= 4); nl();           /* 0 */

    /* logical short-circuit: side_effect() must NOT run when short-circuited */
    g_counter = 0;
    x = (0 && side_effect());
    print_int(g_counter); nl();        /* 0 (right side skipped) */
    x = (1 || side_effect());
    print_int(g_counter); nl();        /* 0 (right side skipped) */
    x = (1 && side_effect());
    print_int(g_counter); nl();        /* 1 (right side runs) */

    /* functions, non-recursive call chains */
    print_int(add(19, 23)); nl();      /* 42 */
    print_int(fact(5)); nl();          /* 120 */

    /* arrays: fill, read back, compound-assign, inc/dec on elements */
    for (i = 0; i < 6; i = i + 1) {
        arr[i] = i * i;
    }
    for (i = 0; i < 6; i = i + 1) {
        print_int(arr[i]);
        putchar(' ');
    }
    nl();                               /* 0 1 4 9 16 25 */

    arr[2] += 100;
    print_int(arr[2]); nl();            /* 104 */
    arr[3]++;
    print_int(arr[3]); nl();            /* 10 */
    ++arr[4];
    print_int(arr[4]); nl();            /* 17 */
    arr[5]--;
    print_int(arr[5]); nl();            /* 24 */

    /* char array + peek/poke */
    cbuf[0] = 'X';
    poke(1024, 65);
    print_int(peek(1024)); nl();        /* 65 */
    putchar(cbuf[0]); nl();             /* uppercase X -> displays flipped case, see README */

    /* break / continue */
    x = 0;
    for (i = 0; i < 10; i = i + 1) {
        if (i == 3) continue;
        if (i == 7) break;
        x = x + i;
    }
    print_int(x); nl();                 /* 0+1+2+4+5+6 = 18 */

    /* pre/post inc on a plain variable, used as a value */
    i = 5;
    print_int(i++); nl();               /* 5 */
    print_int(i); nl();                 /* 6 */
    print_int(++i); nl();               /* 7 */

    puts("DONE.");
}
