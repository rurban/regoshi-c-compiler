// Struct argument passed after scalar args and vice versa
#include <stdio.h>

typedef struct {
    int a, b, c;
} trio;

int bar(int i, int j, int k, trio t) {
    printf("bar: i=%d j=%d k=%d t={%d,%d,%d}\n", i, j, k, t.a, t.b, t.c);
    return (t.a == 1 && t.b == 2 && t.c == 3 && i == 4 && j == 5 && k == 6) ? 0 : 1;
}

int foo(trio t, int i, int j, int k) {
    return bar(i, j, k, t);
}

int main(void) {
    trio t = {1, 2, 3};
    int r = foo(t, 4, 5, 6);
    printf("result=%d (expect 0)\n", r);
    return r;
}
