# RCC Test Infrastructure

1. Makefile `make check` Target

File: Makefile (lines 134-143)

```
ifeq ($(OS),Windows_NT)
TEST_RUNNER = powershell -ExecutionPolicy Bypass -File run_tcc_suite.ps1 -O1 && ./gen-test-report.sh mingw
BENCH_RUNNER = powershell -ExecutionPolicy Bypass -File bench/run_bench.ps1 ./$(TARGET)
else
TEST_RUNNER = ./run_tcc_suite.sh "" "" -O1 && ./run-c-testsuite.sh && test/compliance/run.sh && ./gen-test-report.sh
BENCH_RUNNER = ./bench/run_bench.sh ./$(TARGET)
endif

test check: $(TARGET)
	@$(TEST_RUNNER)
```

On Linux/macOS: `make check` runs four things sequentially:

1. `./run_tcc_suite.sh "" "" -O1` — TCC compatibility test suite with `-O1` optimization
2. `./run-c-testsuite.sh` — c-testsuite (220 single-exec C tests)
3. `test/compliance/run.sh` — Compliance tests comparing rcc output against gcc
4. `./gen-test-report.sh` — Generates a unified `test_report_<platform>.md` from all test summaries

On Windows: `make check` runs `run_tcc_suite.ps1 -O1` then `gen-test-report.sh mingw`.

The `test-torture` target runs the gcc torture tests, and updates the test report.

The `gen-test-report.sh` auto-detects the native platform from `uname` or accepts an explicit
platform argument for cross-compilation runs.

---

2. The `make test-full` Target

File: Makefile (lines 145-152)

```
test-full: $(TARGET)
	$(MAKE) clean
	$(MAKE)
	@$(TEST_RUNNER)
	$(MAKE) test-torture
	-./mingw-test.sh
	-./arm64-test.sh
	-./darwin-test.sh
```

This does a full clean rebuild and runs all test suites:

1. Rebuild from scratch
2. Runs the standard `make check` suite (including unified report generation)
3. Runs the GCC torture test suite (make torture) and updates the native report
4. Runs cross-compilation tests for mingw, arm64, and darwin
   (each generates its own unified report via EXIT trap)

---

3. Test Suites Breakdown

3a. `run_tcc_suite.sh` — TCC Compatibility Tests (main test runner)

File: `run_tcc_suite.sh` (748 lines)

Key variables:

- `SCRIPT_DIR=.` — project root
- `REPORT_DIR="test"` — central report output directory
- `PLATFORM` — detected platform: `linux`, `mingw_cross`, `arm64_cross`, `darwin_cross`, `arm64`

How it works:

- Locates the rcc binary (or cross-compilation wrapper), defaulting to `./rcc`
- Detects the platform to set `PLATFORM` and select report file:
  | Platform | PLATFORM | Report file |
  | -------------- | -------------- | ------------------------------------------ |
  | Linux native | `linux` | `test/tcc_test_linux.md` |
  | Mingw cross | `mingw_cross` | `test/tcc_test_mingw_cross.md` |
  | ARM64 cross | `arm64_cross` | `test/tcc_test_arm64_cross.md` |
  | Darwin cross | `darwin_cross` | `test/tcc_test_darwin_cross.md` |
  | ARM64 native | `arm64` | `test/tcc_test_arm64.md` |
  | Windows native | `mingw` | `test/tcc_test_mingw.md` |

Skip lists:

- `SKIP_TESTS` (global): tests requiring TCC internals (bound checker, backtrace, riscv asm)
- `MINGW_SKIP_TESTS`: platform-specific mingw skips
- `INTEL_SKIP_TESTS`: arm64-specific tests skipped on x86 (73, 138, 139, 140)
- `ARM64_SKIP_TESTS`: `95_bitfields_ms` and `127_asm_goto` skipped on arm64

Test flow for each `.c` file in `tinycc/tests/tests2/`:

