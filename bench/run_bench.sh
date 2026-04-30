#!/bin/sh
# RCC vs TCC vs all compilers benchmark (Unix version of run_bench.ps1)
# Usage: ./bench/run_bench.sh [rcc-binary]

set -e

BENCHDIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$BENCHDIR/bench.c"
RCC="${1:-./rcc}"
TCC="${TCC:-tcc}"
GCC="${GCC:-gcc}"
CLANG="$(which clang 2>/dev/null || true)"
KEFIR="$(which kefir 2>/dev/null || true)"
if [ -z "$KEFIR" ] && [ -e "/opt/kefir/bin/kefir" ]; then
   KEFIR="/opt/kefir/bin/kefir"
fi
SLIMCC="$(which slimcc 2>/dev/null || true)"
if [ -z "$SLIMCC" ] && [ -e "$BENCHDIR/../../slimcc/slimcc" ]; then
   SLIMCC="$BENCHDIR/../../slimcc/slimcc"
fi

# Check rcc and tcc exist
if [ ! -x "$RCC" ]; then
    echo "ERROR: rcc not found at '$RCC'. Build it first." >&2
    exit 1
fi
if ! command -v "$TCC" >/dev/null 2>&1; then
    echo "WARNING: tcc not found — skipping TCC benchmark" >&2
    TCC=""
fi

RCC_EXE="$BENCHDIR/bench_rcc"
RCC_O1_EXE="$BENCHDIR/bench_rcc_o1"
TCC_EXE="$BENCHDIR/bench_tcc"
GCC_EXE="$BENCHDIR/bench_gcc"
GCC_O2_EXE="$BENCHDIR/bench_gcc_o2"
CLANG_EXE="$BENCHDIR/bench_clang"
CLANG_O2_EXE="$BENCHDIR/bench_clang_o2"
KEFIR_EXE="$BENCHDIR/bench_kefir"
KEFIR_O1_EXE="$BENCHDIR/bench_kefir_o1"
SLIMCC_EXE="$BENCHDIR/bench_slimcc"

RUNS=3
REPORT="$BENCHDIR/bench_report.md"

cleanup() {
	rm -f "$RCC_EXE" "$RCC_O1_EXE" "$TCC_EXE" "$GCC_EXE" "$GCC_O2_EXE" "$CLANG_EXE" "$CLANG_O2_EXE"
	rm -f "$KEFIR_EXE" "$SLIMCC_EXE"
}
trap cleanup EXIT

# time_ms: prints elapsed ms for a command
# Usage: elapsed=$(time_ms cmd args...)
time_ms() {
	# Use date +%s%N if available (GNU), else fall back to seconds
	if date +%s%N >/dev/null 2>&1; then
		_start=$(date +%s%N)
		"$@" >/dev/null
		_rc=$?
		_end=$(date +%s%N)
		echo $(((_end - _start) / 1000000))
		return $_rc
	else
		_start=$(date +%s)
		"$@" >/dev/null
		_rc=$?
		_end=$(date +%s)
		echo $(((_end - _start) * 1000))
		return $_rc
	fi
}

