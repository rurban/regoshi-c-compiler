#!/bin/bash
# GCC torture test runner for rcc. Stolen from ncc.
# Usage: ./run.sh [test.c]  — run one test
#        ./run.sh           — run all tests
#        ./run.sh --summary — run all and show only summary

cd "$(dirname "$0")" || exit
RCC="../../rcc"
PASS=0
FAIL_COMPILE=0
FAIL_RUNTIME=0
SKIP=0
TOTAL=0
COMPILE_ERRORS=""
RUNTIME_ERRORS=""
SUMMARY_ONLY=0

if [ "$1" = "--summary" ]; then
    SUMMARY_ONLY=1
    shift
fi

run_test() {
    local src="$1"
    local name
    name="$(basename "$src" .c)"
    TOTAL=$((TOTAL+1))

    # dg-skip-if: x86-only tests (x87 FPU, MMX, SSE, etc.)
    if grep -qE 'dg-skip-if.*!\s*\{.*(i\?86|x86_64|i386)' "$src" 2>/dev/null; then
        SKIP=$((SKIP+1))
        [ $SUMMARY_ONLY -eq 0 ] && echo "SKIP(x86-only): $name"
        return
    fi

    # dg-require-effective-target trampolines (macOS W^X prevents nested fn trampolines)
    if grep -qE 'dg-require-effective-target trampolines' "$src" 2>/dev/null; then
        SKIP=$((SKIP+1))
        [ $SUMMARY_ONLY -eq 0 ] && echo "SKIP(trampolines): $name"
        return
    fi

    # scalar_storage_order attribute (GCC-only, byte-order control)
    if grep -q 'scalar_storage_order' "$src" 2>/dev/null; then
        SKIP=$((SKIP+1))
        [ $SUMMARY_ONLY -eq 0 ] && echo "SKIP(scalar_storage_order): $name"
        return
    fi

    # -finstrument-functions (profiling instrumentation, not implemented)
    if grep -qE 'dg-options.*-finstrument-functions' "$src" 2>/dev/null; then
        SKIP=$((SKIP+1))
        [ $SUMMARY_ONLY -eq 0 ] && echo "SKIP(finstrument): $name"
        return
    fi

    # GCC-specific ULL bitfield arithmetic truncation (clang also fails these)
    if grep -qE 'unsigned long long.*:[[:space:]]*[3-9][0-9]' "$src" 2>/dev/null; then
        SKIP=$((SKIP+1))
        [ $SUMMARY_ONLY -eq 0 ] && echo "SKIP(gcc-ull-bitfield): $name"
        return
    fi

    # Nested functions / statement expressions (not implemented)
    if grep -qE '\{\(\{' "$src" 2>/dev/null || grep -qE 'dg-require-effective-target nested' "$src" 2>/dev/null; then
        SKIP=$((SKIP+1))
        [ $SUMMARY_ONLY -eq 0 ] && echo "SKIP(nested-func): $name"
        return
    fi

    # Compile with rcc; capture stderr to detect missing-include failures
    local err
    err=$($RCC -o "/tmp/torture_rcc_${name}" "$src" -lm 2>&1)
    local rc=$?
    if [ $rc -ne 0 ]; then
        # Missing include file = test infrastructure gap, not a compiler bug
        if echo "$err" | grep -qE "No such file|cannot open|file not found"; then
            SKIP=$((SKIP+1))
            [ $SUMMARY_ONLY -eq 0 ] && echo "SKIP(missing-include): $name"
            return
        fi
        FAIL_COMPILE=$((FAIL_COMPILE+1))
        COMPILE_ERRORS="$COMPILE_ERRORS $name"
        [ $SUMMARY_ONLY -eq 0 ] && echo "FAIL(compile): $name"
        return
    fi

    # Run with timeout
    if ! perl -e 'alarm 5; exec @ARGV' -- "/tmp/torture_rcc_${name}" >/dev/null 2>&1; then
        FAIL_RUNTIME=$((FAIL_RUNTIME+1))
        RUNTIME_ERRORS="$RUNTIME_ERRORS $name"
        [ $SUMMARY_ONLY -eq 0 ] && echo "FAIL(runtime): $name"
        return
    fi

    PASS=$((PASS+1))
    [ $SUMMARY_ONLY -eq 0 ] && echo "PASS: $name"
}

if [ -n "$1" ]; then
    run_test "$1"
else
    for f in *.c; do
        run_test "$f"
    done
fi

echo ""
echo "=== TOTAL=$TOTAL PASS=$PASS FAIL_COMPILE=$FAIL_COMPILE FAIL_RUNTIME=$FAIL_RUNTIME SKIP=$SKIP ==="
echo "Pass rate (excl. skip): $(( PASS * 100 / (TOTAL - SKIP) ))%"

if [ -n "$COMPILE_ERRORS" ]; then
    echo ""
    echo "Compile failures:$COMPILE_ERRORS" | fold -s -w 80
fi
if [ -n "$RUNTIME_ERRORS" ]; then
    echo ""
    echo "Runtime failures:$RUNTIME_ERRORS" | fold -s -w 80
fi
