# RCC — Regoshi C Compiler

A fast, self-contained C compiler targeting x86-64 on Windows and Unix, and AArch64 (ARM64) on elf and darwin. Written from scratch in C11 by Hosokawa-t, a 16 year old student. And then ported to linux, arm64 and fixed the rest by Reini Urban.
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

| Compiler  | Compile (ms) | Execute (ms) | Total (ms) |
| :-------- | -----------: | -----------: | ---------: |
| RCC       |           45 |          597 |        642 |
| RCC -O1   |           47 |          606 |        653 |
| TCC       |       **12** |          565 |    **577** |
| SLIMCC    |           49 |          625 |        674 |
| KEFIR     |          228 |          662 |        890 |
| KEFIR -O1 |          192 |      **486** |        678 |
| GCC -O0   |           69 |          562 |        631 |
| GCC -O2   |          178 |          214 |        392 |
| Clang -O0 |          108 |          629 |        737 |
| Clang -O2 |          132 |          228 |        360 |

- RCC vs TCC vs GCC -O2 execution: same speed on windows, competitive on linux.
- All outputs verified correct against TCC, GCC -O2 and CLANG -O2 references.
- **Compile-time performance**: RCC emits assembly to stdout then invokes GCC (`system()`) to assemble and link, which is ~2× slower than TCC's native internal assembler/linker. The peephole optimizer also reparses assembly lines as text strings (4 passes over emitted asm), while TCC works on an internal abstract representation. Together these account for the compile-time gap. Generated code quality is on par with TCC.

## Key Features

- **Register-machine codegen** — 8-register allocator on x86-64 (r10, r11, rbx, r12–r15, rsi), 12-register on ARM64 (x10–x15, x19–x24) with dynamic allocation, no stack machine overhead. The register allocator is a simple first-fit bitmask with no spilling to stack except for the predefined spill slots. If all registers are in use, it spills the additional registers on the stack. Currently with a spill warning on -W.
- **Two-pass function emission** — Body generated to buffer first; prologue only pushes callee-saved registers actually used. Recursive functions like `fib` get zero callee-saved pushes.
- **Peephole optimizer** — Multi-pass assembly optimizer with:
  - Copy propagation (`mov r10, rax; mov [mem], r10` → `mov [mem], rax`)
  - Store-load forwarding (`mov [rbp-N], rcx; mov r10d, [rbp-N]` → `mov r10d, ecx`)
  - Immediate folding (`mov r11d, 1; cmp r10d, r11d` → `cmp r10d, 1`)
  - Identity elimination (`imul r10d, 1` → deleted, `add r10, 0` → deleted)
  - Strength reduction (multiply by power-of-2 → shift) in codegen already.
  - 3-instruction chain folding (`load; op; mov dst` → `load dst; op dst`)
  - Dead jump elimination (`jmp .L; .L:` → `.L:`)
  - Liveness-aware dead code removal
- **Direct function calls** — `call funcname` instead of `lea reg, [rip+func]; call reg`.
- **Shadow space** — Maximal 32-byte shadow space in stack frame; no `sub rsp`/`add rsp` per call for ≤4 args.
- **Compile-Time Function Execution (CTFE)** — AST interpreter evaluates pure functions with constant arguments at compile time with -O1.
- **C preprocessor** — `#include`, `#define`, `#ifdef`/`#ifndef`/`#if`, `#pragma once`, macro expansion with token pasting.
- **Floating-point support** — `float`/`double/long double` arithmetic, casts, function calls via SSE2 on x86-64 (xmm0–xmm7) or via ARM64 NEON/FP (v0–v7). 80-bit long double x87 on x86-64 via `fld`/`fstp` (truncated to 64 bits on store). ARM64 ELF 128-bit long double passed in register pairs (v0–v7 in even-odd pairs) following the AAPCS64 calling convention. Float args properly classified as SSE/FP class with separate GP/FP argument counters. ARM^4 on APPLE only uses 8-byte doubles.
- **Windows x64 ABI** — Shadow space, correct volatile/non-volatile register handling, 16-byte stack alignment.
- **SystemV x64 ABI** — No Shadow space. amd64 calling convention. Float and struct alignment specialities.
- **ARM64 ABI (AAPCS64)** — x29 frame pointer, x30 link register, x0–x7 argument/return registers, x8 indirect result register, x9–x15 caller-saved, x19–x28 callee-saved. Variadic args passed on the stack. 16-byte stack alignment. NEON v0–v7 for FP/SIMD args; long double pairs on ELF use even-odd register pairs.
- **Inline builtins** — `memset`, `memcpy`, `memcmp`, `strlen`, `strcmp`, `strchr` expanded inline(`rep stosb`/`rep movsb`/`repe cmpsb`/`repne scasb`/ byte loops), avoiding libc call overhead. Mandatory SSE4.2 not yet.

