# Linux RCC Benchmark Results

_Generated: May 2026_

| Compiler  | Compile (ms) | Execute (ms) | Total (ms) |
| :-------- | -----------: | -----------: | ---------: |
| RCC       |           70 |          336 |        406 |
| RCC -O1   |           90 |          327 |        417 |
| TCC       |            8 |          563 |        571 |
| SLIMCC    |           54 |          627 |        681 |
| KEFIR     |          203 |          697 |        900 |
| KEFIR -O1 |          205 |          510 |        715 |
| GCC -O0   |           85 |          670 |        755 |
| GCC -O2   |          426 |          361 |        787 |
| Clang -O0 |          137 |          702 |        839 |
| Clang -O2 |          174 |          275 |        449 |

## RCC Substep Timing

```
RCC:
  preprocess  bench.c:   1974 us
  lex         bench.c:   1125 us
  parse       bench.c:   1543 us
  typecheck   bench.c:     41 us
  codegen     bench.c:   2179 us
  peephole    bench.c:   2416 us
  link        bench_rcc:  32017 us

RCC -O1:
  preprocess  bench.c:   1154 us
  lex         bench.c:    977 us
  parse       bench.c:   1606 us
  typecheck   bench.c:     39 us
  opt(CTFE)   bench.c:   1075 us
  codegen     bench.c:   3008 us
  peephole    bench.c:   2822 us
  link        bench_rcc_o1:  50582 us
```
