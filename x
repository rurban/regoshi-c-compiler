commit 59b1addd43bd32b24ef3e36e053928a5d0ee366e
Author: Reini Urban <reini.urban@gmail.com>
Date:   Sat Apr 18 08:40:05 2026 +0200

    add -O0 to skip peephole optimizer
    
    adjust docs

diff --git README.md README.md
index 613aaa9..e27c5a1 100644
--- README.md
+++ README.md
@@ -1,18 +1,18 @@
 # RCC — Regoshi C Compiler
 
-A fast, self-contained C compiler targeting x86-64 Windows. Written from scratch in C11.
+A fast, self-contained C compiler targeting x86-64 Windows and Unix. Written from scratch in C11.
 **RCC generates faster code than TCC** while keeping compilation speed competitive.
 
 ## Benchmark Results
 
 Six workloads: Fibonacci(38), Ackermann(3,10), Sieve of Eratosthenes (1M), 128×128 matrix multiply, floating-point math loop (500K), and bubble sort (5K).
 
-| Compiler | Execute (ms) | Compile (ms) | Total (ms) |
-|---|---:|---:|---:|
-| **RCC** | **349** | 1042 | **1391** |
-| TCC 0.9.27 | 400 | 1013 | 1413 |
-| GCC -O0 | 298 | 1021 | 1319 |
-| GCC -O2 | 132 | 1020 | 1152 |
+| Compiler   | Execute (ms) | Compile (ms) | Total (ms) |
+|------------|-------------:|-------------:|-----------:|
+| **RCC**    |      **349** |        1042  |    **1391**|
+| TCC 0.9.27 |        400   |        1013  |      1413  |
+| GCC -O0    |        298   |        1021  |      1319  |
+| GCC -O2    |        132   |        1020  |      1152  |
 
 - **RCC vs TCC execution: 0.87× (13% faster)**
 - All outputs verified correct against GCC -O2 reference.
@@ -36,6 +36,7 @@ Six workloads: Fibonacci(38), Ackermann(3,10), Sieve of Eratosthenes (1M), 128×
 - **C preprocessor** — `#include`, `#define`, `#ifdef`/`#ifndef`/`#if`, `#pragma once`, macro expansion with token pasting.
 - **Floating-point support** — `float`/`double` arithmetic, casts, function calls via SSE2 (xmm0–xmm3).
 - **Windows x64 ABI** — Shadow space, correct volatile/non-volatile register handling, 16-byte stack alignment.
+- **System x64 ABI** — No Shadow space. Calling convention. Float and struct alignment specialities.
 
 ## Supported C Features
 
@@ -57,25 +58,36 @@ gcc -std=c11 -O2 -o rcc.exe src/main.c src/lexer.c src/parser.c src/type.c src/c
 ./rcc.exe source.c -S -o output.s
 
 # Run benchmark
-powershell -File bench/run_bench.ps1
+make check
+
 ```
 
+## Options
+
+    -S       assemble-only
+    
+    -c       compile-only
+    
+    -o file  set output filename
+    
+    -O0      skip peephole optimizer
+
 ## Project Structure
 
-| File | Description |
-|---|---|
-| `src/main.c` | Driver: CLI, assembler/linker invocation |
-| `src/lexer.c` | Tokenizer with number/string/char literal support |
-| `src/preprocess.c` | C preprocessor (`#include`, `#define`, `#if`, macros) |
-| `src/parser.c` | Recursive-descent parser → AST |
-| `src/type.c` | Type system (primitives, pointers, arrays, structs, functions) |
-| `src/codegen.c` | x86-64 code generator with register allocator and peephole optimizer |
-| `src/opt.c` | AST-level optimizer and CTFE interpreter |
-| `src/alloc.c` | Arena memory allocator |
-| `src/rcc.h` | Shared data structures and declarations |
-| `include/` | Minimal C standard library headers (`stdio.h`, `math.h`, etc.) |
-| `bench/` | Benchmark suite and runner script |
-| `test/` | Test programs |
+| File             | Description                              |
+|------------------|------------------------------------------|
+| `src/main.c`     | Driver: CLI, assembler/linker invocation |
+| `src/lexer.c`    | Tokenizer with number/string/char literal support |
+| `src/preprocess.c`| C preprocessor (`#include`, `#define`, `#if`, macros) |
+| `src/parser.c`   | Recursive-descent parser → AST           |
+| `src/type.c`     | Type system (primitives, pointers, arrays, structs, functions) |
+| `src/codegen.c`  | x86-64 code generator with register allocator and peephole optimizer |
+| `src/opt.c`      | AST-level optimizer and CTFE interpreter |
+| `src/alloc.c`    | Arena memory allocator                   |
+| `src/rcc.h`      | Shared data structures and declarations  |
+| `include/`       | Minimal C standard library headers (`stdio.h`, `math.h`, etc.) |
+| `bench/`         | Benchmark suite and runner script        |
+| `test/`          | Test programs                            |
 
 ## License
 
diff --git src/codegen.c src/codegen.c
index 2d036fc..6a3df40 100644
--- src/codegen.c
+++ src/codegen.c
@@ -1335,6 +1335,7 @@ void codegen(Program *prog) {
         }
 
         // Skip peephole on very large functions to avoid truncation/pathological compile behavior.
+        if (opt_O0) goto skip_peep;
         if (nlines < 50000)
         for (int pass = 0; pass < 4; pass++) {
             for (int li = 0; li < nlines - 1; li++) {
@@ -1465,6 +1466,7 @@ void codegen(Program *prog) {
             }
         }
 
+    skip_peep:
         // Emit optimized lines
         for (int li = 0; li < nlines; li++) {
             if (lines[li] && lines[li][0])
diff --git src/main.c src/main.c
index d1b2db9..d852879 100644
--- src/main.c
+++ src/main.c
@@ -29,6 +29,8 @@ static char *read_file(char *path) {
     return buf;
 }
 
+bool opt_O0 = false;
+
 int main(int argc, char **argv) {
     char *out_path = "a.exe";
     char *in_path = NULL;
@@ -40,6 +42,8 @@ int main(int argc, char **argv) {
             opt_S = true;
         } else if (!strcmp(argv[i], "-c")) {
             opt_c = true;
+        } else if (!strcmp(argv[i], "-O0")) {
+            opt_O0 = true;
         } else if (!strcmp(argv[i], "-o")) {
             if (++i >= argc) {
                 fprintf(stderr, "error: missing argument for -o\n");
diff --git src/rcc.h src/rcc.h
index 1ce961c..8bbf249 100644
--- src/rcc.h
+++ src/rcc.h
@@ -125,6 +125,8 @@ extern Type *ty_float;
 extern Type *ty_double;
 extern Type *ty_ldouble;
 
+extern bool opt_O0;
+
 bool is_integer(Type *ty);
 bool is_flonum(Type *ty);
 bool is_number(Type *ty);
