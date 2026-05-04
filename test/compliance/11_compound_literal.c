// Compound literals and complex initializers
#include <stdio.h>

struct S {
    int a;
    int b;
    int c;
};

void swap(struct S *s) {
    *s = (struct S){s->b, s->a, s->c};
}

struct S make(int x, int y, int z) {
    return (struct S){x, y, z};
}

int main(void) {
    // Compound literal assignment
    struct S s1;
    s1 = (struct S){10, 20, 30};
    printf("assign: %d %d %d\n", s1.a, s1.b, s1.c);

    // Compound literal in init
    struct S s2 = (struct S){40, 50, 60};
    printf("init: %d %d %d\n", s2.a, s2.b, s2.c);

    // Self-referencing swap
    struct S s3 = {1, 2, 3};
    swap(&s3);
    printf("swap: %d %d %d\n", s3.a, s3.b, s3.c);

    // Function returning compound literal
    struct S s4 = make(7, 8, 9);
    printf("ret: %d %d %d\n", s4.a, s4.b, s4.c);

    // Nested struct init
    struct T {
        struct S inner;
        int d;
    };
    struct T t = {{100, 200, 300}, 400};
    printf("nested: %d %d %d %d\n", t.inner.a, t.inner.b, t.inner.c, t.d);

    // Array of structs
    struct S arr[2] = {{1, 2, 3}, {4, 5, 6}};
    printf("arr: %d %d\n", arr[0].a, arr[1].a);

    return 0;
}
