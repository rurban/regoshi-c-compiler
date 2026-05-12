# Linux RCC Benchmark Results

_Generated: May 2026_

| Compiler  | Compile (ms) | Execute (ms) | Total (ms) |
| :-------- | -----------: | -----------: | ---------: |
| RCC       |           44 |          601 |        645 |
| RCC -O1   |           50 |          606 |        656 |
| TCC       |           13 |          557 |        570 |
| SLIMCC    |           51 |          616 |        667 |
| KEFIR     |          196 |          657 |        853 |
| KEFIR -O1 |          177 |          485 |        662 |
| GCC -O0   |           68 |          561 |        629 |
| GCC -O2   |          171 |          211 |        382 |
| Clang -O0 |          109 |          621 |        730 |
| Clang -O2 |          133 |          230 |        363 |

## RCC Substep Timing

```
RCC:
  preprocess  bench.c:    372 us
  lex         bench.c:    238 us
  parse       bench.c:    251 us
  typecheck   bench.c:     10 us
  codegen     bench.c:    944 us
  peephole    bench.c:    537 us
  link        bench_rcc:  39162 us

RCC -O1:
  preprocess  bench.c:    475 us
  lex         bench.c:    324 us
  parse       bench.c:    351 us
  typecheck   bench.c:     10 us
  opt(CTFE)   bench.c:     35 us
  codegen     bench.c:   1185 us
  peephole    bench.c:    774 us
  link        bench_rcc_o1:  37531 us
```
