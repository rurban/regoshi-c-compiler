# Windows RCC Benchmark Results

_Generated: 05/06/2026 11:41:49_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         4631 |          607 |       5238 |
| RCC -O1 (optimized)   |         1253 |          607 |       1860 |
| TCC (Tiny C Compiler) |         1243 |          498 |       1741 |
| GCC -O0 (no opt)      |         1255 |          468 |       1723 |
| GCC -O2 (optimized)   |         1243 |          118 |       1361 |
| CLANG -O2 (optimized) |         4058 |          188 |       4246 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 3.73x
- Execute speed : RCC/TCC = 1.22x
- Total : RCC/TCC = 3.01x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
