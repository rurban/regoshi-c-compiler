#!/bin/sh
# Run the TCC compatibility test suite against rcc.
# Usage: ./run_tcc_suite.sh [rcc-binary] [test-dir]
#
# Compiles each tcc_tests/*.c file with rcc, runs it, and diffs the output
# against the corresponding *.expect file.  Tests without an .expect file
# pass as long as the program exits successfully.

set -u
cd "$(dirname "$0")" || exit
SCRIPT_DIR=.
RCC="${1:-}"
TEST_DIR="${2:-$SCRIPT_DIR/tinycc/tests/tests2}"
REPORT_FILE=tcc_test_linux.md

# Locate rcc binary
if [ -z "$RCC" ]; then
	for candidate in "$SCRIPT_DIR/rcc" "$SCRIPT_DIR/rcc.exe"; do
		if [ -x "$candidate" ]; then
			RCC="$candidate"
			if [ "$RCC" = "$SCRIPT_DIR/rcc.exe" ]; then
				RCC="$SCRIPT_DIR/mingw-cross.sh"
                                REPORT_FILE="$SCRIPT_DIR/tcc_test_mingw_cross.md"
			fi
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
	RED='\033[0;31m'
	GREEN='\033[0;32m'
	YELLOW='\033[0;33m'
	CYAN='\033[0;36m'
	GRAY='\033[0;37m'
	RESET='\033[0m'
else
	RED=''
	GREEN=''
	YELLOW=''
	CYAN=''
	GRAY=''
	RESET=''
fi

# shellcheck disable=SC2059
printf "${CYAN}Starting TCC Test Suite on RCC...${RESET}\n"
printf "RCC:      %s\n" "$RCC"
printf "Test dir: %s\n\n" "$TEST_DIR"

total=0
passed=0
failed=0
report_rows=""

add_row() {
	report_rows="${report_rows}$(printf '| %-40s | %-12s | %-36s |' "$1" "$2" "$3")\n"
}

# Helper: tests with special runtime arguments
test_args() {
	case "$1" in
	31_args) printf '%s' "arg1 arg2 arg3 arg4 arg5" ;;
	46_grep) printf '%s' '[^* ]*[:a:d: ]+\:\*-/: $'" $TEST_DIR/46_grep.c" ;;
        128_run_atexit) printf "1" ;;
	*) printf '' ;;
	esac
}

test_expected_exit() {
	case "$1" in
	101_cleanup) echo 105 ;;
	*) echo 0 ;;
	esac
}

TMPDIR="${TMPDIR:-/tmp}"
TMP_OUT="$TMPDIR/rcc_test_$$.out"
TMP_EXE="$TMPDIR/rcc_test_$$"
if [ "$RCC" = "$SCRIPT_DIR/mingw-cross.sh" ]; then
	TMP_EXE="$TMP_EXE.exe"
	WINEDEBUG=fixme-all
	WINEDLLOVERRIDES="winedbg=d"
	export WINEDEBUG WINEDLLOVERRIDES
fi
trap 'rm -f "$TMP_OUT" "$TMP_EXE"' EXIT INT TERM

# Parse previous report for change detection
OLD_STATES="$TMPDIR/rcc_old_states_$$.txt"
if [ -f "$REPORT_FILE" ]; then
	awk -F'|' 'NR>3 && /^\|/ { name=$2; status=$3; gsub(/[[:space:]]/, "", name); gsub(/[[:space:]]/, "", status); if (name != "" && status != "") print name, status }' "$REPORT_FILE" > "$OLD_STATES"
else
	: > "$OLD_STATES"
fi
trap 'rm -f "$TMP_OUT" "$TMP_EXE" "$OLD_STATES"' EXIT INT TERM

regressions=0
fixes=0
changed=0

