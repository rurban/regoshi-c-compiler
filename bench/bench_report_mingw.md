# Windows RCC Benchmark Results

_Generated: 05/03/2026 12:43:09_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         2827 |          552 |       3379 |
| RCC -O1 (optimized)   |         1252 |          549 |       1801 |
| TCC (Tiny C Compiler) |         1258 |          429 |       1687 |
| GCC -O0 (no opt)      |         1257 |          417 |       1674 |
| GCC -O2 (optimized)   |         1255 |          116 |       1371 |
| CLANG -O2 (optimized) |         1261 |          169 |       1430 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.25x
- Execute speed : RCC/TCC = 1.29x
- Total : RCC/TCC = 2x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
