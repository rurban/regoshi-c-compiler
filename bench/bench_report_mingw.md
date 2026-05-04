# Windows RCC Benchmark Results

_Generated: 05/05/2026 11:31:36_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         6152 |          572 |       6724 |
| RCC -O1 (optimized)   |         1256 |          563 |       1819 |
| TCC (Tiny C Compiler) |         1254 |          431 |       1685 |
| GCC -O0 (no opt)      |         1252 |          420 |       1672 |
| GCC -O2 (optimized)   |         1261 |          116 |       1377 |
| CLANG -O2 (optimized) |         1268 |          169 |       1437 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 4.91x
- Execute speed : RCC/TCC = 1.33x
- Total : RCC/TCC = 3.99x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
