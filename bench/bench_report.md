# Linux RCC Benchmark Results

_Generated: Mai 2026_

| Compiler  | Compile (ms) | Execute (ms) | Total (ms) |
| :-------- | -----------: | -----------: | ---------: |
| RCC       |           58 |          764 |        822 |
| RCC -O1   |           51 |          778 |        829 |
| TCC       |            6 |          641 |        647 |
| SLIMCC    |           48 |          631 |        679 |
| KEFIR     |          243 |          753 |        996 |
| KEFIR -O1 |          255 |          408 |        663 |
| GCC -O0   |           75 |          627 |        702 |
| GCC -O2   |          174 |          226 |        400 |
| Clang -O0 |          115 |          588 |        703 |
| Clang -O2 |          173 |          235 |        408 |

## RCC Substep Timing

```
RCC:
  preprocess  bench.c:    427 us
  lex         bench.c:    246 us
  parse       bench.c:    257 us
  typecheck   bench.c:      8 us
  codegen     bench.c:   1337 us
  peephole    bench.c:    919 us
  link        bench_rcc:  45952 us

RCC -O1:
  preprocess  bench.c:    588 us
  lex         bench.c:    313 us
  parse       bench.c:    340 us
  typecheck   bench.c:      8 us
  opt(CTFE)   bench.c:     45 us
  codegen     bench.c:   1120 us
  peephole    bench.c:    472 us
  link        bench_rcc_o1:  58118 us
```
