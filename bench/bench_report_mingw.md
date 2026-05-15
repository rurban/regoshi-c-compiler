# Windows RCC Benchmark Results

_Generated: 05/15/2026 10:53:07_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         5084 |          606 |       5690 |
| RCC -O1 (optimized)   |         1240 |          610 |       1850 |
| TCC (Tiny C Compiler) |         1251 |          429 |       1680 |
| GCC -O0 (no opt)      |         1242 |          416 |       1658 |
| GCC -O2 (optimized)   |         1248 |          115 |       1363 |
| CLANG -O2 (optimized) |         2262 |          169 |       2431 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 4.06x
- Execute speed : RCC/TCC = 1.41x
- Total : RCC/TCC = 3.39x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
