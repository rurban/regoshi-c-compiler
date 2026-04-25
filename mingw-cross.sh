#!/bin/sh
# Usage: mingw-cross.sh file.c [-o output.exe]
scriptdir="$(cd "$(dirname "$0")" && pwd)"
input="$1"
base="${input%.c}"
shift

# Parse -o option
output=""
while [ $# -gt 0 ]; do
	case "$1" in
	-o)
		output="$2"
		shift 2
		;;
	*) shift ;;
	esac
done

if [ -z "$output" ]; then
	output="$base.exe"
fi
outbase="${output%.exe}"

WINEDEBUG=fixme-all
export WINEDEBUG

wine "$scriptdir/rcc.exe" -S "$input" -o "$outbase.s" &&
	x86_64-w64-mingw32-gcc "$outbase.s" -o "$output" && rm "$outbase.s"
