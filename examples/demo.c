#include <stdio.h>
#include <string.h>

struct point { int x; int y; };

static int add(int a, int b)
{
    return a + b;
}

static int fact(int n)
{
    if (n <= 1)
        return 1;
    return n * fact(n - 1);
}

static struct point move(struct point p, int dx)
{
    p.x += dx;
    return p;
}

static void fill(int *t, int n)
{
    for (int i = 0; i < n; i++)
        t[i] = i * i;
}

int main(void)
{
    int s = 0;
    int tab[5];
    char name[16];
    struct point pt = { 1, 2 };
    double ratio = 0.5;

    s = add(s, 2);
    fill(tab, 5);
    strcpy(name, "Mercator");
    pt = move(pt, 10);

    switch (s) {
    case 2: ratio *= 3; break;
    default: ratio = 0;
    }

    int i = 0;
    while (i < 3) i++;

    printf("fact(3)=%d ratio=%g %s\n", fact(3), ratio, name);
    return 0;
}
