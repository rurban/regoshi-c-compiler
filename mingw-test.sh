#!/bin/sh
# Cross-build Windows rcc and run the TCC test suite against it.
set -e
trap './gen-test-report.sh mingw_cross; make clean; make -s' EXIT   # restore host build after cross-test
make clean
make -s CC=x86_64-w64-mingw32-gcc
WINE_DISABLE_RANDR=1
export WINE_DISABLE_RANDR
./run_tcc_suite.sh
test/torture/run.sh ../../mingw-cross.sh
./gen-test-report.sh mingw_cross
