#!/bin/sh
cd "$(dirname "$0")" || exit
if [ ! -f c-testsuite/single-exec ]; then
    git submodule update --init --recursive
fi
cd c-testsuite || exit
echo "c-testsuite with ../rcc -O1 -lm"
env CC="../rcc -O1" CFLAGS=-lm ./single-exec posix | scripts/tapsummary | tee ../c-testsuite.tap.txt
