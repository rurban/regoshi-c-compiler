// Complex struct layouts matching SQLite patterns
#include <stdio.h>
#include <stddef.h>

typedef unsigned char u8;
typedef unsigned int u32;
typedef long long i64;

// Large struct with many member types (like sqlite3)
struct Big {
    void *p1; // 0
    void *p2; // 8
    void *p3; // 16
    void *p4; // 24
    void *p5; // 32
    int n1; // 40
    u32 flags; // 44
    unsigned long long f2; // 48
    i64 i1; // 56
    i64 i2; // 64
    u32 u1; // 72
    unsigned int u2; // 76
    int e1; // 80
    int e2; // 84
    int e3; // 88
    int e4; // 92
    u32 opts; // 96
    u8 x1; // 100
    u8 x2; // 101
    u8 x3; // 102
    u8 x4; // 103
    u8 x5; // 104
    u8 x6; // 105
    u8 x7; // 106
    u8 x8; // 107
    u8 x9; // 108
    u8 x10; // 109
    u8 x11; // 110
    u8 x12; // 111
    u8 x13; // 112
    u8 x14; // 113
    int next; // 116 (after 2 bytes padding)
    i64 change; // 120
    i64 total; // 128
    int limits[12]; // 136
};

int main(void) {
    printf("sizeof=%lu\n", sizeof(struct Big));
    printf("n1=%lu flags=%lu\n", offsetof(struct Big, n1), offsetof(struct Big, flags));
    printf("f2=%lu i1=%lu\n", offsetof(struct Big, f2), offsetof(struct Big, i1));
    printf("x1=%lu x14=%lu\n", offsetof(struct Big, x1), offsetof(struct Big, x14));
    printf("next=%lu change=%lu total=%lu\n", offsetof(struct Big, next), offsetof(struct Big, change), offsetof(struct Big, total));
    printf("limits=%lu\n", offsetof(struct Big, limits));

    // Init and read back
    struct Big b = {0};
    b.limits[0] = 0x7ffffffe;
    b.limits[11] = 42;
    b.x1 = 1;
    b.x14 = 14;
    b.next = 999;
    printf("limits[0]=%d limits[11]=%d\n", b.limits[0], b.limits[11]);
    printf("x1=%d x14=%d next=%d\n", b.x1, b.x14, b.next);
    return 0;
}
