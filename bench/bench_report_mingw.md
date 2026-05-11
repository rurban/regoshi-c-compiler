# Windows RCC Benchmark Results

_Generated: 05/11/2026 12:24:18_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         8627 |          571 |       9198 |
| RCC -O1 (optimized)   |         1197 |          574 |       1771 |
| TCC (Tiny C Compiler) |         1195 |          385 |       1580 |
| GCC -O0 (no opt)      |         3218 |          363 |       3581 |
| GCC -O2 (optimized)   |         4408 |           92 |       4500 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 7.22x
- Execute speed : RCC/TCC = 1.48x
- Total : RCC/TCC = 5.82x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
