/* #include / standard library feature test for cc64 */

#include <string.h>
#include <print.h>
#include <string.h>   /* deliberately repeated: must be a no-op (include-once) */
#include "testinc.h"  /* quoted, local to tests/; itself #includes <string.h> again */

char buf[32];
char buf2[32];

void main(void) {
    /* strlen */
    print_int(strlen("hello")); newline();       /* 5 */
    print_int(strlen("")); newline();             /* 0 */

    /* strcpy + strcat, and their return value chained straight
     * into puts() (return dst, matching real libc) */
    strcpy(buf, "hello, ");
    strcat(buf, "world!");
    puts(buf); newline();                          /* hello, world! */
    puts(strcpy(buf2, "chained")); newline();       /* chained */

    /* strcmp: equal, less, greater */
    print_int(strcmp("abc", "abc")); newline();     /* 0 */
    print_int(strcmp("abc", "abd") < 0); newline();  /* 1 */
    print_int(strcmp("abd", "abc") > 0); newline();  /* 1 */
    print_int(strcmp("ab", "abc") < 0); newline();   /* 1  (prefix is "less") */

    /* strchr: found and not-found */
    puts(strchr("hello, world!", 'w')); newline();  /* world! */
    print_int(strchr("hello", 'z') == 0); newline(); /* 1 (NULL) */

    /* memset / memcpy */
    memset(buf, 'A', 5);
    buf[5] = 0;
    puts(buf); newline();                            /* AAAAA */
    memcpy(buf2, buf, 6);
    puts(buf2); newline();                           /* AAAAA */

    /* print_hex: exercises the "sign-extension doesn't contaminate
     * the extracted nibble" property for both a positive and a
     * negative (top-bit-set) value */
    print_hex(0); newline();      /* 0000 */
    print_hex(255); newline();    /* 00FF */
    print_hex(4096); newline();   /* 1000 */
    print_hex(-1); newline();     /* FFFF */
    print_hex(-4096); newline();  /* F000 */

    /* the local header's function, and its own nested include of
     * string.h working correctly */
    print_int(count_matching("abcdef", "abzdxf")); newline(); /* 4 */

    puts("DONE.");
}
