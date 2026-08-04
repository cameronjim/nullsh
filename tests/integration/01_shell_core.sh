#!/bin/sh
# End to end tests for the phase 1 shell: real commands, expansion, builtins,
# exit status, the background refusal, and history that survives a run.
# Usage: 01_shell_core.sh <path to nullsh binary>, supplied by make test.

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

# HOME points at the scratch dir for every run, so the developer's own
# ~/.nullsh_history is never touched and history persistence is observable.
run() {
    HOME="$tmp" "$NULLSH" >"$tmp/out" 2>"$tmp/err"
}

# 1. A real external command runs and prints.
printf 'echo hi\n' | run
code=$?
check "echo status" "$code" "0"
check "echo output" "$(cat "$tmp/out")" "hi"

# 2. An unknown command reports itself and leaves 127 behind for the next line.
printf 'nsh_no_such_command_here\necho $?\n' | run
code=$?
check_contains "unknown command message" "$tmp/err" "command not found"
check "unknown command leaves 127" "$(cat "$tmp/out")" "127"
check "shell exits with the last status" "$code" "0"

# 3. export then expansion inside double quotes.
printf 'export X=hello\necho "$X world"\n' | run
check "export then expand" "$(cat "$tmp/out")" "hello world"

# 4. Single quotes suppress expansion.
printf "export X=hello\necho '\$X'\n" | run
check "single quotes stay literal" "$(cat "$tmp/out")" '$X'

# 5. cd changes the shell's own directory.
printf 'cd %s\npwd\n' "$tmp" | run
check "cd then pwd" "$(cat "$tmp/out")" "$(cd "$tmp" && pwd -P)"

# 6. exit with an argument sets the shell's status.
printf 'exit 42\n' | run
code=$?
check "exit 42" "$code" "42"

# 7. exit with no argument reuses the last status.
printf 'false\nexit\n' | run
code=$?
check "bare exit after false" "$code" "1"

# 8. A pipe runs both stages and the shell keeps reading after it.
printf 'echo a | cat\necho still here\nexit 3\n' | run
code=$?
check "pipe output" "$(sed -n 1p "$tmp/out")" "a"
check "shell keeps reading after a pipe" "$(sed -n 2p "$tmp/out")" "still here"
check "later exit still controls the status" "$code" "3"

# 9. A redirect creates the file and takes the output out of stdout.
printf 'echo a > %s/redirected\n' "$tmp" | run
check "redirect says nothing on stdout" "$(cat "$tmp/out")" ""
if [ -f "$tmp/redirected" ]; then
    check "redirect wrote the file" "$(cat "$tmp/redirected")" "a"
else
    echo "  FAIL redirect did not create $tmp/redirected"
    fail=1
fi

# 10. Background gets the phase 4 message.
printf 'echo a &\n' | run
check_contains "background refusal names phase 4" "$tmp/err" \
    "job control arrives in phase 4"

# 11. A syntax error is reported and sets 2, and the shell keeps going.
printf 'echo "unclosed\necho $?\n' | run
check_contains "syntax error message" "$tmp/err" "syntax error"
check "syntax error leaves 2" "$(cat "$tmp/out")" "2"

# 12. Quoted arguments reach the program unsplit.
printf 'echo "a   b"\n' | run
check "quoted argument stays one word" "$(cat "$tmp/out")" "a   b"

# 13. History persists: the first run writes the file, the second reads it.
rm -f "$tmp/.nullsh_history"
printf 'echo first_run_line\n' | run
if [ -f "$tmp/.nullsh_history" ]; then
    echo "  ok   history file written"
else
    echo "  FAIL history file not written to $tmp/.nullsh_history"
    fail=1
fi
check_contains "history file holds the typed line" "$tmp/.nullsh_history" \
    "echo first_run_line"

printf 'history\n' | run
check_contains "second run recalls the first run's line" "$tmp/out" \
    "echo first_run_line"

exit $fail
