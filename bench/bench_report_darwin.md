# Darwin RCC Benchmark Results

_Generated: May 2026_

| Compiler  | Compile (ms) | Execute (ms) | Total (ms) |
| :-------- | -----------: | -----------: | ---------: |
| RCC       |           84 |          792 |        876 |
| RCC -O1   |          154 |          825 |        979 |
| TCC       |           98 |          676 |        774 |
| GCC -O0   |          154 |          620 |        774 |
| GCC -O2   |          268 |          360 |        628 |
| Clang -O0 |          114 |          563 |        677 |
| Clang -O2 |          171 |          360 |        531 |

## RCC Substep Timing

```
RCC:
  preprocess  bench.c:    492 us
  lex         bench.c:    135 us
  parse       bench.c:     58 us
  typecheck   bench.c:      4 us
  codegen     bench.c:   1982 us
  peephole    bench.c:    339 us
  link        bench_rcc:  77915 us

RCC -O1:
  preprocess  bench.c:    611 us
  lex         bench.c:    135 us
  parse       bench.c:     59 us
  typecheck   bench.c:      4 us
  opt(CTFE)   bench.c:     11 us
  codegen     bench.c:   2145 us
  peephole    bench.c:    513 us
  link        bench_rcc_o1:  96627 us
```
