#!/bin/sh
# Cross-build ARM64 rcc and run the TCC test suite against it.
set -e
trap 'make clean; make -s' EXIT   # restore host build after cross-test
make clean
make -s CC=aarch64-linux-gnu-gcc
./run_tcc_suite.sh ./rcc-arm64
