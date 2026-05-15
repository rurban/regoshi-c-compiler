# Linux RCC Benchmark Results

_Generated: May 2026_

| Compiler  | Compile (ms) | Execute (ms) | Total (ms) |
| :-------- | -----------: | -----------: | ---------: |
| RCC       |           34 |          328 |        362 |
| RCC -O1   |           32 |          329 |        361 |
| TCC       |           12 |          595 |        607 |
| SLIMCC    |           48 |          651 |        699 |
| KEFIR     |          199 |          703 |        902 |
| KEFIR -O1 |          207 |          500 |        707 |
| GCC -O0   |           77 |          582 |        659 |
| GCC -O2   |          189 |          228 |        417 |
| Clang -O0 |          116 |          647 |        763 |
| Clang -O2 |          134 |          237 |        371 |

## RCC Substep Timing

```
RCC:
  preprocess  bench.c:    339 us
  lex         bench.c:    170 us
  parse       bench.c:    257 us
  typecheck   bench.c:      9 us
  codegen     bench.c:    303 us
  peephole    bench.c:    166 us
  link        bench_rcc:  25401 us

RCC -O1:
  preprocess  bench.c:    331 us
  lex         bench.c:    176 us
  parse       bench.c:    271 us
  typecheck   bench.c:     11 us
  opt(CTFE)   bench.c:     25 us
  codegen     bench.c:    300 us
  peephole    bench.c:    182 us
  link        bench_rcc_o1:  31521 us
```
