// Category 5: Casting and truncation
#include <stdio.h>
int main(void) {
    // Widening signed
    char c = -50;
    printf("char→short: %d\n", (short)c);
    printf("char→int: %d\n", (int)c);
    printf("char→long: %ld\n", (long)c);
    short s = -1000;
    printf("short→int: %d\n", (int)s);
    printf("short→long: %ld\n", (long)s);
    int i = -100000;
    printf("int→long: %ld\n", (long)i);

    // Widening unsigned
    unsigned char uc = 200;
    printf("uchar→int: %d\n", (int)uc);
    printf("uchar→long: %ld\n", (long)uc);
    unsigned short us = 50000;
    printf("ushort→int: %d\n", (int)us);
    printf("ushort→long: %ld\n", (long)us);

    // Narrowing
    int big = 0x12345678;
    printf("int→char: %d\n", (char)big);
    printf("int→short: %d\n", (short)big);
    long lbig = 0x123456789ABCDEF0L;
    printf("long→int: %d\n", (int)lbig);

    // Float↔int
    printf("3.7→int: %d\n", (int)3.7);
    printf("−3.7→int: %d\n", (int)-3.7);
    printf("42→double: %.1f\n", (double)42);
    printf("−1→double: %.1f\n", (double)-1);

    // Unsigned→float
    unsigned int u = 3000000000u;
    printf("3e9→double: %.0f\n", (double)u);

    return 0;
}
