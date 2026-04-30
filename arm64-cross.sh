#!/bin/sh
# Usage: arm64-cross.sh [rcc-options] file.c [file2.c ...] [-o output]
# Cross-compiles C inputs to aarch64-linux ELF binaries, tested via qemu.
#
# NOTE: This targets Linux ARM64 (ELF), not macOS ARM64 (Mach-O).
# There is no qemu-user for Darwin syscalls.  macOS ARM64 is tested
# natively on the macos-latest CI runner (Apple Silicon).
#
# Run the prebuilt rcc-arm64 under qemu to produce .s files, then
# assemble+link with the aarch64 cross-compiler.  Build rcc-arm64 first:
#   make clean && make CC=aarch64-linux-gnu-gcc
#
# Environment:
#   ARM64_CC       aarch64 cross-compiler (default: aarch64-linux-gnu-gcc)
#   ARM64_QEMU     qemu user-mode runner (default: qemu-aarch64)
#   ARM64_SYSROOT  sysroot for qemu -L and cross-compiler --sysroot

scriptdir="$(cd "$(dirname "$0")" && pwd)"

# --- toolchain detection ---
for cc in "${ARM64_CC:-aarch64-linux-gnu-gcc}" aarch64-redhat-linux-gcc aarch64-linux-gnu-gcc; do
    if command -v "$cc" >/dev/null 2>&1; then
        ARM64_CC="$cc"
        break
    fi
done
ARM64_QEMU="${ARM64_QEMU:-qemu-aarch64}"
ARM64_SYSROOT="${ARM64_SYSROOT:-$("$ARM64_CC" -print-sysroot 2>/dev/null)}"
if [ -z "$ARM64_SYSROOT" ] || [ ! -d "$ARM64_SYSROOT/usr/include" ]; then
    for p in /usr/aarch64-redhat-linux/sys-root/fc43 /usr/aarch64-linux-gnu/sys-root; do
        if [ -d "$p/usr/include" ]; then ARM64_SYSROOT="$p"; break; fi
    done
fi

rcc_bin="$scriptdir/rcc-arm64"
if [ ! -x "$rcc_bin" ]; then
    echo "arm64-cross.sh: rcc-arm64 not found; build it first with:" >&2
    echo "  make clean && make CC=aarch64-linux-gnu-gcc" >&2
    exit 1
fi

rcc_flags=""
ld_flags=""
inputs=""
output=""

while [ $# -gt 0 ]; do
    case "$1" in
    -o)
        output="$2"; shift 2 ;;
    -l*|-L*)
        ld_flags="$ld_flags $1"
        rcc_flags="$rcc_flags $1"
        shift ;;
    -*)
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
        inputs="$inputs $1"; shift ;;
    *)
        inputs="$inputs $1"; shift ;;
    esac
done

inputs="${inputs# }"
if [ -z "$inputs" ]; then
    echo "arm64-cross.sh: no input files" >&2
    exit 1
fi
if [ -z "$output" ]; then
    output="a.out"
fi

s_files=""
for input in $inputs; do
    TMP_S="$(mktemp /tmp/arm64_cross_XXXXXX.s)"
    # shellcheck disable=SC2086
    if ! "$ARM64_QEMU" ${ARM64_SYSROOT:+-L "$ARM64_SYSROOT"} "$rcc_bin" $rcc_flags -S -o "$TMP_S" "$input"; then
        rm -f $s_files
        exit 1
    fi
    s_files="$s_files $TMP_S"
done

# shellcheck disable=SC2086
if [ -n "$ARM64_SYSROOT" ] && [ -d "$ARM64_SYSROOT" ]; then
    "$ARM64_CC" --sysroot="$ARM64_SYSROOT" -no-pie -o "$output" $s_files $ld_flags && rm -f $s_files
else
    "$ARM64_CC" -no-pie -o "$output" $s_files $ld_flags && rm -f $s_files
fi

ret=$?
exit $ret
