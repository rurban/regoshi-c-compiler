# Windows RCC Benchmark Results

_Generated: 05/03/2026 08:32:28_

| Compiler              | Compile (ms) | Execute (ms) | Total (ms) |
| :-------------------- | -----------: | -----------: | ---------: |
| RCC (your compiler)   |         3162 |          550 |       3712 |
| RCC -O1 (optimized)   |         1254 |          551 |       1805 |
| TCC (Tiny C Compiler) |         1259 |          430 |       1689 |
| GCC -O0 (no opt)      |         1254 |          418 |       1672 |
| GCC -O2 (optimized)   |         1262 |          116 |       1378 |
| CLANG -O2 (optimized) |         1263 |          169 |       1432 |

## Windows RCC vs TCC Head-to-Head

- Compile speed : RCC/TCC = 2.51x
- Execute speed : RCC/TCC = 1.28x
- Total : RCC/TCC = 2.2x

## Output Correctness

- RCC (your compiler): OK
- RCC -O1 (optimized): OK
- TCC (Tiny C Compiler): OK
- GCC -O0 (no opt): OK
- GCC -O2 (optimized): OK
- CLANG -O2 (optimized): OK