1. Skip check against platform-specific lists
2. Compile with rcc (with optional flags like `-lm` for math tests)
3. If darwin cross: treat compile success as PASS (can't execute Mach-O)
4. Execute the binary (via qemu-aarch64 for arm64 cross, wine for mingw cross)
5. Check exit code matches expected exit code (special: `101_cleanup` expects exit 105)
6. Compare output against `.expect` file (if exists) using `diff -Nbu`
7. Also runs unit tests in `test/` directory (`test_*.c` files)

Results format:

```
Results: 152/152 passed (100%), 0 failed.
```

Report file format: Markdown tables with columns: Test, Status, Message

```markdown
# TCC Test Suite Report for RCC

Generated: May 2026

## Summary

- **Total**: 152
- **Passed**: 152
- **Failed**: 0
- **Pass Rate**: 100%

## Detailed Results

| Test          | Status    | Message        |
| :------------ | :-------- | :------------- |
| 00_assignment | PASS      | Output matches |
| 129_scopes    | EXEC_FAIL | non-zero exit  |
```

Status values: `PASS`, `SKIP`, `COMPILE_FAIL`, `EXEC_FAIL`, `MISMATCH`, `COMPILE_OK` (darwin only)

Change detection: At end of run, compares against previous report, prints regressions/fixes/changes.

Summary file: Writes `test-tcc-<PLATFORM>.summary` for the unified report.

Exit code: Returns 0 if pass count meets platform-specific threshold
(149+ for Linux, 147+ for mingw, 152+ for arm64/darwin).

3b. `run-c-testsuite.sh` — c-testsuite Runner

File: `run-c-testsuite.sh` (85 lines)

How it works:

- Skips on macOS (not yet supported)
- Initializes git submodule if needed
- Runs: `cd c-testsuite && env CC="../rcc" CFLAGS="-O1 -lm" ./single-exec posix | scripts/tapsummary | tee ../c-testsuite.tap.txt`
- The `single-exec` script uses a "runner" pattern from `runners/single-exec/rcc`
- Outputs TAP-format results, piped through `tapsummary`
- Sets a maximum failure limit (`MAX_FAILS=1`)
- The final TAP summary is stored in `c-testsuite.tap.txt`
- Writes `test-ctest-<platform>.summary` for the unified report

c-testsuite output format (`c-testsuite.tap.txt`):

```
Test summary:
pass 220
fail 0
skip 0
---------
total 220
```

3c. `test/compliance/run.sh` — Compliance Tests

File: `test/compliance/run.sh` (85 lines)

How it works:

- For each `.c` file (or a single specified test):
  1. Compile with gcc
  2. Compile with rcc
  3. Run both and compare stdout
- Reports PASS/FAIL/SKIP per test
- Summary: `=== PASS=N FAIL=N ===`
- Tests: `01_type_sizes.c` through `15_long_double_conv.c` (15 compliance tests)
- Writes `test-compliance-<platform>.summary` for the unified report

3d. `test/torture/run.sh` — GCC Torture Tests

File: `test/torture/run.sh` (199 lines)

How it works:

- Runs ~1000 GCC torture test files from `test/torture/*.c`
- Takes optional rcc binary as first argument (for cross-compilation testing)
- Detects cross-compilation wrapper, sets `PLATFORM` and runner:
  - `arm64_cross`: `qemu-aarch64` for arm64 cross
  - `mingw_cross`: `wine` for mingw cross
  - Native: auto-detected from `uname`
- Skips tests with:
  - `dg-skip-if` x86-only (x87 FPU, MMX, SSE)
  - `dg-require-effective-target trampolines` (macOS W^X)
  - `scalar_storage_order` attribute (GCC-only)
  - `-finstrument-functions`
  - GCC-specific ULL bitfield arithmetic
  - Nested functions / statement expressions
  - Missing include files (infrastructure, not compiler bugs)
- Compiles with rcc and `-lm`, then runs binary with 5-second timeout
- Classifies failures as `FAIL_COMPILE` or `FAIL_RUNTIME`
- Summary: `=== TOTAL=N PASS=N FAIL_COMPILE=N FAIL_RUNTIME=N SKIP=N ===`
- Writes `test-torture-<PLATFORM>.summary` for the unified report
- Exit code: >= platform specific pass threshold

---

4. Cross-Compilation Test Scripts

4a. `mingw-test.sh`

File: `mingw-test.sh` (10 lines)

```sh
#!/bin/sh
# Cross-build Windows rcc and run the TCC test suite against it.
set -e
trap './gen-test-report.sh mingw_cross; make clean; make -s' EXIT   # restore host build after cross-test
make clean
make -s CC=x86_64-w64-mingw32-gcc
WINE_DISABLE_RANDR=1
export WINE_DISABLE_RANDR
./run_tcc_suite.sh
test/torture/run.sh ../../mingw-cross.sh
./gen-test-report.sh mingw_cross
```

- Cleans and rebuilds rcc with the mingw cross-compiler (`x86_64-w64-mingw32-gcc`)
- Produces `rcc.exe` (Windows PE binary)
- Runs `run_tcc_suite.sh`, which auto-detects `rcc.exe` and uses `mingw-cross.sh` wrapper
- Runs torture tests via `mingw-cross.sh`
- Generates `test_report_mingw_cross.md` via EXIT trap (always, even on failure)

4b. `arm64-test.sh`

File: `arm64-test.sh` (40 lines)

```sh
#!/bin/sh
# Cross-build ARM64 rcc and run the TCC test suite against it.
set -e
trap './gen-test-report.sh arm64_cross; make clean; make -s' EXIT   # restore host build after cross-test
make clean
make -s CC=aarch64-linux-gnu-gcc
```

- Builds `rcc-arm64` with the aarch64 cross-compiler
- If no test name given: runs `run_tcc_suite.sh ./rcc-arm64` and then `test/torture/run.sh ../../arm64-cross.sh`
- If single test name given: compiles that one test and compares output directly
- Generates `test_report_arm64_cross.md` via EXIT trap

4c. `darwin-test.sh`

File: `darwin-test.sh` (32 lines)

```sh
#!/bin/sh
# Build rcc-darwin and run the TCC test suite for arm64-darwin.
# Tests compile+link only (can't execute Mach-O on Linux).
set -e
trap './gen-test-report.sh darwin_cross; make -s clean; make -s' EXIT
make -s clean
make -s CC=gcc CFLAGS="-std=c11 -Wall -Wextra -O2 -g -D__APPLE__ -DARCH_ARM64" \
    TARGET=rcc-darwin OBJ_EXT=.darwin.o
./run_tcc_suite.sh ./rcc-darwin
```

- Compiles `rcc-darwin` with `-D__APPLE__ -DARCH_ARM64` (host binary, Darwin-targeted codegen)
- Runs the TCC test suite via `darwin-cross.sh` (compile+link only, no execution)
- Generates `test_report_darwin_cross.md` via EXIT trap

Cross-Compilation Wrappers:

- `mingw-cross.sh` (84 lines): Runs `rcc.exe` under Wine to produce `.s` files, then assembles+links with `x86_64-w64-mingw32-gcc`
- `arm64-cross.sh` (101 lines): Runs `rcc-arm64` under `qemu-aarch64` to produce `.s` files, then assembles+links with `aarch64-linux-gnu-gcc`
- `darwin-cross.sh` (97 lines): Runs `rcc-darwin` natively to produce `.s` files, then assembles+links in a Docker/podman container running `aarch64-apple-darwin20.4-clang`

---

5. Test Report Files

5a. Detailed TCC Reports (`test/` directory)

Each run of `run_tcc_suite.sh` writes a detailed per-test markdown report:

| File                            | Platform                  |
| ------------------------------- | ------------------------- |
| `test/tcc_test_linux.md`        | Linux x86_64 native       |
| `test/tcc_test_mingw_cross.md`  | Linux → Windows cross     |
| `test/tcc_test_arm64_cross.md`  | Linux → ARM64 cross       |
| `test/tcc_test_darwin_cross.md` | Linux → macOS ARM64 cross |
| `test/tcc_test_arm64.md`        | macOS ARM64 native        |
| `test/tcc_test_mingw.md`        | Windows x86_64 native     |

Format: Markdown with `# TCC Test Suite Report for RCC` header, Summary section,
and Detailed Results table with `| Test | Status | Message |`.
Compatible with `prettier` from the pre-commit hook.

To find failures: `grep -E 'FAIL|MISMATCH' test/tcc_test_mingw_cross.md`

5b. Machine-Readable Summaries (root)

Each test suite writes a `test-<suite>-<platform>.summary` file in the project root:

| File                                 | Suite       | Format                                  |
| ------------------------------------ | ----------- | --------------------------------------- |
| `test-tcc-<platform>.summary`        | TCC compat  | `SUITE=tcc\nTOTAL=N\nPASS=N\nFAIL=N`    |
| `test-ctest-<platform>.summary`      | c-testsuite | `SUITE=c-testsuite\nTOTAL=N\nPASS=N...` |
| `test-compliance-<platform>.summary` | Compliance  | `SUITE=compliance\nTOTAL=N\nPASS=N...`  |
| `test-torture-<platform>.summary`    | GCC Torture | `SUITE=torture\n...\nFAIL_COMPILE=N...` |

Platform values: `linux`, `arm64`, `mingw`, `mingw_cross`, `arm64_cross`, `darwin_cross`

These summary files are kept in the repository for tracking test history.

5c. Unified Reports (root)

`gen-test-report.sh` reads all `test-*.<platform>.summary` files and generates
a unified markdown report `test_report_<platform>.md`:

```markdown
# RCC Test Suite Report

**Platform**: Linux x86_64
Generated: May 06 2026 13:14

## Overall Summary

- **Total**: 1387
- **Passed**: 1220
- **Failed**: 104
- **Overall Pass Rate**: 87%

## TCC Compatibility Tests

- **Total**: 152
- **Passed**: 152
- **Failed**: 0
- **Pass Rate**: 100%

## c-testsuite

- **Total**: 220
- **Passed**: 220
- **Failed**: 0
- **Pass Rate**: 100%

## Compliance Tests (vs GCC)

- **Total**: 15
- **Passed**: 15
- **Failed**: 0
- **Pass Rate**: 100%

## GCC Torture Tests

- **Total**: 995
- **Passed**: 833
- **Failed**: 104
- **Skipped**: 58
- **Fail Compile**: 32
- **Fail Runtime**: 72
- **Pass Rate (excl. skip)**: 88%
```

Only test suites with data available are included (e.g., cross-compilation
reports typically include TCC + Torture only).

---

6. `run_tcc_suite.ps1` — Windows PowerShell Test Runner

File: `run_tcc_suite.ps1` (373 lines)

Mirrors `run_tcc_suite.sh` for Windows. Key differences:

- Sets `[Console]::OutputEncoding` to UTF8 for proper test handling
- Uses `Start-Process` for compilation
- On mismatch, emits hex dump diagnostics and assembly output
- Skip list is slightly different (Windows-specific: `104_inline`, `106_versym`, `120_alias`, `125_atomic_misc`, `128_run_atexit` among others)
- Outputs `test/tcc_test_mingw.md` detail report
- Writes `test-tcc-mingw.summary` for the unified report
- Exit threshold: >=108 passed

---

7. CI Configuration

File: `.github/workflows/ci.yml` (137 lines)

Triggers: `push` and `pull_request`

Matrix:

- `ubuntu-latest` (Linux)
- `macos-latest` (macOS Apple Silicon)
- `windows-latest` (Windows, uses msys2 MINGW64)

Steps:

1. Checkout with submodules (recursive, full depth)
2. Install dependencies (gcc, TCC, cppcheck on Linux; brew on macOS; msys2 on Windows)
3. Build and install TinyCC for comparison
4. `make` (build rcc)
5. Run tests: `rcc --version`, `rcc -print-search-dirs`, `make check`
6. `make lint`
7. Conditionally run `make bench` if source files changed
8. Upload artifacts (per-platform: TCC detail report + unified report + bench report)
9. Create GitHub release on tags

Uploaded artifacts per platform:

- Windows: `bench/bench_report_mingw.md`, `test/tcc_test_mingw.md`, `test_report_mingw.md`
- macOS: `bench/bench_report_darwin.md`, `test/tcc_test_arm64.md`, `test_report_arm64.md`
- Linux: `bench/bench_report_linux.md`, `test/tcc_test_linux.md`, `test_report_linux.md`

Note: The CI only runs `make check` (not `make test-full`). Cross-compilation tests
(mingw, arm64, darwin) are NOT run in CI — they must be run locally.

---

8. Summary: Test Execution Flow

```
make check
  └─ Non-Windows:
              ./run_tcc_suite.sh "" "" -O1  → test/tcc_test_linux.md
                                              test-tcc-linux.summary
              ./run-c-testsuite.sh           → c-testsuite.tap.txt
                                              test-ctest-linux.summary
              test/compliance/run.sh         → console output
                                              test-compliance-linux.summary
              ./gen-test-report.sh           → test_report_linux.md (unified)
  └─ Windows:
              run_tcc_suite.ps1 -O1          → test/tcc_test_mingw.md
                                              test-tcc-mingw.summary
              ./gen-test-report.sh mingw     → test_report_mingw.md

make test-full
  └─ make clean && make
     make check (as above)
     test/torture/run.sh                     → test-torture-linux.summary
     ./gen-test-report.sh linux              → test_report_linux.md (updated)
     ./mingw-test.sh                         → test/tcc_test_mingw_cross.md
                                               test-tcc-mingw_cross.summary
                                               test-torture-mingw_cross.summary
                                            → test_report_mingw_cross.md
     ./arm64-test.sh                         → test/tcc_test_arm64_cross.md
                                               test-tcc-arm64_cross.summary
                                               test-torture-arm64_cross.summary
                                            → test_report_arm64_cross.md
     ./darwin-test.sh                        → test/tcc_test_darwin_cross.md
                                               test-tcc-darwin_cross.summary
                                            → test_report_darwin_cross.md
```

The full test infrastructure provides **4 different test suites** across
**6 platforms** (Linux x86_64, macOS ARM64 native, Windows x86_64 native,
Linux→Mingw cross, Linux→ARM64 cross, Linux→Darwin cross)
with a total of approximately **1500+ individual test cases**.
