# Darwin RCC Benchmark Results

_Generated: May 2026_

| Compiler  | Compile (ms) | Execute (ms) | Total (ms) |
| :-------- | -----------: | -----------: | ---------: |
| RCC       |          163 |          809 |        972 |
| RCC -O1   |          146 |          771 |        917 |
| TCC       |           94 |          740 |        834 |
| GCC -O0   |          192 |          593 |        785 |
| GCC -O2   |          221 |          380 |        601 |
| Clang -O0 |          166 |          669 |        835 |
| Clang -O2 |          262 |          410 |        672 |

## RCC Substep Timing

```
RCC:
  preprocess  bench.c:   1661 us
  lex         bench.c:    163 us
  parse       bench.c:    135 us
  typecheck   bench.c:     11 us
  codegen     bench.c:   3958 us
  peephole    bench.c:    248 us
  link        bench_rcc: 149927 us

RCC -O1:
  preprocess  bench.c:    732 us
  lex         bench.c:     71 us
  parse       bench.c:     67 us
  typecheck   bench.c:      5 us
  opt(CTFE)   bench.c:     11 us
  codegen     bench.c:   7391 us
  peephole    bench.c:    842 us
  link        bench_rcc_o1: 141632 us
```
