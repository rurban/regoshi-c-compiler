#!/bin/sh
# Build rcc-darwin and run the TCC test suite for arm64-darwin.
# Tests compile+link+execute inside the gotson/crossbuild container.
set -e

trap 'make -s clean; make -s' EXIT

echo "==> Building rcc-darwin (host binary, Darwin-targeted codegen)..."
make -s clean
make -s CC=gcc CFLAGS="-std=c11 -Wall -Wextra -O2 -g -D__APPLE__ -DARCH_ARM64" \
    TARGET=rcc-darwin OBJ_EXT=.darwin.o

echo "==> Running TCC test suite via darwin-cross.sh..."
echo ""
./run_tcc_suite.sh ./rcc-darwin

echo "Report saved to tcc_test_darwin_cross.md"
