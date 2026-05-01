#!/bin/sh
# Darwin ARM64 cross-compiler wrapper — compiles .c -> Mach-O binary.
# Mirrors arm64-cross.sh API: compiles .c -> .s then assemble+link in container.
#
# Usage: darwin-cross.sh [rcc-options] file.c [file2.c ...] [-o output]
#
# Requires: rcc-darwin built with -D__APPLE__ -DARCH_ARM64
#           gotson/crossbuild:latest container for assemble+link

scriptdir="$(cd "$(dirname "$0")" && pwd)"
RUNTIME="${RUNTIME:-podman}"
IMAGE="docker.io/gotson/crossbuild:latest"

rcc_bin="$scriptdir/rcc-darwin"
if [ ! -x "$rcc_bin" ]; then
    echo "darwin-cross.sh: rcc-darwin not found" >&2
    exit 1
fi

rcc_flags=""
ld_flags=""
inputs=""
output=""

while [ $# -gt 0 ]; do
    case "$1" in
    -o) output="$2"; shift 2 ;;
    -l*|-L*) ld_flags="$ld_flags $1"; rcc_flags="$rcc_flags $1"; shift ;;
    -S) rcc_flags="$rcc_flags -S"; shift ;;
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
    *.c) inputs="$inputs $1"; shift ;;
    *)  inputs="$inputs $1"; shift ;;
    esac
done

inputs="${inputs# }"
[ -z "$inputs" ] && exit 1
[ -z "$output" ] && output="a.out"

# Step 1: rcc-darwin -> .s
TMP_S="$(mktemp /tmp/darwin_cross_XXXXXX.s)"
# shellcheck disable=SC2086
if ! "$rcc_bin" $rcc_flags -S -o "$TMP_S" $inputs; then
    rm -f "$TMP_S"
    exit 1
fi

if echo "$rcc_flags" | grep -q -- "-S"; then
    cp "$TMP_S" "$output"
    rm -f "$TMP_S"
    exit 0
fi

# Step 2: assemble+link in container
outbase="$(basename "$output")"
TMP_EXE_DIR="$(mktemp -d /tmp/darwin_exe_XXXXXX)"

# shellcheck disable=SC2086
if ! $RUNTIME run --rm \
    -v "$(realpath "$TMP_S"):/work/input.s:Z" \
    -v "$TMP_EXE_DIR:/out:Z" \
    "$IMAGE" \
    sh -c "export PATH=/usr/osxcross/bin:\$PATH && aarch64-apple-darwin20.4-clang $ld_flags /work/input.s -o /out/$outbase 2>&1"; then
    rm -rf "$TMP_S" "$TMP_EXE_DIR"
    exit 1
fi

cp "$TMP_EXE_DIR/$outbase" "$output"
chmod +x "$output" 2>/dev/null || true
rm -rf "$TMP_S" "$TMP_EXE_DIR"
exit 0
