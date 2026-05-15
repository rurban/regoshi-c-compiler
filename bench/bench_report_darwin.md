# Darwin RCC Benchmark Results

_Generated: May 2026_

| Compiler  | Compile (ms) | Execute (ms) | Total (ms) |
| :-------- | -----------: | -----------: | ---------: |
| RCC       |          128 |          748 |        876 |
| RCC -O1   |          119 |          756 |        875 |
| TCC       |           53 |          615 |        668 |
| GCC -O0   |          231 |          502 |        733 |
| GCC -O2   |          145 |          345 |        490 |
| Clang -O0 |           85 |          591 |        676 |
| Clang -O2 |          123 |          321 |        444 |

## RCC Substep Timing

```
RCC:
  preprocess  bench.c:    655 us
  lex         bench.c:    417 us
  parse       bench.c:     94 us
  typecheck   bench.c:      5 us
  codegen     bench.c:   3478 us
  peephole    bench.c:    327 us
  link        bench_rcc:  75614 us

RCC -O1:
  preprocess  bench.c:    729 us
  lex         bench.c:    145 us
  parse       bench.c:     57 us
  typecheck   bench.c:      4 us
  opt(CTFE)   bench.c:     11 us
  codegen     bench.c:   1998 us
  peephole    bench.c:    238 us
  link        bench_rcc_o1: 103565 us
```
