# Linux RCC Benchmark Results

_Generated: May 2026_

| Compiler  | Compile (ms) | Execute (ms) | Total (ms) |
| :-------- | -----------: | -----------: | ---------: |
| RCC       |           38 |          599 |        637 |
| RCC -O1   |           38 |          589 |        627 |
| TCC       |            8 |          561 |        569 |
| SLIMCC    |           50 |          633 |        683 |
| KEFIR     |          216 |          663 |        879 |
| KEFIR -O1 |          213 |          491 |        704 |
| GCC -O0   |           80 |          564 |        644 |
| GCC -O2   |          180 |          214 |        394 |
| Clang -O0 |          108 |          629 |        737 |
| Clang -O2 |          140 |          235 |        375 |

## RCC Substep Timing

```
RCC:
  preprocess  bench.c:    483 us
  lex         bench.c:    297 us
  parse       bench.c:    391 us
  typecheck   bench.c:     36 us
  codegen     bench.c:    665 us
  peephole    bench.c:    294 us
  link        bench_rcc:  31146 us

RCC -O1:
  preprocess  bench.c:    333 us
  lex         bench.c:    174 us
  parse       bench.c:    228 us
  typecheck   bench.c:      8 us
  opt(CTFE)   bench.c:     41 us
  codegen     bench.c:    358 us
  peephole    bench.c:    213 us
  link        bench_rcc_o1:  30882 us
```
