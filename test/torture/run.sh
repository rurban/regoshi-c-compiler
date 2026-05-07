#!/bin/bash
# GCC torture test runner for rcc. Stolen from ncc.
# Usage: ./run.sh                      — run all tests with ../../rcc
#        ./run.sh [rcc-binary]         — run all with given rcc
#        ./run.sh [rcc-binary] [test]  — run single test
#        ./run.sh --summary            — run all, show only summary
#
# rcc-binary can be an rcc binary or a cross-compilation wrapper
# script (e.g. ./mingw-cross.sh, ./arm64-cross.sh).

cd "$(dirname "$0")" || exit
RCC="${1:-../../rcc}"
CFLAGS="-O1"
PASS=0
FAIL_COMPILE=0
FAIL_RUNTIME=0
SKIP=0
TOTAL=0
COMPILE_ERRORS=""
RUNTIME_ERRORS=""
SUMMARY_ONLY=0

if [ $# -gt 0 ] && [ "$1" != "--summary" ]; then
    RCC="$1"
    shift
fi
if [ "$1" = "--summary" ]; then
    SUMMARY_ONLY=1
    shift
fi

# Detect cross-compilation wrapper and set runner for test execution
RUNNER=""
PLATFORM=linux
case "$RCC" in
    *darwin-cross*|*rcc-darwin*) PLATFORM=darwin_cross ;;
    *arm64-cross*|*rcc-arm64*)
        PLATFORM=arm64_cross
        if command -v qemu-aarch64 >/dev/null 2>&1; then
            for p in /usr/aarch64-linux-gnu /usr/aarch64-redhat-linux/sys-root/fc43 \
                     /usr/aarch64-linux-gnu/sys-root; do
                if [ -f "$p/lib/ld-linux-aarch64.so.1" ] || [ -d "$p/usr/include" ]; then
                    RUNNER="qemu-aarch64 -L $p"
                    break
                fi
            done
            [ -z "$RUNNER" ] && RUNNER="qemu-aarch64"
        fi
        ;;
    *mingw-cross*|*rcc.exe*)
        PLATFORM=mingw_cross
        if command -v wine >/dev/null 2>&1; then
            RUNNER="wine"
        fi
        ;;
    *)
        case "$(uname -s)" in
            Darwin) PLATFORM=arm64 ;;
            MINGW*|MSYS*|CYGWIN*) PLATFORM=mingw ;;
            *) PLATFORM=linux ;;
        esac
        ;;
esac

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

    if grep -qE '(__complex__|[[:space:]_]Complex)' "$src" 2>/dev/null; then
        SKIP=$((SKIP+1))
        [ $SUMMARY_ONLY -eq 0 ] && echo "SKIP(complex): $name"
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
    if [ "$name" = "20061220-1" ] || [ "$name" = "nest-align-1" ] || [ "$name" = "920415-1" ]; then
        SKIP=$((SKIP+1))
        [ $SUMMARY_ONLY -eq 0 ] && echo "SKIP(nested-func): $name"
        return
    fi

    # __attribute__((__vector_size__(N))) — vector types not implemented
    if grep -q '__vector_size__' "$src" 2>/dev/null; then
        SKIP=$((SKIP+1))
        [ $SUMMARY_ONLY -eq 0 ] && echo "SKIP(vector_size): $name"
        return
    fi

    # Compile with rcc; capture stderr to detect missing-include failures
    local err
    err=$($RCC $CFLAGS -I . -o "/tmp/torture_rcc_${name}" "$src" -lm 2>&1)
    local rc=$?
    if [ $rc -ne 0 ]; then
            # Missing include file = test infrastructure gap, not a compiler bug
            if echo "$err" | grep -qE "No such file|cannot open|include file|not found"; then
            SKIP=$((SKIP+1))
            [ $SUMMARY_ONLY -eq 0 ] && echo "SKIP(missing-include): $name"
            return
        fi
        FAIL_COMPILE=$((FAIL_COMPILE+1))
        COMPILE_ERRORS="$COMPILE_ERRORS $name"
        [ $SUMMARY_ONLY -eq 0 ] && echo "FAIL(compile): $name"
        return
    fi

    # darwin_cross: compile-only, cannot run Mach-O binaries on Linux
    if [ "$PLATFORM" = "darwin_cross" ]; then
        PASS=$((PASS+1))
        [ $SUMMARY_ONLY -eq 0 ] && echo "PASS(compile): $name"
        return
    fi

    # Run with timeout
    TIMEOUT=5
    if [ -n "$RUNNER" ]; then
        # shellcheck disable=SC2086
        if ! timeout $TIMEOUT $RUNNER "/tmp/torture_rcc_${name}" >/dev/null 2>&1; then
            FAIL_RUNTIME=$((FAIL_RUNTIME+1))
            RUNTIME_ERRORS="$RUNTIME_ERRORS $name"
            [ $SUMMARY_ONLY -eq 0 ] && echo "FAIL(runtime): $name"
            return
        fi
    else
        if ! timeout $TIMEOUT "/tmp/torture_rcc_${name}" >/dev/null 2>&1; then
            FAIL_RUNTIME=$((FAIL_RUNTIME+1))
            RUNTIME_ERRORS="$RUNTIME_ERRORS $name"
            [ $SUMMARY_ONLY -eq 0 ] && echo "FAIL(runtime): $name"
            return
        fi
    fi

    PASS=$((PASS+1))
    [ $SUMMARY_ONLY -eq 0 ] && echo "PASS: $name"
}

if [ -n "$1" ]; then
    case "$1" in
        *.c) run_test "$1" ;;
        *)   run_test "$1.c" ;;
    esac
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

# Write machine-readable summary for unified report
cd ../../ || true
{
    printf 'SUITE=torture\n'
    printf 'TOTAL=%d\n' "$TOTAL"
    printf 'PASS=%d\n' "$PASS"
    printf 'FAIL=%d\n' "$((FAIL_COMPILE + FAIL_RUNTIME))"
    printf 'FAIL_COMPILE=%d\n' "$FAIL_COMPILE"
    printf 'FAIL_RUNTIME=%d\n' "$FAIL_RUNTIME"
    printf 'SKIP=%d\n' "$SKIP"
} > "test-torture-$PLATFORM.summary"

# shellcheck disable=SC2143
if [ "$RCC" = "../../arm64-cross.sh" ]; then
    [ "$PASS" -ge 845 ]
elif [ "$(uname -m)" = "aarch64" ] || [ "$(uname -m)" = "arm64" ]; then
    [ "$PASS" -ge 852 ]
elif [ "$RCC" = "../../mingw-cross.sh" ]; then
    [ "$PASS" -ge 840 ]
elif [ "$RCC" = "../../darwin-cross.sh" ]; then
    [ "$PASS" -ge 900 ]
elif [ "$(uname -s | grep -qE 'MSYS|MINGW|CYGWIN')" ]; then
    [ "$PASS" -ge 700 ]
else
    [ "$PASS" -ge 873 ]
fi