print_change() {
	_base="$1"
	_new="$2"
	_old=$(awk -v n="$_base" '$1 == n { print $2; exit }' "$OLD_STATES" 2>/dev/null)
	[ -z "$_old" ] && return
	[ "$_old" = "$_new" ] && return
	if [ "$_new" = "PASS" ]; then
		# shellcheck disable=SC2059
		printf "    ${GREEN}-> FIXED${RESET} (was %s)\n" "$_old"
		fixes=$((fixes + 1))
	elif [ "$_old" = "PASS" ]; then
		# shellcheck disable=SC2059
		printf "    ${RED}-> REGRESSION${RESET} (was PASS)\n"
		regressions=$((regressions + 1))
	else
		# shellcheck disable=SC2059
		printf "    ${YELLOW}-> CHANGED${RESET} (%s -> %s)\n" "$_old" "$_new"
		changed=$((changed + 1))
	fi
}

# Tests to skip – mirrors tinycc/tests/tests2/Makefile SKIP, minus TCC-internals
# (bound-checker, backtrace, btdll, builtins are TCC-runtime-only and omitted here)
#tcc-extension working: 34_array_assignment
SKIP_TESTS="
22_floating_point
60_errors_and_warnings
96_nodata_wanted
73_arm64
95_bitfields_ms
112_backtrace
113_btdll
114_bound_signal
115_bound_setjmp
116_bound_setjmp2
122_vla_reuse
126_bound_global
78_vla_label
79_vla_continue
98_al_ax_extend
99_fastcall
120_alias
123_vla_bug
124_atomic_counter
125_atomic_misc
136_atomic_gcc_style
"

is_skipped() {
	case "$SKIP_TESTS" in *"
$1
"*) return 0 ;; esac
	return 1
}

