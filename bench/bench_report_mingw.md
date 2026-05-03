# Windows RCC Benchmark Results

_Generated: 05/03/2026 21:41:28_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         7157 |          474 |       7631 |
| RCC -O1 (optimized)   |         1202 |          477 |       1679 |
| TCC (Tiny C Compiler) |         1231 |          439 |       1670 |
| GCC -O0 (no opt)      |         3201 |          361 |       3562 |
| GCC -O2 (optimized)   |         3222 |           92 |       3314 |
| CLANG -O2 (optimized) |         6039 |          148 |       6187 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 5.81x
- Execute speed : RCC/TCC = 1.08x
- Total : RCC/TCC = 4.57x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
