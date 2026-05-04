// Category 7: Global data initialization
#include <stdio.h>

// Scalar globals
int g_int = 42;
long g_long = 0x123456789ABCL;
char g_char = 'A';
double g_double = 3.14159265358979;

// Struct with mixed types
struct Config {
    int a;
    unsigned char b1, b2, b3, b4, b5, b6, b7;
    int c;
    long d;
};
struct Config g_cfg = {1, 10, 20, 30, 40, 50, 60, 70, 0x7ffffffe, -1L};

// Array with constant expression
int g_arr[] = {1, 2, 3, 4, 5};
int g_arr_expr[] = {3 + 4, 10 * 2, 100 / 3};

// Function pointer in global struct
static int my_func(int x) { return x * 2; }
struct FP {
    int (*fn)(int);
    int val;
};
struct FP g_fp = {my_func, 99};

// Pointer to array element
int g_base[5] = {10, 20, 30, 40, 50};
int *g_ptr = &g_base[2];

// Nested struct
struct Inner {
    int x;
    int y;
};
struct Outer {
    struct Inner a;
    int b;
};
struct Outer g_nested = {{100, 200}, 300};

// String literal
char *g_str = "hello";

int main(void) {
    printf("int=%d long=%lx char=%c double=%.4f\n", g_int, g_long, g_char, g_double);
    printf("cfg: a=%d b1=%d b7=%d c=%d d=%ld\n", g_cfg.a, g_cfg.b1, g_cfg.b7, g_cfg.c, g_cfg.d);
    printf("arr: %d %d %d %d %d\n", g_arr[0], g_arr[1], g_arr[2], g_arr[3], g_arr[4]);
    printf("expr: %d %d %d\n", g_arr_expr[0], g_arr_expr[1], g_arr_expr[2]);
    printf("fp: %d %d\n", g_fp.fn(21), g_fp.val);
    printf("ptr: %d\n", *g_ptr);
    printf("nested: %d %d %d\n", g_nested.a.x, g_nested.a.y, g_nested.b);
    printf("str: %s\n", g_str);
    return 0;
}
