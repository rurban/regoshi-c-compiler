# Windows RCC Benchmark Results

_Generated: 05/07/2026 05:39:33_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         7358 |          649 |       8007 |
| RCC -O1 (optimized)   |         1293 |          629 |       1922 |
| TCC (Tiny C Compiler) |         1277 |          503 |       1780 |
| GCC -O0 (no opt)      |         5310 |          495 |       5805 |
| GCC -O2 (optimized)   |         2290 |          118 |       2408 |
| CLANG -O2 (optimized) |         2346 |          188 |       2534 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 5.76x
- Execute speed : RCC/TCC = 1.29x
- Total : RCC/TCC = 4.5x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
