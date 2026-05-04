// Local array initialization — various types and sizes
#include <stdio.h>
int main(void) {
    // 1D arrays
    int a[] = {10, 20, 30};
    printf("int: %d %d %d\n", a[0], a[1], a[2]);

    char b[] = {65, 66, 67};
    printf("char: %d %d %d\n", b[0], b[1], b[2]);

    unsigned char c[] = {200, 201, 202};
    printf("uchar: %d %d %d\n", c[0], c[1], c[2]);

    short d[] = {1000, 2000, 3000};
    printf("short: %d %d %d\n", d[0], d[1], d[2]);

    long e[] = {100000L, 200000L};
    printf("long: %ld %ld\n", e[0], e[1]);

    float f[] = {1.5f, 2.5f, 3.5f};
    printf("float: %.1f %.1f %.1f\n", (double)f[0], (double)f[1], (double)f[2]);

    double g[] = {1.1, 2.2, 3.3};
    printf("double: %.1f %.1f %.1f\n", g[0], g[1], g[2]);

    // 2D arrays
    int m[2][3] = {{1, 2, 3}, {4, 5, 6}};
    printf("2D: %d %d %d %d %d %d\n", m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2]);

    unsigned char out[][1] = {{71}, {71}, {71}};
    printf("2D uchar: %d %d %d\n", out[0][0], out[1][0], out[2][0]);

    // Array with expression in size
    int arr[3 + 1];
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = 30;
    arr[3] = 40;
    printf("expr_size: %d %d %d %d\n", arr[0], arr[1], arr[2], arr[3]);

    return 0;
}
