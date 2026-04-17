#!/bin/sh
# Run the TCC compatibility test suite against rcc.
# Usage: ./run_tcc_suite.sh [rcc-binary] [test-dir]
#
# Compiles each tcc_tests/*.c file with rcc, runs it, and diffs the output
# against the corresponding *.expect file.  Tests without an .expect file
# pass as long as the program exits successfully.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RCC="${1:-}"
TEST_DIR="${2:-$SCRIPT_DIR/tcc_tests}"
REPORT_FILE="$SCRIPT_DIR/tcc_test_report.md"

# Locate rcc binary
if [ -z "$RCC" ]; then
    for candidate in "$SCRIPT_DIR/rcc" "$SCRIPT_DIR/rcc.exe"; do
        if [ -x "$candidate" ]; then
            RCC="$candidate"
            break
        fi
    done
fi

if [ -z "$RCC" ] || [ ! -x "$RCC" ]; then
    echo "rcc binary not found. Build it first or pass the path as \$1." >&2
    exit 1
fi

if [ ! -d "$TEST_DIR" ]; then
    echo "Test directory not found: $TEST_DIR" >&2
    exit 1
fi

# Terminal colours (fall back gracefully when not a tty)
if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'
    CYAN='\033[0;36m'; GRAY='\033[0;37m'; RESET='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; GRAY=''; RESET=''
fi

printf "${CYAN}Starting TCC Test Suite on RCC...${RESET}\n"
printf "RCC:      %s\n" "$RCC"
printf "Test dir: %s\n\n" "$TEST_DIR"

total=0; passed=0; failed=0
report_rows=""

# Helper: tests with special runtime arguments
test_args() {
    case "$1" in
        31_args)  printf '%s' "arg1 arg2 arg3 arg4 arg5" ;;
        *)        printf '' ;;
    esac
}

TMPDIR="${TMPDIR:-/tmp}"
TMP_OUT="$TMPDIR/rcc_test_$$.out"
TMP_EXE="$TMPDIR/rcc_test_$$"
trap 'rm -f "$TMP_OUT" "$TMP_EXE"' EXIT INT TERM

