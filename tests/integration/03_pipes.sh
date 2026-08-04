#!/bin/sh
# End to end tests for phase 3: pipelines of any length, the four redirects,
# builtins on both sides of a fork, and the failures that must not stop the
# shell.
# Usage: 03_pipes.sh <path to nullsh binary>, supplied by make test.

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

check_absent() {
    if [ -e "$2" ]; then
        echo "  FAIL $1: $2 exists"
        fail=1
    else
        echo "  ok   $1"
    fi
}

# HOME points at the scratch dir for every run, so the developer's own
# ~/.nullsh_history is never touched.
run() {
    HOME="$tmp" "$NULLSH" >"$tmp/out" 2>"$tmp/err"
}

# 1. The smallest pipeline there is.
printf 'echo hi | cat\n' | run
code=$?
check "echo through cat" "$(cat "$tmp/out")" "hi"
check "a pipeline says nothing on stderr" "$(wc -c <"$tmp/err" | tr -d ' ')" "0"
check "shell exits with the last status" "$code" "0"

# 2. A heredoc, so the backslash n reaches printf instead of the outer shell.
run <<'NSH'
printf 'b\na\n' | sort
NSH
check "sort orders what printf wrote" "$(tr '\n' ' ' <"$tmp/out")" "a b "

# 3. Three stages, each one feeding the next.
printf 'printf "one two three" | tr " " "\\n" | sort\n' | run
check "three stages" "$(tr '\n' ' ' <"$tmp/out")" "one three two "

# 4. 2> takes stderr away and leaves stdout untouched.
printf 'sh -c "echo to_out; echo to_err >&2" 2> %s/split_err\n' "$tmp" | run
check "stdout stays clean" "$(cat "$tmp/out")" "to_out"
check "2> captured stderr" "$(cat "$tmp/split_err")" "to_err"
check "the shell's own stderr is quiet" \
    "$(wc -c <"$tmp/err" | tr -d ' ')" "0"

# 5. >> adds to the file where > starts it over.
printf 'echo one > %s/grow\necho two >> %s/grow\n' "$tmp" "$tmp" | run
check "append kept both lines" "$(tr '\n' ' ' <"$tmp/grow")" "one two "
printf 'echo three > %s/grow\n' "$tmp" | run
check "truncation replaced the file" "$(cat "$tmp/grow")" "three"

# 6. < feeds a file to a command that only knows stdin.
printf 'x y z\n' >"$tmp/feed"
printf 'cat < %s/feed\n' "$tmp" | run
check "input redirect feeds the command" "$(cat "$tmp/out")" "x y z"

# 7. A redirected builtin runs in the shell itself: the file lands, and the
# next builtin still sees the history the first one printed.
printf 'echo marker_line\nhistory > %s/hist\nhistory\n' "$tmp" | run
code=$?
check_contains "history wrote the file" "$tmp/hist" "echo marker_line"
check_contains "stdout came back for the next builtin" "$tmp/out" \
    "echo marker_line"
check_contains "the shell kept its history" "$tmp/out" "history > "
check "a redirected builtin succeeds" "$code" "0"

# 8. cd with a redirect still moves the shell, not a child.
printf 'cd %s > /dev/null\npwd\n' "$tmp" | run
check "cd with a redirect still moves the shell" "$(cat "$tmp/out")" \
    "$(cd "$tmp" && pwd -P)"

# 9. A builtin as a pipeline stage runs in a child and its output flows on.
printf 'echo needle\nhistory | grep needle\n' | run
check_contains "a builtin feeds a pipeline stage" "$tmp/out" "echo needle"

# 10. An unknown command mid-pipeline: the shell survives and the last stage
# still decides the status.
printf 'echo a | nsh_no_such_command_here | cat\necho $?\necho still here\n' \
    | run
check_contains "the missing stage reports itself" "$tmp/err" \
    "command not found"
check "the last stage owns the status" "$(sed -n 1p "$tmp/out")" "0"
check "shell survives a missing stage" "$(sed -n 2p "$tmp/out")" "still here"

# 11. The reader leaves first, so the writer takes SIGPIPE and nothing hangs.
printf 'seq 100000 | head -1\n' | run
check "early reader exit gives the first line" "$(cat "$tmp/out")" "1"

# 12. Six stages: one leaked write end here would wait forever.
printf 'echo deep | cat | cat | cat | cat | cat\n' | run
check "six stages deliver the payload" "$(cat "$tmp/out")" "deep"

# 13. A target that cannot be opened errors and the command never runs.
printf 'sh -c "echo hi > %s/ran_anyway" > %s/nosuchdir/target\necho $?\n' \
    "$tmp" "$tmp" | run
check_contains "unopenable target reports itself" "$tmp/err" \
    "No such file or directory"
check "unopenable target leaves 1" "$(sed -n 1p "$tmp/out")" "1"
check_absent "unopenable target ran nothing" "$tmp/ran_anyway"

exit $fail
