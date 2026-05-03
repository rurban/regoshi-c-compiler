#include <stdio.h>

char buf[256];
char *pp;

static long sext_char(long a) { return a; }

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    pp = buf + 4;
    if (pp == buf
        // used to crash here
        || (pp[-1] == 0)) {
        printf("OK\n");
    }

    // GH#11: ND_CAST widening-to-64-bit path received non-integer types
    // (e.g. arrays) whose size was neither 1,2,4.  The old code used
    // reg(r, from->size) which returned reg64 for unrecognized sizes,
    // producing an invalid "movsx r64,r64" that failed to assemble.
    {
        // array-of-unsigned-char has size=7 but is_unsigned=false
        // (rcc array type does not inherit is_unsigned from base),
        // so the cast takes the sign_extend path with from->size=7.
        // Under the old code this emitted movsx reg64,reg64.
        unsigned char arr[7];
        char *p = (char *)arr;
        // cast of the scalar array (decayed to pointer) to long
        long l = (long)arr;
        if (l <= 0)
            return 1;
        (void)p;

        // longer array — also non-integer size
        short arr2[3]; // size=6
        unsigned long ul = (unsigned long)arr2;
        if (ul == 0)
            return 2;
    }

    // exercise sign/zero_extend_to with standard integer sizes
    {
        signed char sc = -100;
        if ((long)(signed char)sc != -100)
            return 11;

        signed short ss = -30000;
        if ((long)(signed short)ss != -30000)
            return 12;

        unsigned char uc = 200;
        if ((unsigned long)(unsigned char)uc != 200UL)
            return 13;

        unsigned short us = 60000;
        if ((unsigned long)(unsigned short)us != 60000UL)
            return 14;

        unsigned int ui = 4000000000U;
        if ((unsigned long)(unsigned int)ui != 4000000000UL)
            return 15;
    }

    return 0;
}
