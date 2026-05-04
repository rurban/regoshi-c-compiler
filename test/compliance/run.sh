#!/bin/bash
# Compliance test runner, taken from ncc: compile with both gcc and rcc, compare output
# Usage: ./run.sh [test.c] or ./run.sh (runs all)

cd "$(dirname "$0")" || exit
RCC="../../rcc"
PASS=0
FAIL=0
ERRORS=""

run_test() {
  local src="$1"
  local name
  name="$(basename "$src" .c)"

  # Compile with gcc
  if ! gcc -o "/tmp/compliance_gcc_${name}" "$src" 2>/dev/null; then
    echo "SKIP: $name (gcc compile failed)"
    return
  fi

  # Compile with rcc
  if ! $RCC -o "/tmp/compliance_rcc_${name}" "$src" 2>/dev/null; then
    echo "FAIL: $name (rcc compile failed)"
    FAIL=$((FAIL+1))
    ERRORS="$ERRORS\n  $name: compile error"
    return
  fi

  # Run both
  expected=$("/tmp/compliance_gcc_${name}" 2>&1)
  actual=$("/tmp/compliance_rcc_${name}" 2>&1)

  if [ "$expected" = "$actual" ]; then
    echo "PASS: $name"
    PASS=$((PASS+1))
  else
    echo "FAIL: $name"
    echo "  gcc: $expected"
    echo "  rcc: $actual"
    FAIL=$((FAIL+1))
    ERRORS="$ERRORS\n  $name: output mismatch"
  fi
}

if [ -n "$1" ]; then
  run_test "$1"
else
  for f in *.c; do
    [ -f "$f" ] && run_test "$f"
  done
fi

echo ""
echo "=== PASS=$PASS FAIL=$FAIL ==="
if [ -n "$ERRORS" ]; then
  echo -e "Failures:$ERRORS"
fi
