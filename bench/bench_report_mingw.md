# Windows RCC Benchmark Results

_Generated: 05/03/2026 05:25:08_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         2866 |          557 |       3423 |
| RCC -O1 (optimized)   |         1255 |          552 |       1807 |
| TCC (Tiny C Compiler) |         1253 |          430 |       1683 |
| GCC -O0 (no opt)      |         1250 |          420 |       1670 |
| GCC -O2 (optimized)   |         1250 |          116 |       1366 |
| CLANG -O2 (optimized) |         1257 |          169 |       1426 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.29x
- Execute speed : RCC/TCC = 1.3x
- Total : RCC/TCC = 2.03x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
