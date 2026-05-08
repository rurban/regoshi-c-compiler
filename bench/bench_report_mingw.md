# Windows RCC Benchmark Results

_Generated: 05/09/2026 05:33:46_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         4940 |          475 |       5415 |
| RCC -O1 (optimized)   |         1197 |          473 |       1670 |
| TCC (Tiny C Compiler) |         1207 |          386 |       1593 |
| GCC -O0 (no opt)      |         1200 |          361 |       1561 |
| GCC -O2 (optimized)   |         1202 |           92 |       1294 |
| CLANG -O2 (optimized) |         2199 |          146 |       2345 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 4.09x
- Execute speed : RCC/TCC = 1.23x
- Total : RCC/TCC = 3.4x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
