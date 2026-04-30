# RCC — Regoshi C Compiler

A fast, self-contained C compiler targeting x86-64 Windows and Unix. Written from scratch in C11 by Hosokawa-t, a 16 year old student. And then ported to linux and fixed the rest by Reini Urban.
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
| RCC       |           44 |          739 |        783 |
| RCC -O1   |           46 |          736 |        782 |
| TCC       |        **7** |          617 |        624 |
| SLIMCC    |           44 |          626 |        670 |
| KEFIR     |          216 |          725 |        941 |
| KEFIR -O1 |          244 |          391 |        635 |
| GCC -O0   |           71 |          617 |        688 |
| GCC -O2   |          158 |          225 |        383 |
| Clang -O0 |          100 |          579 |        679 |
| Clang -O2 |          162 |      **219** |    **381** |

- RCC vs TCC vs GCC -O2 execution: same speed on windows, competitive on linux.
- All outputs verified correct against TCC, GCC -O2 and CLANG -O2 references.
- **Compile-time performance**: RCC emits assembly to stdout then invokes GCC (`system()`) to assemble and link, which is ~2× slower than TCC's native internal assembler/linker. The peephole optimizer also reparses assembly lines as text strings (4 passes over emitted asm), while TCC works on an internal abstract representation. Together these account for the compile-time gap. Generated code quality is on par with TCC.

## Key Features

- **Register-machine codegen** — 8-register allocator (r10, r11, rbx, r12–r15, rsi) with dynamic allocation, no stack machine overhead. The register allocator is a simple first-fit bitmask with no spilling to stack except for the two predefined spill slots. If all 8 registers are in use, it spills the additional registers on the stack. Currently with a spill warning.
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
- **Floating-point support** — `float`/`double/long double` arithmetic, casts, function calls via SSE2 (xmm0–xmm7). 80-bit long double x87 via `fld`/`fstp`, though truncated to 64 bits on store. Float args properly classified as SSE class with separate GP/FP argument counters.
- **Windows x64 ABI** — Shadow space, correct volatile/non-volatile register handling, 16-byte stack alignment.
- **SystemV x64 ABI** — No Shadow space. amd64 calling convention. Float and struct alignment specialities.
- **Inline builtins** — `memset`, `memcpy`, `memcmp`, `strlen`, `strcmp`, `strchr` expanded inline(`rep stosb`/`rep movsb`/`repe cmpsb`/`repne scasb`/ byte loops), avoiding libc call overhead. Mandatory SSE4.2 not yet.

## Supported C Features

Structs, unions, enums, typedefs, arrays (multi-dimensional), pointers (including function pointers), `for`/`while`/`do-while`/`switch`/`goto`, `sizeof`, `_Bool`, `static`, `extern`, variadic `printf`, string literals, compound assignment operators, pre/post increment, ternary operator, comma operator, designated initializers, \_Generic, attribute `__cleanup__`, `__aligned__`, `__packed__`, `__constructor__`, `__destructor__`, Windows and SystemV long doubles (internally all using SSE), unicode identifiers and strings, minimal `"wchar.h"`, inline, weak, gcc, enum and ms bitfields, old K&R function definitions.

Not yet: VLA's, GNU alias, atomics.

Top-level `__asm__("...")` statements are supported and emitted in source order. Unlike GCC (which hoists all file-scope `asm` blocks to the top of the output at `-O2`/`-O3` unless `-fno-toplevel-reorder` is used), rcc always preserves their original position relative to functions.

The tcc suite has 137/137 test passed (100%) on linux and mingw-cross, 101/104 on windows native.
Three tcc bugs have been detected so far.

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
    -S                 assemble-only
    -c                 compile-only
    -o file            set output filename
    -O0                skip peephole optimizer
    -mms-bitfields     use MSVC bitfields (default on Windows)
    -mno-ms-bitfields  use GCC bitfields (default on non-Windows)
    -Dname[=val]       define a macro value
    -Uname             undefine a macro value
    --help
    --version

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
[those](tcc_test_report_mingw1.1.md) test results (61/129 passed tcc tests), and [those](https://github.com/rurban/rcc/blob/old-mingw/bench/bench_report_mingw.md) benchmarks. Tested in the `old-mingw` branch via github actions.

This fork passes now [137/137 tests](tcc_test_linux.md) on linux, [137/137 tests](tcc_test_mingw_cross.md.md) on mingw-cross, and [101/104 tests](tcc_test_mingw.md) on windows native. macOS linking and arm64 port still in work (109/138 tests pass on arm64-elf).

## License

LGPL-2.1 — see [LICENSE](LICENSE) file.
