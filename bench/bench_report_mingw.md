# Windows RCC Benchmark Results

_Generated: 05/02/2026 08:47:30_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         4147 |          643 |       4790 |
| RCC -O1 (optimized)   |         1246 |          628 |       1874 |
| TCC (Tiny C Compiler) |         1245 |          502 |       1747 |
| GCC -O0 (no opt)      |         1250 |          467 |       1717 |
| GCC -O2 (optimized)   |         1249 |          118 |       1367 |
| CLANG -O2 (optimized) |         1247 |          189 |       1436 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 3.33x
- Execute speed : RCC/TCC = 1.28x
- Total : RCC/TCC = 2.74x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
