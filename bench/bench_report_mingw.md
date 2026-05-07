# Windows RCC Benchmark Results

_Generated: 05/07/2026 13:01:22_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         2858 |          549 |       3407 |
| RCC -O1 (optimized)   |         1273 |          553 |       1826 |
| TCC (Tiny C Compiler) |         1262 |          441 |       1703 |
| GCC -O0 (no opt)      |         1285 |          421 |       1706 |
| GCC -O2 (optimized)   |         1287 |          124 |       1411 |
| CLANG -O2 (optimized) |         1257 |          169 |       1426 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.26x
- Execute speed : RCC/TCC = 1.24x
- Total : RCC/TCC = 2x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
