#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Generate a unified test report for rcc.
# Reads *.summary files produced by each test suite and
# creates a single markdown report.
#
# Usage: ./gen-test-report.sh <platform>
#   platform: linux, mingw_cross, arm64_cross, darwin_cross,
#             arm64 (macOS native), mingw (Windows native)

set -u
cd "$(dirname "$0")" || exit

PLATFORM="${1:-}"
if [ -z "$PLATFORM" ]; then
    case "$(uname -s)" in
        Linux)  PLATFORM="linux" ;;
        Darwin) PLATFORM="arm64" ;;
        MINGW*|MSYS*|CYGWIN*) PLATFORM="mingw" ;;
        *)      PLATFORM="linux" ;;
    esac
fi

case "$PLATFORM" in
    linux)         REPORT="test_report_linux.md"
                   PLATFORM_DESC="Linux x86_64" ;;
    mingw_cross)   REPORT="test_report_mingw_cross.md"
                   PLATFORM_DESC="Windows x86_64 (mingw cross)" ;;
    arm64_cross)   REPORT="test_report_arm64_cross.md"
                   PLATFORM_DESC="Linux ARM64 (aarch64 cross)" ;;
    darwin_cross)  REPORT="test_report_darwin_cross.md"
                   PLATFORM_DESC="macOS ARM64 (darwin cross, compile+link only)" ;;
    arm64)         REPORT="test_report_arm64.md"
                   PLATFORM_DESC="macOS ARM64 (native)" ;;
    mingw)         REPORT="test_report_mingw.md"
                   PLATFORM_DESC="Windows x86_64 (native)" ;;
    *)             echo "Unknown platform: $PLATFORM" >&2; exit 1 ;;
esac

# Source a .summary file if it exists and set variables with prefix
read_summary() {
    _prefix="$1"
    _file="$2"
    [ -f "$_file" ] || return 1
    # Read key=value pairs, prefix variable names
    while IFS='=' read -r key value; do
        [ -z "$key" ] && continue
        eval "${_prefix}_${key}='${value}'"
    done < "$_file"
    return 0
}

have_section() {
    eval "[ -n \"\${$1_TOTAL:-}\" ] && [ \"\${$1_TOTAL:-0}\" -gt 0 ]"
}

# Compute pass rate, handling division by zero
pct() {
    _pass="$1" _total="$2"
    if [ "$_total" -gt 0 ]; then
        echo $((_pass * 100 / _total))
    else
        echo 0
    fi
}

# Write a section for a test suite
# Usage: write_section <prefix> <display_name> [extra_keys...]
# shellcheck disable=SC2154  # _total, _pass etc set via eval
write_section() {
    _p="$1" _name="$2"; shift 2
    eval "_total=\${${_p}_TOTAL:-0}"
    eval "_pass=\${${_p}_PASS:-0}"
    eval "_fail=\${${_p}_FAIL:-0}"
    eval "_skip=\${${_p}_SKIP:-0}"
    eval "_total_no_skip=\$((_total - _skip))"

    printf '## %s\n\n' "$_name"
    printf -- '- **Total**: %d\n' "$_total"
    printf -- '- **Passed**: %d\n' "$_pass"
    printf -- '- **Failed**: %d\n' "$_fail"
    if [ "$_skip" -gt 0 ]; then
        printf -- '- **Skipped**: %d\n' "$_skip"
    fi

    # Extra keys (e.g. FAIL_COMPILE, FAIL_RUNTIME for torture)
    for key in "$@"; do
        eval "_val=\${${_p}_${key}:-0}"
        if [ "$_val" -gt 0 ] 2>/dev/null; then
            _label="$(echo "$key" | tr '_' ' ' | sed 's/\b\(.\)/\u\1/g')"
            printf -- '- **%s**: %d\n' "$_label" "$_val"
        fi
    done

    if [ "$_skip" -gt 0 ]; then
        printf -- '- **Pass Rate (excl. skip)**: %d%%\n' "$(pct "$_pass" "$_total_no_skip")"
    else
        printf -- '- **Pass Rate**: %d%%\n' "$(pct "$_pass" "$_total")"
    fi
}

# Determine which test suites have data
has_tcc=0; has_ctest=0; has_compliance=0; has_torture=0

read_summary TCC "test-tcc-$PLATFORM.summary" && has_tcc=1
read_summary CTEST "test-ctest-$PLATFORM.summary" && has_ctest=1
read_summary COMPLIANCE "test-compliance-$PLATFORM.summary" && has_compliance=1
read_summary TORTURE "test-torture-$PLATFORM.summary" && has_torture=1

# Detect overall totals
overall_total=0
overall_pass=0
overall_fail=0
for p in TCC CTEST COMPLIANCE TORTURE; do
    # shellcheck disable=SC2154
    { eval "_t=\${${p}_TOTAL:-0}"
      eval "_p=\${${p}_PASS:-0}"
      eval "_f=\${${p}_FAIL:-0}"
      overall_total=$((overall_total + _t))
      overall_pass=$((overall_pass + _p))
      overall_fail=$((overall_fail + _f)); }
done

{
    printf '# RCC Test Suite Report\n\n'
    printf '**Platform**: %s\n\n' "$PLATFORM_DESC"
    printf 'Generated: %s\n\n' "$(LC_TIME=C date '+%B %d %Y %H:%M')"

    printf '## Overall Summary\n\n'
    printf -- '- **Total**: %d\n' "$overall_total"
    printf -- '- **Passed**: %d\n' "$overall_pass"
    printf -- '- **Failed**: %d\n' "$overall_fail"
    if [ "$overall_total" -gt 0 ]; then
        printf -- '- **Overall Pass Rate**: %d%%\n' "$(pct "$overall_pass" "$overall_total")"
    fi
    printf '\n'

    if [ "$has_tcc" -eq 1 ]; then
        write_section TCC "TCC Compatibility Tests"
    fi
    if [ "$has_ctest" -eq 1 ]; then
        write_section CTEST "c-testsuite"
    fi
    if [ "$has_compliance" -eq 1 ]; then
        write_section COMPLIANCE "Compliance Tests (vs GCC)"
    fi
    if [ "$has_torture" -eq 1 ]; then
        write_section TORTURE "GCC Torture Tests" FAIL_COMPILE FAIL_RUNTIME
    fi
} > "$REPORT"

printf "Unified report saved to %s\n" "$REPORT"
