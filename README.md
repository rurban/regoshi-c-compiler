# RCC — Regoshi C Compiler

A fast, self-contained C compiler targeting x86-64 Windows and Unix. Written from scratch in C11.
**RCC generates faster code than TCC** while keeping compilation speed competitive.

## Benchmark Results

Six workloads: Fibonacci(38), Ackermann(3,10), Sieve of Eratosthenes (1M), 128×128 matrix multiply, floating-point math loop (500K), and bubble sort (5K).

| Compiler   | Execute (ms) | Compile (ms) | Total (ms) |
| ---------- | -----------: | -----------: | ---------: |
| **RCC**    |      **349** |         1042 |   **1391** |
| TCC 0.9.27 |          400 |         1013 |       1413 |
| GCC -O0    |          298 |         1021 |       1319 |
| GCC -O2    |          132 |         1020 |       1152 |

- **RCC vs TCC execution: 0.87× (13% faster)**
- All outputs verified correct against GCC -O2 reference.

## Key Features

- **Register-machine codegen** — 8-register allocator (r10, r11, rbx, r12–r15, rsi) with dynamic allocation, no stack machine overhead.
- **Two-pass function emission** — Body generated to buffer first; prologue only pushes callee-saved registers actually used. Recursive functions like `fib` get zero callee-saved pushes.
- **Peephole optimizer** — Multi-pass assembly optimizer with:
  - Copy propagation (`mov r10, rax; mov [mem], r10` → `mov [mem], rax`)
  - Store-load forwarding (`mov [rbp-N], rcx; mov r10d, [rbp-N]` → `mov r10d, ecx`)
  - Immediate folding (`mov r11d, 1; cmp r10d, r11d` → `cmp r10d, 1`)
  - Identity elimination (`imul r10d, 1` → deleted, `add r10, 0` → deleted)
  - Strength reduction (multiply by power-of-2 → shift)
  - 3-instruction chain folding (`load; op; mov dst` → `load dst; op dst`)
  - Dead jump elimination (`jmp .L; .L:` → `.L:`)
  - Liveness-aware dead code removal
- **Direct function calls** — `call funcname` instead of `lea reg, [rip+func]; call reg`.
- **Pre-allocated shadow space** — 32-byte shadow space in stack frame; no `sub rsp`/`add rsp` per call for ≤4 args.
- **Compile-Time Function Execution (CTFE)** — AST interpreter evaluates pure functions with constant arguments at compile time.
- **C preprocessor** — `#include`, `#define`, `#ifdef`/`#ifndef`/`#if`, `#pragma once`, macro expansion with token pasting.
- **Floating-point support** — `float`/`double` arithmetic, casts, function calls via SSE2 (xmm0–xmm3).
- **Windows x64 ABI** — Shadow space, correct volatile/non-volatile register handling, 16-byte stack alignment.
- **SystemV x64 ABI** — No Shadow space. amd64 calling convention. Float and struct alignment specialities.

## Supported C Features

Structs, unions, enums, typedefs, arrays (multi-dimensional), pointers (including function pointers), `for`/`while`/`do-while`/`switch`/`goto`, `sizeof`, `_Bool`, `static`, `extern`, variadic `printf`, string literals, compound assignment operators, pre/post increment, ternary operator, comma operator, designated initializers.

Not yet: goto, break, continue, attribute `__cleanup__`, struct returns, long double, float casts.

## Build

```bash
gcc -std=c11 -O2 -o rcc.exe src/main.c src/lexer.c src/parser.c src/type.c src/codegen.c src/alloc.c src/preprocess.c src/opt.c
```

## Usage

```bash
# Compile to executable
./rcc.exe source.c -o output.exe

# Output assembly
./rcc.exe source.c -S -o output.S

# Run tests and benchmark
make check
make bench
```

## Options

    -Lpath   add linker path
    -lname   add lib
    -S       assemble-only
    -c       compile-only
    -o file  set output filename
    -O0      skip peephole optimizer

## Project Structure

| File               | Description                                                          |
| ------------------ | -------------------------------------------------------------------- |
| `src/main.c`       | Driver: CLI, assembler/linker invocation                             |
| `src/lexer.c`      | Tokenizer with number/string/char literal support                    |
| `src/preprocess.c` | C preprocessor (`#include`, `#define`, `#if`, macros)                |
| `src/parser.c`     | Recursive-descent parser → AST                                       |
| `src/type.c`       | Type system (primitives, pointers, arrays, structs, functions)       |
| `src/codegen.c`    | x86-64 code generator with register allocator and peephole optimizer |
| `src/opt.c`        | AST-level optimizer and CTFE interpreter                             |
| `src/alloc.c`      | Arena memory allocator                                               |
| `src/rcc.h`        | Shared data structures and declarations                              |
| `include/`         | Minimal C standard library headers (`stdio.h`, `math.h`, etc.)       |
| `bench/`           | Benchmark suite and runner script                                    |
| `test/`            | Test programs                                                        |

## License

LGPL-2.1 — see [LICENSE](LICENSE) file.
