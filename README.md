# RCC — Regoshi C Compiler

A fast, self-contained C compiler targeting x86-64 Windows and Unix. Written from scratch in C11 by Hosokawa-t, a 16 year old student.
**RCC equally fast code as TCC** while keeping compilation speed competitive.

## Benchmark Results

Six workloads: Fibonacci(38), Ackermann(3,10), Sieve of Eratosthenes (1M), 128×128 matrix multiply, floating-point math loop (500K), and bubble sort (5K).

Windows:

| Compiler   | Execute (ms) | Compile (ms) | Total (ms) |
| ---------- | -----------: | -----------: | ---------: |
| RCC        |         1009 |          514 |       1523 |
| TCC 0.9.27 |         1005 |          431 |       1436 |
| GCC -O0    |         3012 |          417 |       3429 |
| GCC -O2    |         1002 |          115 |       1117 |

Linux:

| Compiler | Compile (ms) | Execute (ms) | Total (ms) |
| :------- | -----------: | -----------: | ---------: |
| RCC      |           74 |          596 |        670 |
| TCC      |        **7** |          598 |        605 |
| SLIMCC   |           50 |          649 |        699 |
| KEFIR    |          215 |          774 |        989 |
| GCC0     |           76 |          592 |        668 |
| GCCO2    |          227 |      **227** |        454 |
| CLANG0   |          123 |          673 |        796 |
| CLANGO2  |          159 |          239 |    **398** |

- RCC vs TCC vs GCC -O2 execution: same speed on windows, competitive on linux.
- All outputs verified correct against TCC, GCC -O2 and CLANG -O2 references.

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

Structs, unions, enums, typedefs, arrays (multi-dimensional), pointers (including function pointers), `for`/`while`/`do-while`/`switch`/`goto`, `sizeof`, `_Bool`, `static`, `extern`, variadic `printf`, string literals, compound assignment operators, pre/post increment, ternary operator, comma operator, designated initializers, \_Generic, attribute `__cleanup__`, `__aligned__`, `__packed__`, `__constructor__`, `__destructor__`, Windows and SystemV long doubles (internally all using SSE), unicode identifiers and strings, minimal `"wchar.h"`, enum bitfields.

Not yet: VLA's, ms bitfields, inline, GNU alias, atomics, old K&R function definitions.

Top-level `__asm__("...")` statements are supported and emitted in source order. Unlike GCC (which hoists all file-scope `asm` blocks to the top of the output at `-O2`/`-O3` unless `-fno-toplevel-reorder` is used), rcc always preserves their original position relative to functions.

The tcc suite has 126/128 passed (98%), 2 failed.

## Build

```bash
gcc -std=c11 -O2 -o rcc src/main.c src/lexer.c src/parser.c src/type.c src/codegen.c src/alloc.c src/preprocess.c src/opt.c
```

## Usage

```bash
# Compile to executable
./rcc.exe -o output.exe source.c

# Output assembly
./rcc.exe -S -o output.S source.c

# Run tests and benchmark
make check
make bench
```

## Options

    -I path       add include path
    -Lpath        add linker path
    -lname        add lib
    -E            preprocessor-only
    -S            assemble-only
    -c            compile-only
    -o file       set output filename
    -O0           skip peephole optimizer
    -Dname[=val]  define a macro value
    -Uname        undefine a macro value

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

## Unix fork

The original windows repo is at https://github.com/DocDamage/realtime-c-compiler with
[those](tcc_test_report_mingw1.1.md) test results (61/139 passed tcc tests), and [those](https://github.com/rurban/rcc/blob/old-mingw/bench/bench_report_mingw.md) benchmarks. Tested in the `old-mingw` branch via github actions.

This fork passes now [126/128 tests](tcc_test_linux.md) on linux and [83/100 tests](tcc_test_mingw.md) on windows. macOS linking still in work.

## License

LGPL-2.1 — see [LICENSE](LICENSE) file.
