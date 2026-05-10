# Windows RCC Benchmark Results

_Generated: 05/10/2026 08:20:55_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         2738 |          541 |       3279 |
| RCC -O1 (optimized)   |         1250 |          541 |       1791 |
| TCC (Tiny C Compiler) |         1241 |          427 |       1668 |
| GCC -O0 (no opt)      |         1246 |          417 |       1663 |
| GCC -O2 (optimized)   |         1251 |          116 |       1367 |
| CLANG -O2 (optimized) |         1257 |          169 |       1426 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.21x
- Execute speed : RCC/TCC = 1.27x
- Total : RCC/TCC = 1.97x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
