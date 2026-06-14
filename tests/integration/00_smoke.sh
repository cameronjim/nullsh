#!/bin/sh
# Smoke test for the REPL: it starts, it reads, it leaves.
# Usage: 00_smoke.sh <path to nullsh binary>, supplied by make test.

set -u

NULLSH="$1"
tmp=$(mktemp -d)
fail=0

cleanup() { rm -rf "$tmp"; }
trap cleanup EXIT

check() {
    if [ "$2" = "$3" ]; then
        echo "  ok   $1"
    else
        echo "  FAIL $1: expected [$3], got [$2]"
        fail=1
    fi
}

# 1. "exit" leaves quietly with status 0.
# HOME points at the scratch dir so a test run never touches the real history.
printf 'exit\n' | HOME="$tmp" "$NULLSH" >"$tmp/out1" 2>"$tmp/err1"
code=$?
check "exit status" "$code" "0"
check "exit stdout is empty" "$(wc -c <"$tmp/out1" | tr -d ' ')" "0"

# 2. An unknown word is reported on stderr and leaves the shell at 127.
printf 'frobnicate arg1\n' | HOME="$tmp" "$NULLSH" >"$tmp/out2" 2>"$tmp/err2"
code=$?
check "unknown word status" "$code" "127"
check "unknown word stdout is empty" "$(wc -c <"$tmp/out2" | tr -d ' ')" "0"
if grep -q '^nullsh: frobnicate: command not found$' "$tmp/err2"; then
    echo "  ok   unknown word message on stderr"
else
    echo "  FAIL unknown word message on stderr, saw:"
    cat "$tmp/err2"
    fail=1
fi

# 3. Blank lines are ignored and EOF alone exits 0.
printf '\n   \n' | HOME="$tmp" "$NULLSH" >"$tmp/out3" 2>"$tmp/err3"
code=$?
check "blank line status" "$code" "0"
check "blank line stderr is empty" "$(wc -c <"$tmp/err3" | tr -d ' ')" "0"

exit $fail