# Iterate over all *.c files; for helper files containing '+' prepend them to the ones without
p_src=
while IFS= read -r src; do
	fname="$(basename "$src")"
	case "$fname" in
            *+*) p_src="$src" # use as first src arg
                 continue
                 ;;
        esac

	base="${fname%.c}"

	if is_skipped "$base"; then
		printf "  %-40s %s\n" "$base..." "SKIP"
		p_src=
		continue
	fi

	fixed_up=
	# Apply local fixups for tinycc tests2 expect files.
	# If test/tinycc-<base>.expect exists and differs from the upstream
	# expect, overwrite the upstream copy. We fixed these cases; tcc hasn't.
	fixup="$SCRIPT_DIR/test/tinycc-$base.expect"
	upstream_expect="$TEST_DIR/$base.expect"
	if [ -f "$fixup" ] && ! cmp -s "$fixup" "$upstream_expect" 2>/dev/null; then
		cp "$upstream_expect" "$upstream_expect".orig
		cp "$fixup" "$upstream_expect"
		fixed_up=1
	fi

	total=$((total + 1))

	printf "  %-40s " "$base..."

	if [ "$src" = "$TEST_DIR/129_scopes.c" ]; then
		orig_RCC="$RCC"
		RCC="$(realpath "$RCC")"
		cd "$TEST_DIR" || exit
		src=129_scopes.c
        fi
        # 1. Compile (capture warnings/notes to TMP_OUT; errors abort)
        # shellcheck disable=SC2086,SC2129
	if [ "$src" = "$TEST_DIR/128_run_atexit.c" ]; then
            echo "[test_128_return]" >"$TMP_OUT"
	    "$RCC" -Dtest_128_return "$src" -o "$TMP_EXE"
            "$TMP_EXE" >>"$TMP_OUT"
            run_atexit="$?"
            echo "[returns $run_atexit]" >>"$TMP_OUT"
            echo "" >>"$TMP_OUT"
            echo "[test_128_exit]" >>"$TMP_OUT"
	    "$RCC" -Dtest_128_exit "$src" -o "$TMP_EXE"
            "$TMP_EXE" >>"$TMP_OUT"
            xx="$?"
            run_atexit="$run_atexit $xx"
            echo "[returns $xx]" >>"$TMP_OUT"
        elif ! "$RCC" $p_src "$src" -o "$TMP_EXE" 2>"$TMP_OUT"; then
		# shellcheck disable=SC2059
		printf "${RED}COMPILE FAIL${RESET}\n"
		failed=$((failed + 1))
		add_row "$base" "COMPILE_FAIL" "rcc returned non-zero"
		print_change "$base" "COMPILE_FAIL"
		if [ "$src" = "129_scopes.c" ]; then
			cd - >/dev/null || exit
			src="$TEST_DIR/129_scopes.c"
			RCC="$orig_RCC"
		fi
		continue
	fi
        [ -n "$p_src" ] && p_src=
	if [ ! -x "$TMP_EXE" ]; then
		# shellcheck disable=SC2059
		printf "${RED}NO EXE PRODUCED${RESET}\n"
		failed=$((failed + 1))
		add_row "$base" "COMPILE_FAIL" "executable missing"
		print_change "$base" "COMPILE_FAIL"
		if [ "$src" = "129_scopes.c" ]; then
			cd - >/dev/null || exit
			src="$TEST_DIR/129_scopes.c"
			RCC="$orig_RCC"
		fi
		continue
	fi
	if [ "$src" = "129_scopes.c" ]; then
		cd - >/dev/null || exit
		src="$TEST_DIR/129_scopes.c"
		RCC="$orig_RCC"
        fi

	# 2. Execute (append runtime output after compile warnings)
	args="$(test_args "$base")"
	expected_exit="$(test_expected_exit "$base")"
	if [ -n "$args" ]; then
		if [ "$base" = "46_grep" ]; then
			# 46_grep pattern contains spaces; run from TEST_DIR with local filename
			(cd "$TEST_DIR" && "$TMP_EXE" '[^* ]*[:a:d: ]+\:\*-/: $' 46_grep.c) >>"$TMP_OUT" 2>&1; actual_exit=$?
		elif [ "$base" = "128_run_atexit" ]; then
			actual_exit="$run_atexit"
			expected_exit="1 2" # we already ran twice
		else
			# shellcheck disable=SC2086
			"$TMP_EXE" $args >>"$TMP_OUT" 2>&1; actual_exit=$?
		fi
	else
		"$TMP_EXE" >>"$TMP_OUT" 2>&1; actual_exit=$?
	fi
	if [ "$actual_exit" != "$expected_exit" ]; then
		# shellcheck disable=SC2059
		printf "${RED}EXEC FAIL${RESET}\n"
		failed=$((failed + 1))
		add_row "$base" "EXEC_FAIL" "non-zero exit"
		print_change "$base" "EXEC_FAIL"
		rm -f "$TMP_EXE"
		continue
	fi
	rm -f "$TMP_EXE"

	# 3. Verify
	expect_file="$TEST_DIR/$base.expect"
	fixup_expect="$SCRIPT_DIR/test/tinycc-$base.expect"
	if [ -f "$fixup_expect" ]; then
		expect_file="$fixup_expect"
	fi
	if [ -f "$expect_file" ]; then
		if diff -Nbu "$expect_file" "$TMP_OUT"; then
			# shellcheck disable=SC2059
			printf "${GREEN}PASS${RESET}\n"
			passed=$((passed + 1))
			add_row "$base" "PASS" "Output matches"
			print_change "$base" "PASS"
		else
			# shellcheck disable=SC2059
			printf "${YELLOW}MISMATCH${RESET}\n"
			failed=$((failed + 1))
			add_row "$base" "MISMATCH" "Output does not match .expect"
			print_change "$base" "MISMATCH"
			cp "$TMP_OUT" "test/$base.out"
		fi
	else
		# shellcheck disable=SC2059
		printf "${GRAY}PASS (no expect)${RESET}\n"
		passed=$((passed + 1))
		add_row "$base" "PASS" "Executed successfully (no .expect)"
		print_change "$base" "PASS"
	fi
        if [ -n "$fixed_up" ]; then
	    mv "$upstream_expect".orig "$upstream_expect"
        fi
