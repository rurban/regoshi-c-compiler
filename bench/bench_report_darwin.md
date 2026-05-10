# Darwin RCC Benchmark Results

_Generated: May 2026_

| Compiler  | Compile (ms) | Execute (ms) | Total (ms) |
| :-------- | -----------: | -----------: | ---------: |
| RCC       |           77 |          633 |        710 |
| RCC -O1   |           86 |          622 |        708 |
| TCC       |           33 |          555 |        588 |
| GCC -O0   |           79 |          470 |        549 |
| GCC -O2   |          104 |          291 |        395 |
| Clang -O0 |           81 |          474 |        555 |
| Clang -O2 |          112 |          284 |        396 |

## RCC Substep Timing

```
RCC:
  preprocess  bench.c:    457 us
  lex         bench.c:     67 us
  parse       bench.c:     49 us
  typecheck   bench.c:      4 us
  codegen     bench.c:   1722 us
  peephole    bench.c:    235 us
  link        bench_rcc:  61906 us

RCC -O1:
  preprocess  bench.c:    452 us
  lex         bench.c:     67 us
  parse       bench.c:     47 us
  typecheck   bench.c:      5 us
  opt(CTFE)   bench.c:     11 us
  codegen     bench.c:   1927 us
  peephole    bench.c:    230 us
  link        bench_rcc_o1:  69565 us
```
