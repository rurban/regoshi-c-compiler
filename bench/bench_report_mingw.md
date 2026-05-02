# Windows RCC Benchmark Results

_Generated: 05/02/2026 11:39:25_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         2950 |          562 |       3512 |
| RCC -O1 (optimized)   |         1282 |          556 |       1838 |
| TCC (Tiny C Compiler) |         1288 |          437 |       1725 |
| GCC -O0 (no opt)      |         1274 |          437 |       1711 |
| GCC -O2 (optimized)   |         1277 |          126 |       1403 |
| CLANG -O2 (optimized) |         1305 |          170 |       1475 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.29x
- Execute speed : RCC/TCC = 1.29x
- Total : RCC/TCC = 2.04x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
