#ifdef __aarch64__
#include <stdio.h>

int main(void) {
    int x = 42, y = 0;
    __asm__("str %1, %0" : "=r"(y) : "r"(x));
    if (y != 42) return 1;

    printf("OK arm64 asm\n");
    return 0;
}
#else
int main(void) {
    printf("OK skipped arm64\n");
    return 0;
}
#endif
