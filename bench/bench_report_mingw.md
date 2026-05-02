# Windows RCC Benchmark Results

_Generated: 05/02/2026 19:37:33_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         9285 |          562 |       9847 |
| RCC -O1 (optimized)   |         1245 |          548 |       1793 |
| TCC (Tiny C Compiler) |         1249 |          427 |       1676 |
| GCC -O0 (no opt)      |         1256 |          420 |       1676 |
| GCC -O2 (optimized)   |         1247 |          116 |       1363 |
| CLANG -O2 (optimized) |         1252 |          169 |       1421 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 7.43x
- Execute speed : RCC/TCC = 1.32x
- Total : RCC/TCC = 5.88x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
