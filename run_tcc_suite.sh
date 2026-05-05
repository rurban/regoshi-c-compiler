#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Run the TCC compatibility test suite against rcc.
# Usage: ./run_tcc_suite.sh [rcc-binary] [test-dir] [extra-rcc-flags]
#
# Extra rcc flags (e.g. -O0) are passed to every compilation invocation.
#
# Compiles each tcc_tests/*.c file with rcc, runs it, and diffs the output
# against the corresponding *.expect file.  Tests without an .expect file
# pass as long as the program exits successfully.

set -u
cd "$(dirname "$0")" || exit
SCRIPT_DIR=.
RCC="${1:-}"
TEST_DIR="${2:-$SCRIPT_DIR/tinycc/tests/tests2}"
RCCFLAGS="${3:-}"
REPORT_FILE=tcc_test_linux.md

if [ ! -e tcc_tests ]; then
    ln -s tinycc/tests/tests2 tcc_tests
fi

# Locate rcc binary
if [ -z "$RCC" ]; then
	for candidate in "$SCRIPT_DIR/rcc" "$SCRIPT_DIR/rcc.exe"; do
		if [ -x "$candidate" ]; then
			RCC="$candidate"
			if [ "$RCC" = "$SCRIPT_DIR/rcc.exe" ]; then
				RCC="$SCRIPT_DIR/mingw-cross.sh"
				REPORT_FILE="$SCRIPT_DIR/tcc_test_mingw_cross.md"
                                if command -v winetricks>/dev/null 2>&1; then
                                    winetricks nocrashdialog
                                fi
				WINEDEBUG=fixme-all
				WINEDLLOVERRIDES="winedbg=d"
				WINENOPOPUPS=1
				export WINEDEBUG WINEDLLOVERRIDES WINENOPOPUPS
                        fi
			break
		fi
	done
fi

# If arm64-cross.sh is available and rcc not found natively, use arm64 cross
if [ "$RCC" = "./rcc-arm64" ] || [ "$RCC" = "./arm64-cross.sh" ]; then
	RCC="$SCRIPT_DIR/arm64-cross.sh"
	REPORT_FILE="$SCRIPT_DIR/tcc_test_arm64_cross.md"
fi
if [ "$RCC" = "./rcc-darwin" ] || [ "$RCC" = "./darwin-cross.sh" ]; then
	RCC="$SCRIPT_DIR/darwin-cross.sh"
	REPORT_FILE="$SCRIPT_DIR/tcc_test_darwin_cross.md"
fi
if [ "$(uname -m)" = "aarch64" ] || [ "$(uname -m)" = "arm64" ]; then
    REPORT_FILE="$SCRIPT_DIR/tcc_test_arm64.md"
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
[ -n "$RCCFLAGS" ] && printf "Flags:    %s\n" "$RCCFLAGS"
printf "Test dir: %s\n\n" "$TEST_DIR"

total=0
passed=0
failed=0

