// Test TR39 Unicode identifier security via libu8ident.
// TR39 rejects what C11-C26 innocently allows:
//   - Mixed scripts (e.g. Cyrillic + Latin)   -> ERR_SCRIPTS  warning
//   - Limited use scripts (Cherokee, Adlam)    -> ERR_SCRIPT   hard error
//   - Greek letters confusable with Latin      -> ERR_SCRIPTS  warning
//   - Combining mark violations (>4 marks)     -> ERR_COMBINE  warning
//   - Non-spacing mark making base confusable  -> ERR_COMBINE  warning
//
// #pragma unicode ScriptName  allows a script for the rest of the file/scope.
// #pragma unicode reset       resets the script context to default.
//
// Known homoglyphs C11-C26 would allow:
//   Cyrillic 'а' (U+0430) looks like Latin 'a' (U+0061)
//   Cyrillic 'о' (U+043E) looks like Latin 'o' (U+006F)
//   Greek   'ο' (U+03BF) looks like Latin 'o' (U+006F)

#include <stdio.h>
#include <assert.h>

// Test 1: With #pragma unicode Cyrillic, Cyrillic identifiers are allowed.
#pragma unicode Cyrillic
double привет = 0.1;
#pragma unicode reset

// Test 2: Without pragma, Cyrillic+Lefèvre(Latin) triggers mixed script warning.
// But Lefèvre alone with Latin is fine.
int Lefèvre = 2;

// Test 3: Greek+Lefèvre would trigger mixed script warning (Cyrillic disallowed in TR39_4).
// Use Greek with pragma.
#pragma unicode Greek
double λόγος = 3.14;
#pragma unicode reset

// Test 4: CJK with Han+Hiragana+Katakana — these are allowed combinations.
// Japanese: Hiragana + Katakana + Han
#pragma unicode Han
#pragma unicode Hiragana
#pragma unicode Katakana
int 漢字 = 42;
#pragma unicode reset

// Test 5: Arabic with Latin — allowed in TR39_4 (any Recommended + Latin)
#pragma unicode Arabic
int كتاب = 99;
#pragma unicode reset

int main() {
#pragma unicode Cyrillic
    printf("привет=%g\n", привет);
    assert(привет == 0.1);
#pragma unicode reset

    printf("Lefèvre=%d\n", Lefèvre);
    assert(Lefèvre == 2);

#pragma unicode Greek
    printf("λόγος=%g\n", λόγος);
    assert(λόγος == 3.14);
#pragma unicode reset

#pragma unicode Han
#pragma unicode Hiragana
#pragma unicode Katakana
    printf("漢字=%d\n", 漢字);
    assert(漢字 == 42);
#pragma unicode reset

#pragma unicode Arabic
    printf("كتاب=%d\n", كتاب);
    assert(كتاب == 99);
#pragma unicode reset

    return 0;
}
