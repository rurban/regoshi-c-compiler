// Category 2: Integer promotions and arithmetic conversions
#include <stdio.h>
int main(void) {
    // §6.3.1.1: char/short promoted to int in expressions
    char c = -1;
    unsigned char uc = 255;
    short s = -1;
    unsigned short us = 65535;

    printf("char -1 as int: %d\n", c + 0);
    printf("uchar 255 as int: %d\n", uc + 0);
    printf("short -1 as int: %d\n", s + 0);
    printf("ushort 65535 as int: %d\n", us + 0);

    // §6.3.1.8: usual arithmetic conversions
    // signed + unsigned same rank → unsigned
    int si = -1;
    unsigned int ui = 1;
    printf("(int)-1 + (uint)1 = %u\n", si + ui); // wraps unsigned
    printf("(int)-1 < (uint)1 = %d\n", si < ui); // comparison as unsigned

    // wider type wins
    long l = -1;
    int i = 1;
    printf("(long)-1 + (int)1 = %ld\n", l + i);

    // sign extension on widening
    char c2 = -128;
    printf("(char)-128 as long: %ld\n", (long)c2);
    unsigned char uc2 = 200;
    printf("(uchar)200 as long: %ld\n", (long)uc2);
    short s2 = -32768;
    printf("(short)-32768 as long: %ld\n", (long)s2);

    return 0;
}
