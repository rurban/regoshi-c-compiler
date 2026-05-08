# Windows RCC Benchmark Results

_Generated: 05/08/2026 05:00:06_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         8843 |          555 |       9398 |
| RCC -O1 (optimized)   |         1291 |          567 |       1858 |
| TCC (Tiny C Compiler) |         1283 |          473 |       1756 |
| GCC -O0 (no opt)      |         1302 |          463 |       1765 |
| GCC -O2 (optimized)   |         1293 |          117 |       1410 |
| CLANG -O2 (optimized) |         1285 |          173 |       1458 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 6.89x
- Execute speed : RCC/TCC = 1.17x
- Total : RCC/TCC = 5.35x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
