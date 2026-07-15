/*
 * print.h - fixed-arity print helpers, since cc64 has no variadic
 * functions and so no real printf(). print_int(n) instead of
 * printf("%d", n); chain calls together (with putchar/puts for
 * literal text in between) for anything printf would normally do in
 * one call. All of these build on putchar(), so they automatically
 * get its PETSCII case handling - see the README's "PETSCII and
 * case" section if that's unfamiliar.
 *
 * NOTABLY MISSING: print_uint(), an unsigned decimal printer. It's
 * left out deliberately rather than shipped broken: cc64's `int` is
 * always signed, with no unsigned type and no cast operator, so
 * there's no correct way to write "divide this 16-bit bit pattern as
 * if it were unsigned" using cc64's own `/` and `%` (which are always
 * signed) or its `<`/`>` comparisons (also always signed, except when
 * comparing pointers) - a print_uint built from ordinary cc64 code
 * would silently misprint any value from 32768-65535. print_hex()
 * below doesn't have this problem (bitwise `&` and `>>` extract the
 * same bits regardless of how you'd interpret their sign - see the
 * comment on it for why) and covers most of the same need: showing
 * the raw contents of a 16-bit value, address, or bit pattern.
 */

/* Prints n in decimal, with a leading '-' if negative. */
void print_int(int n) {
    char buf[6];
    int i;
    int neg;
    neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    if (n == 0) {
        putchar('0');
        return;
    }
    i = 0;
    while (n > 0) {
        buf[i] = n % 10;
        n = n / 10;
        i = i + 1;
    }
    if (neg) putchar('-');
    while (i > 0) {
        i = i - 1;
        putchar(buf[i] + 48);
    }
}

/* Prints n as 4 uppercase hex digits (0000-FFFF), the raw bit pattern
 * regardless of sign - useful for addresses (e.g. print_hex(&x)
 * won't compile since &x is a pointer, not an int, but
 * print_hex(peek(addr)) or printing a value built from shifts and
 * masks works fine). Extracting 4 bits at a time with `(n >> k) & 15`
 * gives the right answer even though cc64's `>>` sign-extends
 * negative values: the sign-extended copies only ever land ABOVE the
 * 4 bits `& 15` keeps, for every shift amount used here (0, 4, 8, 12),
 * so they never contaminate the digit being extracted. */
void print_hex(int n) {
    int i;
    int shift;
    int digit;
    for (i = 3; i >= 0; i = i - 1) {
        shift = i * 4;
        digit = (n >> shift) & 15;
        if (digit < 10) putchar(digit + 48);
        else putchar(digit - 10 + 65);
    }
}

/* Shorthand for putchar('\n') - a small thing, but it's the single
 * most-repeated line across this project's own test programs before
 * this library existed. */
void newline(void) {
    putchar('\n');
}
