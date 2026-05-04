// Category 1: Type sizes and alignment
#include <stdio.h>
int main(void) {
    printf("char:%lu short:%lu int:%lu long:%lu llong:%lu\n",
           sizeof(char), sizeof(short), sizeof(int), sizeof(long), sizeof(long long));
    printf("float:%lu double:%lu ldouble:%lu\n",
           sizeof(float), sizeof(double), sizeof(long double));
    printf("ptr:%lu bool:%lu\n", sizeof(void *), sizeof(_Bool));
    printf("align char:%lu int:%lu long:%lu ptr:%lu\n",
           _Alignof(char), _Alignof(int), _Alignof(long), _Alignof(void *));
    return 0;
}
