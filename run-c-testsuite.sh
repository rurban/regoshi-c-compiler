#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
cd "$(dirname "$0")" || exit

# c-testsuite not yet supported on macOS (arm64-darwin)
if [ "$(uname -s)" = "Darwin" ]; then
    echo "SKIP: c-testsuite not yet supported on macOS"
    exit 0
fi

if [ ! -f c-testsuite/single-exec ]; then
    git submodule update --init --recursive
fi
if [ ! -f c-testsuite/fred.txt ] && command -v git >/dev/null 2>&1; then
    cd c-testsuite && git clean -dxf
fi

TO_TMP=""
if ! command -v timeout >/dev/null 2>&1; then
    if command -v gtimeout >/dev/null 2>&1; then
        TO_TMP=$(mktemp -d)
        ln -s "$(command -v gtimeout)" "$TO_TMP/timeout"
        export PATH="$TO_TMP:$PATH"
    else
        # no timeout available, emulate with a wrapper
        TO_TMP=$(mktemp -d)
        cat > "$TO_TMP/timeout" << 'WRAPPER'
#!/bin/sh
dur="${1%s}"; shift
"$@" &
pid=$!
(sleep "$dur" && kill -TERM "$pid" 2>/dev/null && sleep 1 && kill -KILL "$pid" 2>/dev/null) &
wait "$pid"
WRAPPER
        chmod +x "$TO_TMP/timeout"
        export PATH="$TO_TMP:$PATH"
    fi
fi

cd c-testsuite || exit
echo "c-testsuite with ../rcc -O1 -lm"
env CC="../rcc" CFLAGS="-O1 -lm" ./single-exec posix | scripts/tapsummary | tee ../c-testsuite.tap.txt

# TODO: 00204
MAX_FAILS=1
if [ "$(uname -s)" = "Darwin" ]; then
    echo "SKIP: failure limit not enforced on macOS"
    exit 0
fi

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