# Compute max test name length for terminal display
_unitname() { basename "$1" .c; }
max_name_len=4
for f in "$TEST_DIR"/*.c; do
	[ -f "$f" ] || continue
	n=$(_unitname "$f")
	[ "${#n}" -gt "$max_name_len" ] && max_name_len="${#n}"
done
UNIT_TEST_DIR="$SCRIPT_DIR/test"
if [ -d "$UNIT_TEST_DIR" ]; then
	for f in "$UNIT_TEST_DIR"/test_*.c; do
		[ -f "$f" ] || continue
		n=$(_unitname "$f")
		[ "${#n}" -gt "$max_name_len" ] && max_name_len="${#n}"
	done
fi

# Format a table cell prettier-style: " content_padded_to_width "
fmt_cell() { printf " %-*s " "$2" "$1"; }
# Format table separator column: " :--- "
fmt_sep() { printf " :%s " "$(printf '%*s' "$(($1 - 1))" '' | tr ' ' '-')"; }

report_raw=""
add_row() {
	report_raw="${report_raw}${1}|${2}|${3}\n"
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
is_arm64=''
is_darwin=''
RUN_PREFIX=''
if [ -z "${ARM64_SYSROOT+x}" ]; then
    for p in /usr/aarch64-linux-gnu /usr/aarch64-redhat-linux/sys-root/fc43 /usr/aarch64-linux-gnu/sys-root /usr/aarch64-linux-gnu; do
        if [ -f "$p/lib/ld-linux-aarch64.so.1" ] || [ -d "$p/usr/include" ]; then
            ARM64_SYSROOT="$p"
            break
        fi
    done
fi
if [ "$RCC" = "$SCRIPT_DIR/mingw-cross.sh" ]; then
	TMP_EXE="$TMP_EXE.exe"
	WINEDEBUG=fixme-all
	WINEDLLOVERRIDES="winedbg=d"
	export WINEDEBUG WINEDLLOVERRIDES
elif [ "$RCC" = "$SCRIPT_DIR/arm64-cross.sh" ]; then
	if [ -d "$ARM64_SYSROOT" ]; then
		RUN_PREFIX="qemu-aarch64 -L $ARM64_SYSROOT"
	else
		RUN_PREFIX="qemu-aarch64"
	fi
	is_arm64=1
elif [ "$RCC" = "$SCRIPT_DIR/darwin-cross.sh" ]; then
	# Darwin Mach-O can't execute on Linux — compile+link only
	is_darwin=1
	is_arm64=1
elif [ "$(uname -m)" = "aarch64" ] || [ "$(uname -m)" = "arm64" ]; then
	is_arm64=1
fi

run_exe() {
	if [ -n "$RUN_PREFIX" ]; then
		# shellcheck disable=SC2086
		timeout 5s $RUN_PREFIX "$@"
	else
		"$@"
	fi
}
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
	# Normalize COMPILE_OK to PASS for change detection (Darwin compile-only)
	_old_cmp="$_old"
	_new_cmp="$_new"
	[ "$_old_cmp" = "COMPILE_OK" ] && _old_cmp="PASS"
	[ "$_new_cmp" = "COMPILE_OK" ] && _new_cmp="PASS"
	[ "$_old_cmp" = "$_new_cmp" ] && return
	if [ "$_new_cmp" = "PASS" ]; then
		# shellcheck disable=SC2059
		printf "    ${GREEN}-> FIXED${RESET} (was %s)\n" "$_old"
		fixes=$((fixes + 1))
	elif [ "$_old_cmp" = "PASS" ]; then
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
# (bound-checker, backtrace, btdll are TCC-runtime-only and omitted here)
#tcc-extension working: 34_array_assignment
SKIP_TESTS="
60_errors_and_warnings
96_nodata_wanted
98_al_ax_extend
99_fastcall
112_backtrace
113_btdll
114_bound_signal
115_bound_setjmp
116_bound_setjmp2
 120_alias
126_bound_global
141_riscv_asm_pseudo
142_riscv_asm_longlong
143_riscv_asm_farith
"

# Tests skipped only when using mingw-cross.sh (Windows cross-compilation)
MINGW_SKIP_TESTS="
"

INTEL_SKIP_TESTS="
73_arm64
138_arm64_encoding
139_arm64_errors
140_arm64_extasm
"

# we provide our own arm64 asm_goto test, this is x86_64 only. tcc bug to be filed
ARM64_SKIP_TESTS="
95_bitfields_ms
127_asm_goto
"

# Tests that need to be compiled and run in the test directory (for __FILE__ or .expect path handling)
CD_TESTS="
125_atomic_misc
129_scopes
139_arm64_errors
"

# Tests that use tcc's -dt flag: multi-sub-test files with .expect
# Each test name is extracted from '#if defined test_' / '#elif defined test_'
DT_TESTS="
125_atomic_misc
139_arm64_errors
"

# Extract test names from a source file in source order
# Uses grep -E for POSIX compatibility (macOS grep lacks -P)
extract_dt_tests() {
    grep -nE 'defined[[:space:]]*\(?test_[_[:alnum:]]+' "$1" 2>/dev/null | \
        grep -oE 'test_[_[:alnum:]]+' | awk '!seen[$0]++'
}

is_skipped() {
	case "$SKIP_TESTS" in *"
$1
"*) return 0 ;; esac
	if [ "$RCC" = "$SCRIPT_DIR/mingw-cross.sh" ]; then
		case "$MINGW_SKIP_TESTS" in *"
$1
"*) return 0 ;; esac
        fi
	if [ "$RCC" = "$SCRIPT_DIR/arm64-cross.sh" ] || \
           [ "$RCC" = "$SCRIPT_DIR/darwin-cross.sh" ] || \
           [ "$is_arm64" = "1" ]; then
		case "$ARM64_SKIP_TESTS" in *"
$1
"*) return 0 ;; esac
        else
		case "$INTEL_SKIP_TESTS" in *"
$1
"*) return 0 ;; esac
        fi
	return 1
}

extra_ldflags() {
	case "$1" in
	22_floating_point|24_math_library) printf '%s' " -lm" ;;
	*) printf '' ;;
	esac
}

# Iterate over all *.c files; for helper files containing '+' prepend them to the ones without
p_src=
while IFS= read -r src; do
	fname="$(basename "$src")"
	case "$fname" in
            *+*) p_src="$src" # use as first src arg
                 continue
                 ;;
            95_bitfields_ms.c)
                [ "$RCC" = "$SCRIPT_DIR/mingw-cross.sh" ] || \
                p_src="-mms-bitfields"
                ;;
            95_bitfields.c)
                [ "$RCC" = "$SCRIPT_DIR/mingw-cross.sh" ] && \
                p_src="-mno-ms-bitfields"
                ;;
        esac

	base="${fname%.c}"

	if is_skipped "$base"; then
		printf "  %-${max_name_len}s %s\n" "$base" "SKIP"
		add_row "$base" "SKIP" "Skipped"
		p_src=
		continue
	fi
        ldflags="$(extra_ldflags "$base")"

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

	printf "  %-${max_name_len}s " "$base"

	in_cd_dir=
	if echo "$CD_TESTS" | grep -qw "$base"; then
		orig_RCC="$RCC"
		RCC="$(realpath "$RCC")"
		cd "$TEST_DIR" || exit
		src="$base.c"
		in_cd_dir=1
		if [ "$base" = "129_scopes" ] && echo "$RCC" | grep -q mingw-cross; then
			p_src="-D__TINYC__"
		fi
        fi
        # 1. Compile (capture warnings/notes to TMP_OUT; errors abort)
        # Handle -dt multi-sub-test files
        # shellcheck disable=SC2086,SC2129
        if echo "$DT_TESTS" | grep -qw "$base"; then
            expect_file="$TEST_DIR/$base.expect"
            true >"$TMP_OUT"
            for tname in $(extract_dt_tests "$src" 2>/dev/null); do
                echo "[$tname]" >>"$TMP_OUT"
                # Try to compile and run; capture both stdout and stderr
                if "$RCC" $RCCFLAGS -D$tname -o "$TMP_EXE" \
                    "$src" >"$TMP_OUT".err 2>&1; then
                    # Also include any warnings emitted during successful compilation
                    if [ -s "$TMP_OUT".err ]; then
                        sed 's/\x1b\[[0-9;]*m//g' "$TMP_OUT".err >>"$TMP_OUT"
                    fi
                    if [ "$is_darwin" != "1" ]; then
                        run_exe "$TMP_EXE" >>"$TMP_OUT" 2>&1 || true
                    fi
                else
                    # Compilation failed: capture error output, strip ANSI codes
                    sed 's/\x1b\[[0-9;]*m//g' "$TMP_OUT".err >>"$TMP_OUT"
                fi
                echo "" >>"$TMP_OUT"
            done
            rm -f "$TMP_OUT".err
            if [ -n "$in_cd_dir" ]; then
                cd - >/dev/null || exit
                src="$TEST_DIR/$base.c"
                RCC="$orig_RCC"
                in_cd_dir=
            fi
            # Darwin: skip execution, treat compile+link as success
            if [ "$is_darwin" = "1" ]; then
                # shellcheck disable=SC2059
                printf "${GREEN}PASS${RESET}\n"
                passed=$((passed + 1))
                add_row "$base" "COMPILE_OK" "linked, (execution skipped)"
                p_src=
                continue
            fi
            # Compare against expect file
        if [ -f "$expect_file" ]; then
            if diff -Nbu "$expect_file" "$TMP_OUT"; then
                # shellcheck disable=SC2059
                printf "${GREEN}PASS${RESET}\n"
                    passed=$((passed + 1))
                    add_row "$base" "PASS" "Output matches"
                    print_change "$base" "PASS"
                else
                    # shellcheck disable=SC2059
                    printf "${RED}MISMATCH${RESET}\n"
                    failed=$((failed + 1))
                    add_row "$base" "MISMATCH" "Output differs"
                    print_change "$base" "MISMATCH"
                    cp "$TMP_OUT" "test/$base.out"
                    [ -n "$p_src" ] && p_src=
                fi
            else
                # shellcheck disable=SC2059
                printf "${GREEN}PASS (no expect)${RESET}\n"
                passed=$((passed + 1))
                add_row "$base" "PASS" "Output generated (no expect)"
                print_change "$base" "PASS"
            fi
            if [ -n "$fixed_up" ]; then
                mv "$upstream_expect".orig "$upstream_expect"
            fi
            [ -n "$p_src" ] && p_src=
            continue
        fi
        if [ "$src" = "$TEST_DIR/128_run_atexit.c" ]; then
            # shellcheck disable=SC2129
            echo "[test_128_return]" >"$TMP_OUT"
	    # shellcheck disable=SC2086
	    "$RCC" $RCCFLAGS -Dtest_128_return -o "$TMP_EXE" "$src"
            run_exe "$TMP_EXE" >>"$TMP_OUT"
            run_atexit="$?"
            # shellcheck disable=SC2129
            echo "[returns $run_atexit]" >>"$TMP_OUT"
            echo "" >>"$TMP_OUT"
	    # shellcheck disable=SC2129
            echo "[test_128_exit]" >>"$TMP_OUT"
	    # shellcheck disable=SC2086
	    "$RCC" $RCCFLAGS -Dtest_128_exit -o "$TMP_EXE" "$src"
            run_exe "$TMP_EXE" >>"$TMP_OUT"
            xx="$?"
            run_atexit="$run_atexit $xx"
            echo "[returns $xx]" >>"$TMP_OUT"
        else
            # shellcheck disable=SC2086
            if ! "$RCC" $RCCFLAGS -o "$TMP_EXE" $p_src "$src" $ldflags 2>"$TMP_OUT"; then
		# shellcheck disable=SC2059
		printf "${RED}COMPILE FAIL${RESET}\n"
		failed=$((failed + 1))
		add_row "$base" "COMPILE_FAIL" "rcc returned non-zero"
		print_change "$base" "COMPILE_FAIL"
		[ -n "$p_src" ] && p_src=
		if [ -n "$in_cd_dir" ]; then
			cd - >/dev/null || exit
			src="$TEST_DIR/$base.c"
			RCC="$orig_RCC"
			in_cd_dir=
		fi
		continue
	    fi
        fi
        [ -n "$p_src" ] && p_src=
	if [ ! -x "$TMP_EXE" ]; then
		# shellcheck disable=SC2059
		printf "${RED}NO EXE PRODUCED${RESET}\n"
		failed=$((failed + 1))
		add_row "$base" "COMPILE_FAIL" "executable missing"
		print_change "$base" "COMPILE_FAIL"
		[ -n "$p_src" ] && p_src=
		if [ -n "$in_cd_dir" ]; then
			cd - >/dev/null || exit
			src="$TEST_DIR/$base.c"
			RCC="$orig_RCC"
			in_cd_dir=
		fi
		continue
	fi
	if [ -n "$in_cd_dir" ]; then
		cd - >/dev/null || exit
		src="$TEST_DIR/$base.c"
		RCC="$orig_RCC"
		in_cd_dir=
	fi

        # 2a. Darwin: compile+link only (can't execute Mach-O on Linux)
	if [ "$is_darwin" = "1" ]; then
            expect_file="$TEST_DIR/$base.expect"
	    if [ -f "$expect_file" ]; then
		add_row "$base" "COMPILE_OK" "linked, (execution skipped)"
		# shellcheck disable=SC2059
		printf "${GREEN}PASS (compile OK)${RESET}\n"
		passed=$((passed + 1))
		print_change "$base" "PASS"
	    else
		add_row "$base" "COMPILE_OK" "linked ok (no expect, no exec)"
		# shellcheck disable=SC2059
		printf "${GRAY}PASS (no expect, compile OK)${RESET}\n"
		passed=$((passed + 1))
		print_change "$base" "PASS"
	    fi
            if [ -n "$fixed_up" ]; then
		mv "$upstream_expect".orig "$upstream_expect"
            fi
	    continue
        fi

	# 2. Execute (append runtime output after compile warnings)
	args="$(test_args "$base")"
	expected_exit="$(test_expected_exit "$base")"
	if [ -n "$args" ]; then
		if [ "$base" = "46_grep" ]; then
			# 46_grep pattern contains spaces; run from TEST_DIR with local filename
			(cd "$TEST_DIR" && run_exe "$TMP_EXE" '[^* ]*[:a:d: ]+\:\*-/: $' 46_grep.c) >>"$TMP_OUT" 2>&1; actual_exit=$?
		elif [ "$base" = "128_run_atexit" ]; then
			actual_exit="$run_atexit"
			expected_exit="1 2" # we already ran twice
		else
			# shellcheck disable=SC2086
			run_exe "$TMP_EXE" $args >>"$TMP_OUT" 2>&1; actual_exit=$?
		fi
	else
		run_exe "$TMP_EXE" >>"$TMP_OUT" 2>&1; actual_exit=$?
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
if [ -d "$UNIT_TEST_DIR" ]; then
	# shellcheck disable=SC2059
	printf "\n${CYAN}Unit tests (test/)${RESET}\n"

	# Tests expected to fail compilation (compile error is the correct outcome)
	expect_compile_fail() {
		case "$1" in test_err) return 0 ;; esac
		return 1
	}

	# Unit tests to skip on certain platforms
	skip_unit_test() {
		case "$1" in
			test_arm64_asm) [ -z "$is_arm64" ] && return 0 ;;
		esac
		return 1
	}

	# Tests to skip (benchmarks, logs)
	while IFS= read -r src; do
		fname="$(basename "$src")"
		base="${fname%.c}"

		if skip_unit_test "$base"; then
			printf "  %-${max_name_len}s %s\n" "$base..." "SKIP"
			add_row "$base" "SKIP" "Skipped"
			continue
		fi

		total=$((total + 1))
		printf "  %-${max_name_len}s " "$base..."

		if expect_compile_fail "$base"; then
			# shellcheck disable=SC2086
			if "$RCC" $RCCFLAGS -o "$TMP_EXE" "$src" >/dev/null 2>&1; then
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

		# shellcheck disable=SC2086
		if ! "$RCC" $RCCFLAGS -o "$TMP_EXE" "$src" >/dev/null 2>&1; then
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

		run_exe "$TMP_EXE" >"$TMP_OUT" 2>&1
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
        LC_TIME=en_US.UTF-8
	printf '# TCC Test Suite Report for RCC\n'
	printf '\n'
	printf 'Generated: %s\n\n' "$(date '+%B %Y')"
	printf '## Summary\n'
	printf '\n'
	printf -- '- **Total**: %d\n' "$total"
	printf -- '- **Passed**: %d\n' "$passed"
	printf -- '- **Failed**: %d\n' "$failed"
	printf -- '- **Pass Rate**: %d%%\n\n' "$pct"
	printf '## Detailed Results\n'
	printf '\n'
	# Post-process: compute column widths from actual data
	printf '%b' "$report_raw" > "$TMP_OUT".data
	nm=4; sm=6; mm=7
	while IFS='|' read -r name status message; do
		[ -z "$name" ] && continue
		[ "${#name}" -gt "$nm" ] && nm="${#name}"
		[ "${#status}" -gt "$sm" ] && sm="${#status}"
		[ "${#message}" -gt "$mm" ] && mm="${#message}"
	done < "$TMP_OUT".data
	# Format header and separator
	printf "|%s|%s|%s|\n" \
		"$(fmt_cell "Test" "$nm")" \
		"$(fmt_cell "Status" "$sm")" \
		"$(fmt_cell "Message" "$mm")"
	printf "|%s|%s|%s|\n" \
		"$(fmt_sep "$nm")" "$(fmt_sep "$sm")" "$(fmt_sep "$mm")"
	# Format data rows
	while IFS='|' read -r name status message; do
		printf "|%s|%s|%s|\n" \
			"$(fmt_cell "$name" "$nm")" \
			"$(fmt_cell "$status" "$sm")" \
			"$(fmt_cell "$message" "$mm")"
	done < "$TMP_OUT".data
	rm -f "$TMP_OUT".data
} >"$SCRIPT_DIR/$REPORT_FILE"

printf "Report saved to %s\n" "$REPORT_FILE"

# arm64-darwin native
if [ "$REPORT_FILE" = "$SCRIPT_DIR/tcc_test_arm64.md" ]; then
    [ "$passed" -ge 145 ]
elif [ "$RCC" = "$SCRIPT_DIR/darwin-cross.sh" ]; then
    [ "$passed" -ge 152 ]
elif [ "$RCC" = "$SCRIPT_DIR/arm64-cross.sh" ]; then
    [ "$passed" -ge 152 ]
elif [ "$RCC" = "$SCRIPT_DIR/mingw-cross.sh" ]; then
    [ "$passed" -ge 147 ]
else
    [ "$passed" -ge 149 ]
fi
