#!/bin/sh
# Cross-build ARM64 rcc and run the TCC test suite against it.
# Usage: ./arm64-test.sh [test-name]
set -e
trap 'make clean; make -s' EXIT   # restore host build after cross-test
make clean
make -s CC=aarch64-linux-gnu-gcc

if [ -n "${1:-}" ]; then
    # Run a single test directly
    TEST_BASE="$1"
    TEST_SRC="tinycc/tests/tests2/${TEST_BASE}.c"
    TEST_EXPECT="tinycc/tests/tests2/${TEST_BASE}.expect"
    TMP_OUT="/tmp/rcc_test_${TEST_BASE}_$$.out"
    TMP_EXE="/tmp/rcc_test_${TEST_BASE}_$$"
    printf "  %-40s " "${TEST_BASE}..."
    if ! ./arm64-cross.sh "$TEST_SRC" -o "$TMP_EXE" 2>/dev/null; then
        printf "COMPILE FAIL\n"
        exit 1
    fi
    if [ -f "$TEST_EXPECT" ]; then
        if "$TMP_EXE" >"$TMP_OUT" 2>&1 && diff -q "$TEST_EXPECT" "$TMP_OUT" >/dev/null 2>&1; then
            printf "PASS\n"
        else
            printf "MISMATCH\n"
            exit 1
        fi
    else
        if "$TMP_EXE" >"$TMP_OUT" 2>&1; then
            printf "PASS (no expect)\n"
        else
            printf "EXEC FAIL\n"
            exit 1
        fi
    fi
    rm -f "$TMP_OUT" "$TMP_EXE"
else
    ./run_tcc_suite.sh ./rcc-arm64
fi
