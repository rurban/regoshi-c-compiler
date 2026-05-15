# Windows RCC Benchmark Results

_Generated: 05/15/2026 13:24:51_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         7890 |          599 |       8489 |
| RCC -O1 (optimized)   |         1245 |          609 |       1854 |
| TCC (Tiny C Compiler) |         1248 |          429 |       1677 |
| GCC -O0 (no opt)      |         3251 |          418 |       3669 |
| GCC -O2 (optimized)   |         3273 |          116 |       3389 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 6.32x
- Execute speed : RCC/TCC = 1.4x
- Total : RCC/TCC = 5.06x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
