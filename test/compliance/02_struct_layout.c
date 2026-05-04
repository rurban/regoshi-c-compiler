// Category 3: Struct layout — padding and alignment
#include <stdio.h>
#include <stddef.h>
struct S1 {
    char a;
    int b;
};
struct S2 {
    int a;
    char b;
};
struct S3 {
    char a;
    short b;
    int c;
    long d;
};
struct S4 {
    char a;
    char b;
    char c;
    int d;
};
struct S5 {
    long a;
    char b;
}; // trailing padding
struct S6 {
    char a;
    long b;
    char c;
};
struct Nested {
    struct S1 inner;
    int x;
};
union U1 {
    int a;
    long b;
    char c;
};

int main(void) {
    printf("S1: size=%lu align=%lu b_off=%lu\n", sizeof(struct S1), _Alignof(struct S1), offsetof(struct S1, b));
    printf("S2: size=%lu align=%lu b_off=%lu\n", sizeof(struct S2), _Alignof(struct S2), offsetof(struct S2, b));
    printf("S3: size=%lu b=%lu c=%lu d=%lu\n", sizeof(struct S3), offsetof(struct S3, b), offsetof(struct S3, c), offsetof(struct S3, d));
    printf("S4: size=%lu d_off=%lu\n", sizeof(struct S4), offsetof(struct S4, d));
    printf("S5: size=%lu\n", sizeof(struct S5));
    printf("S6: size=%lu b=%lu c=%lu\n", sizeof(struct S6), offsetof(struct S6, b), offsetof(struct S6, c));
    printf("Nested: size=%lu x_off=%lu\n", sizeof(struct Nested), offsetof(struct Nested, x));
    printf("U1: size=%lu align=%lu\n", sizeof(union U1), _Alignof(union U1));
    return 0;
}
