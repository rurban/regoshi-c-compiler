#!/bin/sh
# Usage: mingw-cross.sh [rcc-options] file.c [file2.c ...] [-o output.exe]
# Wraps rcc.exe (Windows PE) under Wine for cross-compilation.
#
# Passes all non -o options through to rcc's -S invocation, then links
# the resulting .s files with x86_64-w64-mingw32-gcc.

scriptdir="$(cd "$(dirname "$0")" && pwd)"

rcc_flags=""
inputs=""
output=""

while [ $# -gt 0 ]; do
	case "$1" in
	-o)
		output="$2"
		shift 2
		;;
	-*)
		# rcc option: collect for passing through
		rcc_flags="$rcc_flags $1"
		if [ $# -gt 1 ]; then
			case "$1" in
			-o|-I|-L|-D|-U) rcc_flags="$rcc_flags $2"; shift 2 ;;
			*) shift ;;
			esac
		else
			shift
		fi
		;;
	*.c|*.s)
		inputs="$inputs $1"
		shift
		;;
	*)
		# Assume it's an input file
		inputs="$inputs $1"
		shift
		;;
	esac
done

if [ -z "$inputs" ]; then
	echo "mingw-cross.sh: no input files" >&2
	exit 1
fi

# Derive output name from first input if not specified
first_src="${inputs%% *}"
first_base="${first_src%.c}"
if [ -z "$output" ]; then
	output="$(basename "$first_base.exe")"
fi

WINEDEBUG=fixme-all
WINEDLLOVERRIDES="winedbg=d"
WINENOPOPUPS=1
export WINEDEBUG WINEDLLOVERRIDES WINENOPOPUPS

s_files=""
for input in $inputs; do
	TMP_S="$(mktemp -u /tmp/mingw_cross_XXXXXX.s)"
	# shellcheck disable=SC2086
	if ! wine "$scriptdir/rcc.exe" $rcc_flags -S -o "$TMP_S" "$input"; then
		rm -f $s_files
		exit 1
	fi
	s_files="$s_files $TMP_S"
done

# shellcheck disable=SC2086
x86_64-w64-mingw32-gcc -o "$output" $s_files && rm -f $s_files
