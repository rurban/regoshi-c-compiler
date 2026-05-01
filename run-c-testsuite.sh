#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
cd "$(dirname "$0")" || exit
if [ ! -f c-testsuite/single-exec ]; then
    git submodule update --init --recursive
fi

TO_TMP=""
if ! command -v timeout >/dev/null 2>&1; then
    if command -v gtimeout >/dev/null 2>&1; then
        TO_TMP=$(mktemp -d)
        ln -s "$(command -v gtimeout)" "$TO_TMP/timeout"
        export PATH="$TO_TMP:$PATH"
    fi
fi

cd c-testsuite || exit
echo "c-testsuite with ../rcc -O1 -lm"
env CC="../rcc" CFLAGS="-O1 -lm" ./single-exec posix | scripts/tapsummary | tee ../c-testsuite.tap.txt

MAX_FAILS=2
fails=$(grep -m1 '^fail ' ../c-testsuite.tap.txt | awk '{print $2}')
if [ -z "$fails" ]; then
    echo "ERROR: could not determine test fail count"
    exit 1
fi
if [ "$fails" -gt "$MAX_FAILS" ]; then
    echo "FAIL: got $fails failures, maximum allowed is $MAX_FAILS"
    exit 1
fi
echo "OK: $fails failures, within limit of $MAX_FAILS"

if [ -n "$TO_TMP" ]; then
    rm -rf "$TO_TMP"
fi
