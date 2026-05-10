# Windows RCC Benchmark Results

_Generated: 05/11/2026 05:22:23_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         4234 |          546 |       4780 |
| RCC -O1 (optimized)   |         1261 |          549 |       1810 |
| TCC (Tiny C Compiler) |         1273 |          428 |       1701 |
| GCC -O0 (no opt)      |         1274 |          417 |       1691 |
| GCC -O2 (optimized)   |         1259 |          122 |       1381 |
| CLANG -O2 (optimized) |         1278 |          170 |       1448 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 3.33x
- Execute speed : RCC/TCC = 1.28x
- Total : RCC/TCC = 2.81x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
