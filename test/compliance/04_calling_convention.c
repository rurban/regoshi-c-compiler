// Category 4: ARM64 calling convention
#include <stdio.h>

// Test: 8 GP register args
int sum8(int a, int b, int c, int d, int e, int f, int g, int h) {
    return a + b + c + d + e + f + g + h;
}

// Test: overflow to stack (9th arg)
int sum9(int a, int b, int c, int d, int e, int f, int g, int h, int i) {
    return a + b + c + d + e + f + g + h + i;
}

// Test: mixed int and float
double mixed(int a, double b, int c, double d) {
    return a + b + c + d;
}

// Test: struct pass by value (small)
struct S8 {
    int x;
    int y;
};
int struct_val(struct S8 s) { return s.x + s.y; }

// Test: struct pass by value (16 bytes)
struct S16 {
    long a;
    long b;
};
long struct16(struct S16 s) { return s.a + s.b; }

// Test: struct return
struct S8 make_s8(int x, int y) {
    struct S8 s = {x, y};
    return s;
}
struct S16 make_s16(long a, long b) {
    struct S16 s = {a, b};
    return s;
}

// Test: variadic
#include <stdarg.h>
int va_sum(int count, ...) {
    va_list ap;
    va_start(ap, count);
    int total = 0;
    for (int i = 0; i < count; i++)
        total += va_arg(ap, int);
    va_end(ap);
    return total;
}

int main(void) {
    printf("sum8: %d\n", sum8(1, 2, 3, 4, 5, 6, 7, 8));
    printf("sum9: %d\n", sum9(1, 2, 3, 4, 5, 6, 7, 8, 9));
    printf("mixed: %.1f\n", mixed(1, 2.5, 3, 4.5));
    struct S8 s = {10, 20};
    printf("struct8: %d\n", struct_val(s));
    struct S16 s16 = {100, 200};
    printf("struct16: %ld\n", struct16(s16));
    struct S8 r8 = make_s8(42, 58);
    printf("ret8: %d %d\n", r8.x, r8.y);
    struct S16 r16 = make_s16(1000, 2000);
    printf("ret16: %ld %ld\n", r16.a, r16.b);
    printf("va_sum: %d\n", va_sum(4, 10, 20, 30, 40));
    return 0;
}
