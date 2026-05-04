// Bitfield operations
#include <stdio.h>

struct BF1 {
    unsigned x : 3;
    unsigned y : 5;
    unsigned z : 8;
};
struct BF2 {
    signed int a : 12;
    signed int b : 20;
};
struct BF3 {
    unsigned x1 : 1;
    unsigned x2 : 2;
    unsigned x3 : 3;
};

int main(void) {
    // Basic read/write
    struct BF1 b1 = {0};
    b1.x = 5;
    b1.y = 17;
    b1.z = 200;
    printf("bf1: x=%d y=%d z=%d\n", b1.x, b1.y, b1.z);
    printf("bf1 size=%lu\n", sizeof(struct BF1));

    // Signed bitfield
    struct BF2 b2;
    b2.a = -123;
    b2.b = -456;
    printf("bf2: a=%d b=%d\n", b2.a, b2.b);

    // Compound assignment
    struct BF3 b3;
    b3.x1 = 1;
    b3.x2 = 2;
    b3.x3 = 3;
    b3.x3 += (b3.x2 - b3.x1) * b3.x2;
    printf("bf3: x1=%d x2=%d x3=%d\n", b3.x1, b3.x2, b3.x3);

    // Bitfield truncation on assign
    struct BF3 b4;
    b4.x3 = 100; // 100 doesn't fit in 3 bits, should truncate to 4
    printf("bf3 trunc: %d\n", b4.x3);

    // Bitfield in comparison
    int j = 1081;
    struct {
        signed int m : 11;
    } l;
    l.m = j;
    printf("bf assign: l.m=%d j=%d eq=%d\n", l.m, j, l.m == j);

    return 0;
}
