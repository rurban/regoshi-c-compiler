# Windows RCC Benchmark Results

_Generated: 05/01/2026 14:54:21_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         3215 |          627 |       3842 |
| RCC -O1 (optimized)   |         1244 |          616 |       1860 |
| TCC (Tiny C Compiler) |         1245 |          498 |       1743 |
| GCC -O0 (no opt)      |         1243 |          468 |       1711 |
| GCC -O2 (optimized)   |         1246 |          118 |       1364 |
| CLANG -O2 (optimized) |         1245 |          188 |       1433 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.58x
- Execute speed : RCC/TCC = 1.26x
- Total : RCC/TCC = 2.2x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
