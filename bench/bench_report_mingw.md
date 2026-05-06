# Windows RCC Benchmark Results

_Generated: 05/06/2026 19:44:44_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |        12039 |          548 |      12587 |
| RCC -O1 (optimized)   |         1262 |          543 |       1805 |
| TCC (Tiny C Compiler) |         1257 |          427 |       1684 |
| GCC -O0 (no opt)      |         3275 |          417 |       3692 |
| GCC -O2 (optimized)   |         3263 |          116 |       3379 |
| CLANG -O2 (optimized) |         2268 |          170 |       2438 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 9.58x
- Execute speed : RCC/TCC = 1.28x
- Total : RCC/TCC = 7.47x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
