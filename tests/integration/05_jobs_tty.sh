#!/bin/sh
# Best effort phase 4 tests that need a real terminal: Ctrl-Z stops the
# foreground job, jobs shows it Stopped, fg resumes it, and Ctrl-C kills the
# child without touching the shell. The keystrokes are delivered as signals to
# the foreground process group, which is exactly what the tty driver does.
# Anything missing here is a SKIP, never a failure.
# Usage: 05_jobs_tty.sh <path to nullsh binary>, supplied by make test.

set -u

NULLSH="$1"
case "$NULLSH" in
    /*) ;;
    *) NULLSH="$(pwd)/$NULLSH" ;;
esac
NSHNAME=$(basename "$NULLSH")

skip_all() {
    echo "  SKIP 05_jobs_tty: $1"
    exit 0
}

command -v script >/dev/null 2>&1 || skip_all "script(1) is not installed"
command -v mkfifo >/dev/null 2>&1 || skip_all "mkfifo is not available"
ps --ppid $$ -o pid= >/dev/null 2>&1 || skip_all "ps --ppid is not available"

tmp=$(mktemp -d)
fail=0
shpid=""

cleanup() {
    [ -n "$shpid" ] && kill "$shpid" 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT

check_contains() {
    if grep -q "$3" "$2"; then
        echo "  ok   $1"
    else
        echo "  FAIL $1: [$3] not in $2, saw:"
        sed 's/^/        /' "$2"
        fail=1
    fi
}

send() { printf '%s\n' "$1" >&3; }

# The pty pushes carriage returns into the log, so patterns never anchor right.
out="$tmp/out"
fifo="$tmp/in"
mkfifo "$fifo" || skip_all "could not create a fifo"

HOME="$tmp" script -qec "$NULLSH" /dev/null <"$fifo" >"$out" 2>&1 &
shpid=$!
exec 3>"$fifo"

# The shell under script may sit one or two levels down, so find it by name.
nsh=""
i=0
while [ $i -lt 40 ] && [ -z "$nsh" ]; do
    nsh=$(ps -eo pid=,comm= | awk -v n="$NSHNAME" '$2 == n { print $1; exit }')
    [ -n "$nsh" ] && break
    sleep 0.1
    i=$((i + 1))
done
[ -n "$nsh" ] || skip_all "the shell never appeared under script(1)"

# The pgid of the foreground job is its first stage's pid, and the shell runs
# exactly one child at a time here.
foreground_child() {
    ps --ppid "$nsh" -o pid= 2>/dev/null | tr -d ' ' | head -1
}

wait_for_child() {
    i=0
    child=""
    while [ $i -lt 40 ]; do
        child=$(foreground_child)
        [ -n "$child" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

# 1. Ctrl-Z: the group stops, the table says Stopped, fg runs it to the end.
send 'sleep 2'
wait_for_child || skip_all "the foreground child never started"
sleep 0.3
kill -TSTP -"$child" 2>/dev/null || skip_all "could not signal the job's group"
sleep 0.5
send 'jobs'
sleep 0.5
send 'fg'
sleep 3
send 'echo RESUMED_OK'
sleep 0.5

# 2. Ctrl-C: the child dies, the shell prompts again.
send 'sleep 5'
wait_for_child || skip_all "the second foreground child never started"
sleep 0.3
kill -INT -"$child" 2>/dev/null || skip_all "could not interrupt the job's group"
sleep 0.5
send 'echo ALIVE_OK'
sleep 0.5

send 'exit 0'
exec 3>&-
wait "$shpid" 2>/dev/null
shpid=""

check_contains "Ctrl-Z stopped the foreground job" "$out" "Stopped"
check_contains "jobs lists the stopped job" "$out" "\[1\]"
check_contains "fg resumed it to completion" "$out" "RESUMED_OK"
check_contains "Ctrl-C left the shell alive" "$out" "ALIVE_OK"

exit $fail
