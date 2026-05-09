# Windows RCC Benchmark Results

_Generated: 05/09/2026 13:53:28_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         3151 |          557 |       3708 |
| RCC -O1 (optimized)   |         1249 |          557 |       1806 |
| TCC (Tiny C Compiler) |         1255 |          458 |       1713 |
| GCC -O0 (no opt)      |         1255 |          438 |       1693 |
| GCC -O2 (optimized)   |         1250 |          116 |       1366 |
| CLANG -O2 (optimized) |         1251 |          170 |       1421 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.51x
- Execute speed : RCC/TCC = 1.22x
- Total : RCC/TCC = 2.16x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