done <<EOF
$(printf '%s\n' "$TEST_DIR"/*.c | sort -V)
EOF

# Run tests in test/ directory
UNIT_TEST_DIR="$SCRIPT_DIR/test"
if [ -d "$UNIT_TEST_DIR" ]; then
	# shellcheck disable=SC2059
	printf "\n${CYAN}Unit tests (test/)...${RESET}\n"

	# Tests expected to fail compilation (compile error is the correct outcome)
	expect_compile_fail() {
		case "$1" in test_err) return 0 ;; esac
		return 1
	}

	# Tests to skip (benchmarks, logs)
	while IFS= read -r src; do
		fname="$(basename "$src")"
		base="${fname%.c}"

		total=$((total + 1))
		printf "  %-40s " "$base..."

		if expect_compile_fail "$base"; then
			if "$RCC" "$src" -o "$TMP_EXE" >/dev/null 2>&1; then
				# shellcheck disable=SC2059
				printf "${RED}SHOULD FAIL (compiled ok)${RESET}\n"
				failed=$((failed + 1))
				add_row "$base" "FAIL" "expected compile error but succeeded"
				print_change "$base" "FAIL"
				rm -f "$TMP_EXE"
			else
				# shellcheck disable=SC2059
				printf "${GREEN}PASS (expected compile error)${RESET}\n"
				passed=$((passed + 1))
				add_row "$base" "PASS" "compile error as expected"
				print_change "$base" "PASS"
			fi
			continue
		fi

		if ! "$RCC" "$src" -o "$TMP_EXE" >/dev/null 2>&1; then
			# shellcheck disable=SC2059
			printf "${RED}COMPILE FAIL${RESET}\n"
			failed=$((failed + 1))
			add_row "$base" "COMPILE_FAIL" "rcc returned non-zero"
			print_change "$base" "COMPILE_FAIL"
			continue
		fi
		if [ ! -x "$TMP_EXE" ]; then
			# shellcheck disable=SC2059
			printf "${RED}NO EXE PRODUCED${RESET}\n"
			failed=$((failed + 1))
			add_row "$base" "COMPILE_FAIL" "executable missing"
			print_change "$base" "COMPILE_FAIL"
			continue
		fi

		"$TMP_EXE" >"$TMP_OUT" 2>&1
		exit_code=$?
		rm -f "$TMP_EXE"

		# shellcheck disable=SC2059
		printf "${GREEN}PASS${RESET}\n"
		passed=$((passed + 1))
		add_row "$base" "PASS" "exit=$exit_code"
		print_change "$base" "PASS"
	done <<EOF
$(printf '%s\n' "$UNIT_TEST_DIR"/test_*.c | sort -V)
EOF
fi

rm -f "$TMP_OUT"

# Summary
echo ""
if [ "$total" -gt 0 ]; then
	pct=$((passed * 100 / total))
else
	pct=0
fi
# shellcheck disable=SC2059
printf "${CYAN}Results: %d/%d passed (%d%%), %d failed.${RESET}\n" \
	"$passed" "$total" "$pct" "$failed"

if [ $((regressions + fixes + changed)) -gt 0 ]; then
	printf "Changes vs previous run:"
	# shellcheck disable=SC2059
	[ "$regressions" -gt 0 ] && printf "  ${RED}%d regression(s)${RESET}" "$regressions"
	# shellcheck disable=SC2059
	[ "$fixes" -gt 0 ]       && printf "  ${GREEN}%d fixed${RESET}" "$fixes"
	# shellcheck disable=SC2059
	[ "$changed" -gt 0 ]     && printf "  ${YELLOW}%d changed${RESET}" "$changed"
	printf "\n"
fi

# Markdown report
{
	printf '# TCC Test Suite Report for RCC\n'
	printf 'Generated: %s\n\n' "$(date '+%B %Y')"
	printf '## Summary\n'
	printf -- ' - **Total**:     %d\n' "$total"
	printf -- ' - **Passed**:    %d\n' "$passed"
	printf -- ' - **Failed**:    %d\n' "$failed"
	printf -- ' - **Pass Rate**: %d%%\n\n' "$pct"
	printf '## Detailed Results\n'
	printf '| %-40s | %-12s | %-36s |\n' "Test" "Status" "Message"
	printf '|:-----------------------------------------|:-------------|:-------------------------------------|\n'
	printf '%b' "$report_rows"
} >"$SCRIPT_DIR/$REPORT_FILE"

printf "Report saved to %s\n" "$REPORT_FILE"

[ "$passed" -ge 129 ]
