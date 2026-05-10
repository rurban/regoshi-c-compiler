# Windows RCC Benchmark Results

_Generated: 05/10/2026 20:40:23_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         2961 |          642 |       3603 |
| RCC -O1 (optimized)   |         1255 |          639 |       1894 |
| TCC (Tiny C Compiler) |         1283 |          676 |       1959 |
| GCC -O0 (no opt)      |         1264 |          561 |       1825 |
| GCC -O2 (optimized)   |         1299 |          136 |       1435 |
| CLANG -O2 (optimized) |         1269 |          309 |       1578 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.31x
- Execute speed : RCC/TCC = 0.95x
- Total : RCC/TCC = 1.84x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
