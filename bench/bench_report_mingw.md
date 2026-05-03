# Windows RCC Benchmark Results

_Generated: 05/03/2026 07:56:00_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         8008 |          565 |       8573 |
| RCC -O1 (optimized)   |         1292 |          575 |       1867 |
| TCC (Tiny C Compiler) |         1287 |          428 |       1715 |
| GCC -O0 (no opt)      |         2261 |          419 |       2680 |
| GCC -O2 (optimized)   |         3276 |          122 |       3398 |
| CLANG -O2 (optimized) |         1252 |          169 |       1421 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 6.22x
- Execute speed : RCC/TCC = 1.32x
- Total : RCC/TCC = 5x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