## Supported C Features

Structs, unions, enums, typedefs, arrays (multi-dimensional), pointers (including function pointers), `for`/`while`/`do-while`/`switch`/`goto`, `sizeof`, `_Bool`, `static`, `extern`, variadic `printf`, string literals, compound assignment operators, pre/post increment, ternary operator, comma operator, designated initializers, \_Generic, attribute `__cleanup__`, `__aligned__`, `__packed__`, `__constructor__`, `__destructor__`, Windows and SystemV long doubles (internally all using SSE), ARM64 long doubles (128-bit quad precision via register pairs in elf, 8 byte on APPLE), unicode identifiers and strings, minimal `"wchar.h"`, inline, weak, gcc, enum and ms bitfields, old K&R function definitions, VLA's, atomics (LL/SC on ARM64, xadd/lock on x86), GNU alias.

Not yet: C23, -fpie, -fpic

Top-level `__asm__("...")` statements in AT&T, Intel or ARM syntax are supported and emitted in source order. Unlike GCC (which hoists all file-scope `asm` blocks to the top of the output at `-O2`/`-O3` unless `-fno-toplevel-reorder` is used), rcc always preserves their original position relative to functions.

The tcc suite has 150/150 test passed (100%) on linux and mingw-cross,
148/150 on arm64-cross, 149/151 on darwin-cross,
109/111 on windows native, 134/147 on arm64-darwin native.

Three tcc core and test bugs have been detected so far. Fixes in the work.

## Build

```bash
gcc -std=c11 -O2 -o rcc src/main.c src/lexer.c src/parser.c src/type.c src/codegen.c src/alloc.c src/preprocess.c src/opt.c
```

## Usage

```bash
# Compile to executable
./rcc.exe -o output.exe source.c ...

# Output assembly
./rcc.exe -S -o output.S source.c

# Run tests and benchmark
make check
make bench
```

## Options

    -I path            add include path
    -Lpath             add linker path
    -lname             add lib
    -E                 preprocessor-only
    -dM                dump all macro definitions (use with -E)
    -S                 assemble-only
    -c                 compile-only
    -o file            set output filename
    -O0                disable peephole optimizer
    -O1                enable peephole + CTFE optimizations
    -mms-bitfields     use MSVC bitfields (default on Windows)
    -mno-ms-bitfields  use GCC bitfields (default on non-Windows)
    -Dname[=val]       define a macro value
    -Uname             undefine a macro value
    -W                 print diagnostic warnings (stack spilling)
    -###               dry-run (print commands, don't execute)
    -print-search-dirs print install, include and library paths
    --help
    --version

## Project Structure

| File               | Description                                                                |
| ------------------ | -------------------------------------------------------------------------- |
| `src/main.c`       | Driver: CLI, assembler/linker invocation                                   |
| `src/lexer.c`      | Tokenizer with number/string/char literal support                          |
| `src/preprocess.c` | C preprocessor (`#include`, `#define`, `#if`, macros)                      |
| `src/parser.c`     | Recursive-descent parser → AST                                             |
| `src/type.c`       | Type system (primitives, pointers, arrays, structs, functions)             |
| `src/codegen.c`    | x86-64/ARM64 code generator with register allocator and peephole optimizer |
| `src/opt.c`        | AST-level optimizer and CTFE interpreter                                   |
| `src/alloc.c`      | Arena memory allocator                                                     |
| `src/rcc.h`        | Shared data structures and declarations                                    |
| `include/`         | Minimal C standard library headers (`stdio.h`, `math.h`, etc.)             |
| `bench/`           | Benchmark suite and runner script                                          |
| `test/`            | Test programs                                                              |

## Unix fork

The original windows repo is now at https://github.com/DocDamage/realtime-c-compiler with
[those](tcc_test_report_mingw1.1.md) test results (61/129 passed tcc tests), and [those](https://github.com/rurban/rcc/blob/old-mingw/bench/bench_report_mingw.md) benchmarks. Tested in the `old-mingw` branch via github actions.

This fork passes now:

- [150/150 tests](tcc_test_linux.md) on linux (x86-64)
- [148/148 tests](tcc_test_mingw_cross.md) on mingw-cross (x86-64)
- [148/151 tests](tcc_test_arm64_cross.md) on arm64-cross (ELF)
- [149/151 tests](tcc_test_darwin_cross.md) on darwin-cross (Mach-O, compile+link only)
- [143/149 tests](tcc_test_arm64.md) on arm64-darwin native
- [109/111 tests](tcc_test_mingw.md) on windows native via powershell testing
- For the c-testsuite it passes 220/220 tests on all platforms.

## License

LGPL-2.1 — see [LICENSE](LICENSE) file.
