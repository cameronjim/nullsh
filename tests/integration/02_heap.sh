#!/bin/sh
# End to end tests for the heap builtin: stats, the strategy switch, the arena
# dump, and the two error shapes that must not stop the shell.
# Usage: 02_heap.sh <path to nullsh binary>, supplied by make test.

set -u

NULLSH="$1"
case "$NULLSH" in
    /*) ;;
    *) NULLSH="$(pwd)/$NULLSH" ;;
esac

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

check_contains() {
    if grep -q "$3" "$2"; then
        echo "  ok   $1"
    else
        echo "  FAIL $1: [$3] not in $2, saw:"
        sed 's/^/        /' "$2"
        fail=1
    fi
}

check_matches() {
    if grep -qE "$3" "$2"; then
        echo "  ok   $1"
    else
        echo "  FAIL $1: [$3] does not match a line of $2, saw:"
        sed 's/^/        /' "$2"
        fail=1
    fi
}

# HOME points at the scratch dir for every run, so the developer's own
# ~/.nullsh_history is never touched.
run() {
    HOME="$tmp" "$NULLSH" >"$tmp/out" 2>"$tmp/err"
}

# make test sets the boot strategy; a bare run boots on firstfit.
boot="${NSH_ALLOC_STRATEGY:-firstfit}"

# 1. heap stats labels its rows and names the strategy the shell booted with.
printf 'heap stats\necho $?\n' | run
check_contains "stats has a strategy row" "$tmp/out" "^strategy"
check_contains "stats names the boot strategy" "$tmp/out" "^strategy  *$boot\$"
check_contains "stats reports the arena size" "$tmp/out" "^arena size"
check_contains "stats reports used bytes" "$tmp/out" "^used bytes"
check_contains "stats reports the largest free block" "$tmp/out" "^largest free"
check_contains "stats counts mallocs" "$tmp/out" "^total mallocs"
check_contains "stats status is 0" "$tmp/out" "^0\$"

# 2. A bare heap is the same as heap stats.
printf 'heap\n' | run
check_contains "bare heap prints stats" "$tmp/out" "^strategy"

# 3. The strategy switch takes effect for the rest of the session.
printf 'heap strategy buddy\nheap strategy\n' | run
check "strategy switch prints nothing but the new name" "$(cat "$tmp/out")" "buddy"
check "strategy switch says nothing on stderr" \
    "$(wc -c <"$tmp/err" | tr -d ' ')" "0"

# 4. Stats follow the switch, and the shell still allocates after it.
printf 'heap strategy buddy\necho warm\nheap stats\n' | run
check_contains "stats follow the switch" "$tmp/out" "^strategy  *buddy\$"

# 5. The dump is an address-ordered map of used and free regions.
printf 'heap dump\necho $?\n' | run
check_matches "dump has a used or free line" "$tmp/out" "(FREE|USED)"
check_contains "dump status is 0" "$tmp/out" "^0\$"

# 6. An unknown subcommand explains itself and the shell keeps reading.
printf 'heap frobnicate\necho $?\necho still here\n' | run
code=$?
check_contains "unknown subcommand prints a usage line" "$tmp/err" \
    "^nullsh: heap: usage: heap "
check "unknown subcommand leaves 1" "$(sed -n 1p "$tmp/out")" "1"
check "shell survives an unknown subcommand" "$(sed -n 2p "$tmp/out")" \
    "still here"
check "shell exits with the last status" "$code" "0"

# 7. An unknown strategy names the two that exist and changes nothing.
printf 'heap strategy slab\necho $?\nheap strategy\n' | run
check_contains "unknown strategy message" "$tmp/err" \
    "^nullsh: heap: unknown strategy slab (firstfit, buddy)\$"
check "unknown strategy leaves 1" "$(sed -n 1p "$tmp/out")" "1"
check "unknown strategy keeps the active one" "$(sed -n 2p "$tmp/out")" "$boot"

# 8. A refused switch says nothing on stdout.
printf 'heap strategy slab\n' | run
check "unknown strategy prints nothing on stdout" \
    "$(wc -c <"$tmp/out" | tr -d ' ')" "0"

exit $fail