# Iterate over all *.c files; skip helper files containing '+' in the name
for src in "$TEST_DIR"/*.c; do
    fname="$(basename "$src")"
    case "$fname" in *+*) continue ;; esac   # skip multi-file helpers

    base="${fname%.c}"
    total=$((total + 1))

    printf "  %-40s " "$base..."

    # 1. Compile (capture warnings/notes to TMP_OUT; errors abort)
    if ! "$RCC" "$src" -o "$TMP_EXE" 2>"$TMP_OUT"; then
        printf "${RED}COMPILE FAIL${RESET}\n"
        failed=$((failed + 1))
        report_rows="${report_rows}| $base | COMPILE_FAIL | rcc returned non-zero |\n"
        continue
    fi
    if [ ! -x "$TMP_EXE" ]; then
        printf "${RED}NO EXE PRODUCED${RESET}\n"
        failed=$((failed + 1))
        report_rows="${report_rows}| $base | COMPILE_FAIL | executable missing |\n"
        continue
    fi

    # 2. Execute (append runtime output after compile warnings)
    args="$(test_args "$base")"
    if [ -n "$args" ]; then
        # shellcheck disable=SC2086
        if ! "$TMP_EXE" $args >>"$TMP_OUT" 2>&1; then
            printf "${RED}EXEC FAIL${RESET}\n"
            failed=$((failed + 1))
            report_rows="${report_rows}| $base | EXEC_FAIL | non-zero exit |\n"
            rm -f "$TMP_EXE"
            continue
        fi
    else
        if ! "$TMP_EXE" >>"$TMP_OUT" 2>&1; then
            printf "${RED}EXEC FAIL${RESET}\n"
            failed=$((failed + 1))
            report_rows="${report_rows}| $base | EXEC_FAIL | non-zero exit |\n"
            rm -f "$TMP_EXE"
            continue
        fi
    fi
    rm -f "$TMP_EXE"

    # 3. Verify
    expect_file="$TEST_DIR/$base.expect"
    if [ -f "$expect_file" ]; then
        # Normalise line endings before diff
        actual="$(tr -d '\r' <"$TMP_OUT")"
        expected="$(tr -d '\r' <"$expect_file")"
        if [ "$actual" = "$expected" ]; then
            printf "${GREEN}PASS${RESET}\n"
            passed=$((passed + 1))
            report_rows="${report_rows}| $base | PASS | Output matches |\n"
        else
            printf "${YELLOW}MISMATCH${RESET}\n"
            failed=$((failed + 1))
            report_rows="${report_rows}| $base | MISMATCH | Output does not match .expect |\n"
            cp "$TMP_OUT" "$TEST_DIR/$base.out"
        fi
    else
        printf "${GRAY}PASS (no expect)${RESET}\n"
        passed=$((passed + 1))
        report_rows="${report_rows}| $base | PASS | Executed successfully (no .expect) |\n"
    fi
done

# Run tests in test/ directory
UNIT_TEST_DIR="$SCRIPT_DIR/test"
if [ -d "$UNIT_TEST_DIR" ]; then
    printf "\n${CYAN}Unit tests (test/)...${RESET}\n"

    # Tests expected to fail compilation (compile error is the correct outcome)
    expect_compile_fail() {
        case "$1" in test_err) return 0 ;; esac
        return 1
    }

    # Tests to skip (benchmarks, logs)
    skip_test() {
        case "$1" in benchmark|primes) return 0 ;; esac
        return 1
    }

    for src in "$UNIT_TEST_DIR"/*.c; do
        fname="$(basename "$src")"
        base="${fname%.c}"

        skip_test "$base" && continue

        total=$((total + 1))
        printf "  %-40s " "$base..."

        if expect_compile_fail "$base"; then
            if "$RCC" "$src" -o "$TMP_EXE" >/dev/null 2>&1; then
                printf "${RED}SHOULD FAIL (compiled ok)${RESET}\n"
                failed=$((failed + 1))
                report_rows="${report_rows}| $base | FAIL | expected compile error but succeeded |\n"
                rm -f "$TMP_EXE"
            else
                printf "${GREEN}PASS (expected compile error)${RESET}\n"
                passed=$((passed + 1))
                report_rows="${report_rows}| $base | PASS | compile error as expected |\n"
            fi
            continue
        fi

        if ! "$RCC" "$src" -o "$TMP_EXE" >/dev/null 2>&1; then
            printf "${RED}COMPILE FAIL${RESET}\n"
            failed=$((failed + 1))
            report_rows="${report_rows}| $base | COMPILE_FAIL | rcc returned non-zero |\n"
            continue
        fi
        if [ ! -x "$TMP_EXE" ]; then
            printf "${RED}NO EXE PRODUCED${RESET}\n"
            failed=$((failed + 1))
            report_rows="${report_rows}| $base | COMPILE_FAIL | executable missing |\n"
            continue
        fi

        if ! "$TMP_EXE" >"$TMP_OUT" 2>&1; then
            printf "${RED}EXEC FAIL${RESET}\n"
            failed=$((failed + 1))
            report_rows="${report_rows}| $base | EXEC_FAIL | non-zero exit |\n"
            rm -f "$TMP_EXE"
            continue
        fi
        rm -f "$TMP_EXE"

        printf "${GREEN}PASS${RESET}\n"
        passed=$((passed + 1))
        report_rows="${report_rows}| $base | PASS | Executed successfully |\n"
    done
fi

rm -f "$TMP_OUT"

# Summary
echo ""
if [ "$total" -gt 0 ]; then
    pct=$((passed * 100 / total))
else
    pct=0
fi
printf "${CYAN}Results: %d/%d passed (%d%%), %d failed.${RESET}\n" \
    "$passed" "$total" "$pct" "$failed"

# Markdown report
{
    printf '# TCC Test Suite Report for RCC\n'
    printf 'Generated: %s\n\n' "$(date)"
    printf '## Summary\n'
    printf -- ' - **Total**: %d\n' "$total"
    printf -- ' - **Passed**: %d\n' "$passed"
    printf -- ' - **Failed**: %d\n' "$failed"
    printf -- ' - **Pass Rate**: %d%%\n\n' "$pct"
    printf '## Detailed Results\n'
    printf '| Test | Status | Message |\n'
    printf '| --- | --- | --- |\n'
    printf '%b' "$report_rows"
} >"$REPORT_FILE"

printf "Report saved to %s\n" "$REPORT_FILE"

[ "$failed" -eq 0 ]
