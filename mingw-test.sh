#!/bin/sh
# Cross-build Windows rcc and run the TCC test suite against it.
set -e
trap 'make clean; make -s' EXIT   # restore host build after cross-test
make clean
make -s CC=x86_64-w64-mingw32-gcc
./run_tcc_suite.sh
