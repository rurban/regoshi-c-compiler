# Windows RCC Benchmark Results

_Generated: 05/04/2026 12:39:25_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         2970 |          628 |       3598 |
| RCC -O1 (optimized)   |         1294 |          633 |       1927 |
| TCC (Tiny C Compiler) |         1286 |          502 |       1788 |
| GCC -O0 (no opt)      |         1276 |          479 |       1755 |
| GCC -O2 (optimized)   |         1292 |          119 |       1411 |
| CLANG -O2 (optimized) |         1275 |          195 |       1470 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.31x
- Execute speed : RCC/TCC = 1.25x
- Total : RCC/TCC = 2.01x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
