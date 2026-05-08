# Windows RCC Benchmark Results

_Generated: 05/08/2026 08:14:09_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         7587 |          469 |       8056 |
| RCC -O1 (optimized)   |         1205 |          490 |       1695 |
| TCC (Tiny C Compiler) |         1203 |          384 |       1587 |
| GCC -O0 (no opt)      |         2196 |          363 |       2559 |
| GCC -O2 (optimized)   |         2202 |           92 |       2294 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 6.31x
- Execute speed : RCC/TCC = 1.22x
- Total : RCC/TCC = 5.08x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
