#!/bin/sh
# Smoke test for the phase 0 REPL stub.
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
printf 'exit\n' | "$NULLSH" >"$tmp/out1" 2>"$tmp/err1"
code=$?
check "exit status" "$code" "0"
check "exit stdout is empty" "$(wc -c <"$tmp/out1" | tr -d ' ')" "0"

# 2. An unknown word reports the phase 1 message on stderr, still status 0.
printf 'frobnicate arg1\n' | "$NULLSH" >"$tmp/out2" 2>"$tmp/err2"
code=$?
check "unknown word status" "$code" "0"
check "unknown word stdout is empty" "$(wc -c <"$tmp/out2" | tr -d ' ')" "0"
if grep -q '^nullsh: frobnicate: command execution arrives in phase 1$' "$tmp/err2"; then
    echo "  ok   unknown word message on stderr"
else
    echo "  FAIL unknown word message on stderr, saw:"
    cat "$tmp/err2"
    fail=1
fi

# 3. Blank lines are ignored and EOF alone exits 0.
printf '\n   \n' | "$NULLSH" >"$tmp/out3" 2>"$tmp/err3"
code=$?
check "blank line status" "$code" "0"
check "blank line stderr is empty" "$(wc -c <"$tmp/err3" | tr -d ' ')" "0"

exit $fail
