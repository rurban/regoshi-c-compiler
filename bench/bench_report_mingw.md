# Windows RCC Benchmark Results

_Generated: 05/10/2026 06:49:44_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         3038 |          563 |       3601 |
| RCC -O1 (optimized)   |         1277 |          557 |       1834 |
| TCC (Tiny C Compiler) |         1283 |          436 |       1719 |
| GCC -O0 (no opt)      |         1288 |          418 |       1706 |
| GCC -O2 (optimized)   |         1248 |          116 |       1364 |
| CLANG -O2 (optimized) |         1310 |          198 |       1508 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.37x
- Execute speed : RCC/TCC = 1.29x
- Total : RCC/TCC = 2.09x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
