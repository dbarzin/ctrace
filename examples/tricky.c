#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define SQUARE(x) ((x) * (x))

enum color { RED, GREEN, BLUE };
union num { int i; float f; };
struct node { int val; struct node *next; };

int counter = 0;

static void set(int *p, int v) { *p = v; }

static void maybe(struct node *n)
{
    if (!n) return;
    if (n && n->val > 0) n->val = -n->val;
}

static const char *label(enum color c)
{
    if (c == RED) return "red";
    else if (c == GREEN) return "green";
    else return "blue";
}

int main(void)
{
    struct node a = { 5, NULL };
    struct node *p = &a;
    int x = 0, y = SQUARE(3);
    bool ok = true;
    enum color c = BLUE;
    union num u;
    float fs[3] = { 1.5f, 2.5f };

    u.i = 7;
    set(&x, 42);
    maybe(p);
    maybe(NULL);
    assert(x == 42);
    do { counter++; } while (counter < 2);
    if (x > 10) ok = false; else ok = true;
    p->val += y;
    printf("%s %d %d %d\n", label(c), x, a.val, counter);
    if (!ok) goto out;
    x = -1;
out:
    fs[2] = fs[0] + fs[1];
    return x > 0 ? 0 : 1;
}