# run_bench LABEL COMPILER ARGS EXE
run_bench() {
	_label="$1"
	_compiler="$2"
	_args="$3"
	_exe="$4"
	printf "\n--- %s ---\n" "$_label"
        list_c="$list_c|$_label"

	# Compile
        # shellcheck disable=SC2086
	_compile_ms=$(time_ms $_compiler $_args 2>/dev/null) || true
	if [ ! -x "$_exe" ]; then
		printf "  COMPILE FAILED\n"
		return 1
	fi
	printf "  Compile : %6s ms\n" "$_compile_ms"

	# Execute best of N
	_best=1000
	_output=""
	_i=0
	while [ $_i -lt $RUNS ]; do
		_output="$("$_exe")"
		_exec_ms=$(time_ms "$_exe")
		if [ "$_exec_ms" -lt "$_best" ]; then
		    _best=$_exec_ms
		fi
		_i=$((_i + 1))
	done
        # shellcheck disable=SC2086
        if [ $_best = 1000 ]; then
            _best=$_exec_ms
        fi
	printf "  Execute : %6s ms  (best of %d)\n" "$_best" "$RUNS"
	printf "  Total   : %6s ms\n" $((_compile_ms + _best))

	# Store for scoreboard — replace -/space with _ for safe variable names
	_vname="$(echo "${_label%%(*}" | tr ' -' '__')"
	eval "${_vname}_COMPILE=$_compile_ms"
	eval "${_vname}_EXEC=$_best"
	eval "${_vname}_TOTAL=$((_compile_ms + _best))"
	eval "${_vname}_OUTPUT='$_output'"
	rm -f "$_exe"
	return 0
}

list_c=""

echo ""
echo "============================================"
echo "  RCC vs TCC vs others  --  Benchmark Battle"
echo "============================================"

run_bench "RCC" "$RCC" "$SRC -o $RCC_EXE" "$RCC_EXE"
run_bench "RCC -O1" "$RCC" "-O1 $SRC -o $RCC_O1_EXE" "$RCC_O1_EXE"
if [ -n "$TCC" ]; then
    run_bench "TCC" "$TCC" "$SRC -o $TCC_EXE" "$TCC_EXE" || true
fi
if [ -n "$SLIMCC" ]; then
   run_bench "SLIMCC" "$SLIMCC" "$SRC -o $SLIMCC_EXE" "$SLIMCC_EXE" || true
fi
if [ -n "$KEFIR" ]; then
   run_bench "KEFIR" "$KEFIR" "$SRC -o $KEFIR_EXE" "$KEFIR_EXE" || true
   run_bench "KEFIR -O1" "$KEFIR" "-O1 $SRC -o $KEFIR_O1_EXE" "$KEFIR_O1_EXE" || true
fi
run_bench "GCC -O0" "$GCC" "-O0 $SRC -o $GCC_EXE -lm" "$GCC_EXE" || true
run_bench "GCC -O2" "$GCC" "-O2 $SRC -o $GCC_O2_EXE -lm" "$GCC_O2_EXE" || true
if [ -n "$CLANG" ]; then
   run_bench "Clang -O0" "$CLANG" "-O0 $SRC -o $CLANG_EXE -lm" "$CLANG_EXE" || true
   run_bench "Clang -O2" "$CLANG" "-O2 $SRC -o $CLANG_O2_EXE -lm" "$CLANG_O2_EXE" || true
fi

echo ""
echo "============================================="
echo "               SCOREBOARD"
echo "============================================="
printf "%-30s %10s %10s %10s\n" "Compiler" "Compile" "Execute" "Total"
printf "%-30s %10s %10s %10s\n" "--------" "-------" "-------" "-----"
oldifs="$IFS"
IFS='|'
for _c in $list_c; do
	[ -z "$_c" ] && continue
	_vname="$(echo "$_c" | tr ' -' '__')"
	eval "_cm=\${${_vname}_COMPILE:-}"
	eval "_em=\${${_vname}_EXEC:-}"
	eval "_tm=\${${_vname}_TOTAL:-}"
	[ -z "$_cm" ] && continue
        # shellcheck disable=SC2154
	printf "%-30s %8s ms %8s ms %8s ms\n" "$_c" "$_cm" "$_em" "$_tm"
done
IFS="$oldifs"

# Write markdown report
{
	printf "# Linux RCC Benchmark Results\n\n"
	printf "_Generated: %s_\n\n" "$(date '+%B %Y')"
	printf "| %-12s | %12s | %12s | %10s |\n" \
		"Compiler" "Compile (ms)" "Execute (ms)" "Total (ms)"
	printf "| :----------- | -----------: | -----------: | ---------: |\n"
IFS='|'
for _c in $list_c; do
	[ -z "$_c" ] && continue
	_vname="$(echo "$_c" | tr ' -' '__')"
	eval "_cm=\${${_vname}_COMPILE:-}"
	eval "_em=\${${_vname}_EXEC:-}"
	eval "_tm=\${${_vname}_TOTAL:-}"
	[ -z "$_cm" ] && continue
	printf "| %-12s | %12s | %12s | %10s |\n" "$_c" "$_cm" "$_em" "$_tm"
done
IFS="$oldifs"
} > "$REPORT"
printf "Report: %s\n" "$REPORT"

echo ""
echo "ALL DONE"
