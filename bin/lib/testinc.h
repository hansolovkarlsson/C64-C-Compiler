/* testinc.h - a small quoted, local header used by tests/include.c to
 * check that #include "..." resolves relative to the including file,
 * and that a header can itself #include <...> a library header
 * (nested includes), with correct include-once behavior either way. */

#include <string.h>  /* pulled in again here on purpose - already
                       * included by include.c itself before this file
                       * is reached; must not be duplicated/redefined */

int count_matching(char *a, char *b) {
    int n;
    n = 0;
    while (*a && *b) {
        if (*a == *b) n = n + 1;
        a = a + 1;
        b = b + 1;
    }
    return n;
}
