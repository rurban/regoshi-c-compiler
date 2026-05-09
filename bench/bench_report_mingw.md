# Windows RCC Benchmark Results

_Generated: 05/09/2026 07:41:44_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         2943 |          543 |       3486 |
| RCC -O1 (optimized)   |         1258 |          543 |       1801 |
| TCC (Tiny C Compiler) |         1248 |          430 |       1678 |
| GCC -O0 (no opt)      |         1253 |          417 |       1670 |
| GCC -O2 (optimized)   |         1244 |          115 |       1359 |
| CLANG -O2 (optimized) |         1255 |          169 |       1424 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.36x
- Execute speed : RCC/TCC = 1.26x
- Total : RCC/TCC = 2.08x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
