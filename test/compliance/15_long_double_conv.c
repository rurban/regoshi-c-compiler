// Long long ↔ long double conversions
#include <stdio.h>
int main(void) {
    long long n = 1000000000LL;
    long double ld = n;
    long long m = (long long)ld;
    printf("ll=%lld ld=%.0Lf back=%lld\n", n, ld, m);
    printf("match=%d\n", n == m);

    // Negative
    long long neg = -1000000000LL;
    long double ld2 = neg;
    long long neg2 = (long long)ld2;
    printf("neg=%lld back=%lld match=%d\n", neg, neg2, neg == neg2);

    return 0;
}
