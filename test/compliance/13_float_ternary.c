// Float ternary and comparison edge cases
#include <stdio.h>
int main(void) {
    int a = 1, b = -1;
    double f = 1.0, g = 0.0;
    double e = (a < b) ? f : g;
    printf("e=%f (expect 0.0)\n", e);
    printf("e nonzero=%d (expect 0)\n", e != 0.0);

    // Float comparison results used in expressions
    double x = 3.14, y = 3.14;
    printf("eq=%d ne=%d lt=%d\n", x == y, x != y, x < y);

    float fa = 1.0f, fb = 2.0f;
    printf("feq=%d flt=%d\n", fa == fb, fa < fb);

    return 0;
}
