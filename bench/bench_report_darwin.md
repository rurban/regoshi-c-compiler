# Darwin RCC Benchmark Results

_Generated: May 2026_

| Compiler  | Compile (ms) | Execute (ms) | Total (ms) |
| :-------- | -----------: | -----------: | ---------: |
| RCC       |           85 |          624 |        709 |
| RCC -O1   |          106 |          628 |        734 |
| TCC       |           62 |          612 |        674 |
| GCC -O0   |          396 |          486 |        882 |
| GCC -O2   |          127 |          295 |        422 |
| Clang -O0 |           89 |          489 |        578 |
| Clang -O2 |          100 |          296 |        396 |

## RCC Substep Timing

```
RCC:
  preprocess  bench.c:    541 us
  lex         bench.c:     68 us
  parse       bench.c:     58 us
  typecheck   bench.c:      5 us
  codegen     bench.c:   1771 us
  peephole    bench.c:    258 us
  link        bench_rcc:  90888 us

RCC -O1:
  preprocess  bench.c:    526 us
  lex         bench.c:     72 us
  parse       bench.c:     53 us
  typecheck   bench.c:      5 us
  opt(CTFE)   bench.c:     12 us
  codegen     bench.c:   1837 us
  peephole    bench.c:    270 us
  link        bench_rcc_o1:  79658 us
```
