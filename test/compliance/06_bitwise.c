// Category 6: Bitwise operations
#include <stdio.h>
int main(void) {
    // Shifts — signed vs unsigned
    int si = -1;
    printf("(int)-1 >> 4 = %d\n", si >> 4); // arithmetic shift
    unsigned int ui = 0xFFFFFFFF;
    printf("(uint)0xFFFFFFFF >> 4 = %u\n", ui >> 4); // logical shift
    printf("1 << 31 = %d\n", 1 << 31); // MSB set

    // AND/OR/XOR with promotions
    char c = 0xFF;
    printf("(char)0xFF & 0x0F = %d\n", c & 0x0F); // char promoted to int first
    unsigned char uc = 0xFF;
    printf("(uchar)0xFF & 0x0F = %d\n", uc & 0x0F);

    // Complement
    unsigned char uc2 = 0x0F;
    printf("~(uchar)0x0F as int = %d\n", ~uc2); // promoted to int, then complemented
    printf("~(uchar)0x0F & 0xFF = %d\n", ~uc2 & 0xFF);

    // Mixed width
    long l = 0xFF00FF00FF00FF00L;
    int mask = 0x0000FFFF;
    printf("long & int_mask = %lx\n", l & mask);

    // Shift amount larger than type
    printf("1u << 0 = %u\n", 1u << 0);
    printf("1u << 31 = %u\n", 1u << 31);

    return 0;
}
