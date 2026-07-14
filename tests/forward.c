/* forward-reference / declaration-order test: callee defined AFTER caller,
 * and vice versa, must both work since cc64 does a signature pre-scan. */

int caller_calls_later(int x) {
    return later_defined(x) + 1;
}

int later_defined(int x) {
    return x * 2;
}

int earlier_defined(int x) {
    return x + 100;
}

int caller_calls_earlier(int x) {
    return earlier_defined(x);
}

char g_flag = 1;
int g_neg = -5;

void main(void) {
    putchar(48 + (caller_calls_later(10) == 21));   /* '1' if correct */
    putchar(48 + (caller_calls_earlier(5) == 105)); /* '1' if correct */
    putchar(48 + (g_flag == 1));                    /* '1' */
    putchar(48 + (g_neg == -5));                    /* '1' */
    putchar('\n');
}
