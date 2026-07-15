/* struct feature test for cc64 */

#include <print.h>

/* basic struct: direct member access, both read and write */
struct Point {
    int x;
    int y;
};

/* self-referential struct: the classic linked-list case */
struct Node {
    int val;
    struct Node *next;
};

/* mutually-referential structs (B is used, by pointer, inside A -
 * which is defined first - before B itself is defined at all) */
struct A {
    struct B *b;
    int tag;
};
struct B {
    struct A *a;
    int tag;
};

/* struct containing char members too, and a pointer member alongside
 * plain scalar ones, to check offsets are computed correctly when
 * widths differ (1, 2, 2 bytes) */
struct Mixed {
    char c;
    int n;
    char *s;
};

struct Point g_origin;
struct Point g_pts[4];
struct Node *g_head;

int point_sum(struct Point *p) {
    return p->x + p->y;
}

void point_move(struct Point *p, int dx, int dy) {
    p->x += dx;
    p->y += dy;
}

/* build a small linked list 1 -> 2 -> 3 -> 0(end), summing as we go,
 * to exercise self-referential pointer members and recursion +
 * structs working together */
int list_sum(struct Node *n) {
    if (n == 0) return 0;
    return n->val + list_sum(n->next);
}

int list_length(struct Node *n) {
    int count;
    count = 0;
    while (n != 0) {
        count = count + 1;
        n = n->next;
    }
    return count;
}

void main(void) {
    struct Point p;
    struct Point *pp;
    struct Node n1;
    struct Node n2;
    struct Node n3;
    int i;
    struct Mixed m;

    /* direct member access on a local struct variable - the
     * compile-time-constant "label+offset" fast path */
    p.x = 3;
    p.y = 4;
    print_int(p.x); newline();          /* 3 */
    print_int(p.y); newline();          /* 4 */
    print_int(p.x + p.y); newline();    /* 7 */

    /* &local_struct, then access through the resulting pointer with -> */
    pp = &p;
    print_int(pp->x); newline();        /* 3 */
    pp->x = 30;
    print_int(p.x); newline();          /* 30 - pp really points at p, not a copy */

    /* &p.x - address of a MEMBER, not the whole struct */
    {
        int *px;
        px = &p.x;
        *px = 99;
        print_int(p.x); newline();      /* 99 */
    }

    /* global struct, direct access */
    g_origin.x = 0;
    g_origin.y = 0;
    print_int(g_origin.x); newline();   /* 0 */

    /* struct as a function's pointer parameter, both read (->) and
     * write through it, and via a plain-int-returning helper */
    point_move(&p, 10, 20);
    print_int(p.x); newline();          /* 109 */
    print_int(p.y); newline();          /* 24 */
    print_int(point_sum(&p)); newline(); /* 133 */

    /* array of structs: constant AND runtime indexing, both direct
     * member writes and reads */
    for (i = 0; i < 4; i = i + 1) {
        g_pts[i].x = i * 10;
        g_pts[i].y = i * 100;
    }
    print_int(g_pts[2].x); newline();   /* 20 */
    print_int(g_pts[2].y); newline();   /* 200 */
    print_int(g_pts[0].x + g_pts[3].y); newline(); /* 0 + 300 = 300 */

    /* &arr[i].member - address of a member reached through array
     * indexing, then mutate through that pointer */
    {
        int *pe;
        pe = &g_pts[1].y;
        *pe = 12345;
        print_int(g_pts[1].y); newline(); /* 12345 */
    }

    /* self-referential struct: build a 3-node list by hand (no
     * malloc - these are plain locals, addresses taken with &) and
     * sum/measure it both iteratively and recursively */
    n1.val = 1; n1.next = &n2;
    n2.val = 2; n2.next = &n3;
    n3.val = 3; n3.next = 0;
    g_head = &n1;
    print_int(list_length(g_head)); newline(); /* 3 */
    print_int(list_sum(g_head)); newline();     /* 6 */

    /* mutually-referential structs: A points to B and back, defined
     * in either order in the source (A first, but it references B
     * before B's own body has been parsed) */
    {
        struct A a;
        struct B b;
        a.b = &b; a.tag = 1;
        b.a = &a; b.tag = 2;
        print_int(a.b->tag); newline();   /* 2 */
        print_int(b.a->tag); newline();   /* 1 */
        print_int(a.b->a->tag); newline(); /* 1 - round trip back to a */
    }

    /* mixed-width members: offsets must be computed correctly when
     * widths differ (char=1, int=2, pointer=2) */
    m.c = 'Z';
    m.n = 1000;
    m.s = "hi";
    putchar(m.c); newline();            /* Z */
    print_int(m.n); newline();          /* 1000 */
    puts(m.s); newline();               /* hi */

    puts("DONE.");
}
