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

inputs="${inputs# }"

if [ -z "$inputs" ]; then
	echo "mingw-cross.sh: no input files" >&2
	exit 1
fi

if [ -z "$output" ]; then
	output="a.exe"
fi

WINEDEBUG=fixme-all
WINEDLLOVERRIDES="winedbg=d"
WINENOPOPUPS=1
export WINEDEBUG WINEDLLOVERRIDES WINENOPOPUPS

# Ensure libwinpthread-1.dll is available for Wine (needed by mingw-w64 CRT)
if [ ! -f "$HOME/.wine/drive_c/windows/system32/libwinpthread-1.dll" ]; then
    cp /usr/x86_64-w64-mingw32/sys-root/mingw/bin/libwinpthread-1.dll \
       "$HOME/.wine/drive_c/windows/system32/" 2>/dev/null || true
fi

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
x86_64-w64-mingw32-gcc -o "$output" $s_files "$scriptdir/lib/mingw.o" && rm -f $s_files
