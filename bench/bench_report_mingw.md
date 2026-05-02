# Windows RCC Benchmark Results

_Generated: 05/02/2026 07:17:16_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         2667 |          648 |       3315 |
| RCC -O1 (optimized)   |         1212 |          649 |       1861 |
| TCC (Tiny C Compiler) |         1224 |          670 |       1894 |
| GCC -O0 (no opt)      |         1217 |          555 |       1772 |
| GCC -O2 (optimized)   |         1221 |          134 |       1355 |
| CLANG -O2 (optimized) |         1216 |          307 |       1523 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.18x
- Execute speed : RCC/TCC = 0.97x
- Total : RCC/TCC = 1.75x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
