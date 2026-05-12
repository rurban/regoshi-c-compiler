// Test TR39 Unicode identifier security.
// TR39 rejects scripts that C11-C26 would allow:
//   - Limited use scripts (Cherokee, Adlam, etc.)
//   - Cyrillic + Latin mixed scripts (confusable)
//   - Greek letters confusable with Latin
//
// #pragma unicode Cyrillic allows Cyrillic in the current file.
//
// Known homoglyphs C11-C26 would allow:
//   Cyrillic 'а' (U+0430) looks like Latin 'a' (U+0061)
//   Cyrillic 'о' (U+043E) looks like Latin 'o' (U+006F)
//   Greek   'ο' (U+03BF) looks like Latin 'o' (U+006F)

#pragma unicode Cyrillic

#include <stdio.h>

double привет = 0.1;
int Lefèvre = 2;

int main() {
    printf("привет=%g\n", привет);
    printf("Lefèvre=%d\n", Lefèvre);
    return 0;
}
