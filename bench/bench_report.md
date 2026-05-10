# Linux RCC Benchmark Results

_Generated: May 2026_

| Compiler     | Compile (ms) | Execute (ms) | Total (ms) |
| :----------- | -----------: | -----------: | ---------: |
| RCC          |           72 |          668 |        740 |
| RCC -O1      |           69 |          704 |        773 |
| TCC          |           10 |          637 |        647 |
| SLIMCC       |           62 |          733 |        795 |
| KEFIR        |          229 |          768 |        997 |
| KEFIR -O1    |          237 |          552 |        789 |
| GCC -O0      |           96 |          618 |        714 |
| GCC -O2      |          220 |          231 |        451 |
| Clang -O0    |          122 |          739 |        861 |
| Clang -O2    |          171 |          245 |        416 |

## RCC Substep Timing

```
RCC:
  preprocess  bench.c:    327 us
  lex         bench.c:    147 us
  parse       bench.c:    282 us
  typecheck   bench.c:     15 us
  codegen     bench.c:    973 us
  peephole    bench.c:    915 us
  link        bench_rcc:  65962 us

RCC -O1:
  preprocess  bench.c:    579 us
  lex         bench.c:    226 us
  parse       bench.c:    397 us
  typecheck   bench.c:     11 us
  opt(CTFE)   bench.c:     41 us
  codegen     bench.c:   1128 us
  peephole    bench.c:   1237 us
  link        bench_rcc_o1:  69047 us
```
