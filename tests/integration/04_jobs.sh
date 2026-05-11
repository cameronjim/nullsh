#!/bin/sh
# End to end tests for phase 4 job control without a terminal: & returns at
# once, jobs lists the table, Done lines arrive before the next prompt, fg
# blocks, bg refuses a running job, bogus ids error, and nothing goes zombie.
# Usage: 04_jobs.sh <path to nullsh binary>, supplied by make test.

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

check_below() {
    if [ "$2" -lt "$3" ]; then
        echo "  ok   $1"
    else
        echo "  FAIL $1: $2 ms is not below $3 ms"
        fail=1
    fi
}

check_atleast() {
    if [ "$2" -ge "$3" ]; then
        echo "  ok   $1"
    else
        echo "  FAIL $1: $2 ms is below $3 ms"
        fail=1
    fi
}

ms() { echo $(( $(date +%s%N) / 1000000 )); }

# HOME points at the scratch dir for every run, so the developer's own
# ~/.nullsh_history is never touched.
run() {
    HOME="$tmp" "$NULLSH" >"$tmp/out" 2>"$tmp/err"
}

# 1. The prompt comes back long before the child does.
t0=$(ms)
printf 'sleep 3 &\nexit 0\n' | run
t1=$(ms)
check_below "background returns before the child finishes" $((t1 - t0)) 1000
check_contains "background prints the job id and pgid" "$tmp/err" \
    "^\[1\] [0-9][0-9]*$"

# 2. jobs shows what is running, with the reconstructed command line.
printf 'sleep 2 &\njobs\nexit 0\n' | run
check_contains "jobs lists the job id" "$tmp/out" "^\[1\]"
check_contains "jobs says Running" "$tmp/out" "Running"
check_contains "jobs shows the command" "$tmp/out" "sleep 2 &"

# 3. The Done line is announced before the next command runs.
printf 'sleep 0.2 &\nsleep 0.8\necho after\n' | run
check_contains "finished job announces itself" "$tmp/out" "Done"
check "the Done line comes first" "$(sed -n 1p "$tmp/out" | cut -c1-3)" "[1]"
check "the next command still runs" "$(sed -n 2p "$tmp/out")" "after"

# 4. fg blocks until the job is really gone.
t0=$(ms)
printf 'sleep 1 &\nfg\necho done_fg\n' | run
t1=$(ms)
check_atleast "fg waited for the job" $((t1 - t0)) 900
check_contains "fg came back and the shell kept going" "$tmp/out" "done_fg"

# 5. bg only resumes stopped jobs.
printf 'sleep 2 &\nbg\necho $?\nexit 0\n' | run
check_contains "bg refuses a running job" "$tmp/err" "already running"
check "bg leaves 1 behind" "$(tail -n 1 "$tmp/out")" "1"

# 6. A job id nobody has gets a message, not a crash.
printf 'fg %%99\necho $?\nexit 0\n' | run
check_contains "fg on a bogus id says so" "$tmp/err" "no such job"
check "fg on a bogus id leaves 1" "$(tail -n 1 "$tmp/out")" "1"

printf 'fg\necho $?\nexit 0\n' | run
check_contains "fg with an empty table says so" "$tmp/err" "no current job"
check "fg with an empty table leaves 1" "$(tail -n 1 "$tmp/out")" "1"

# 7. Two background jobs, both reaped and pruned: an unreaped child would keep
# showing up in the table.
printf 'sleep 0.1 &\nsleep 0.1 &\nsleep 0.8\njobs\necho end\n' | run
check "both jobs announced Done" "$(grep -c Done "$tmp/out")" "2"
check "the table is empty afterwards" "$(grep -c Running "$tmp/out")" "0"
check "the shell kept going" "$(tail -n 1 "$tmp/out")" "end"

# 8. No defunct children while the shell sits in a later command.
cat >"$tmp/zombie.nsh" <<'NSH'
sleep 0.2 &
sleep 0.2 &
sleep 0.8
echo ready
sleep 5
NSH
HOME="$tmp" "$NULLSH" <"$tmp/zombie.nsh" >"$tmp/out" 2>"$tmp/err" &
shpid=$!
i=0
while [ $i -lt 60 ] && ! grep -q ready "$tmp/out" 2>/dev/null; do
    sleep 0.1
    i=$((i + 1))
done
# The reap cycle runs between "echo ready" and reading the next line.
sleep 0.4
if ps --ppid $$ -o stat= >/dev/null 2>&1; then
    zombies=$(ps --ppid "$shpid" -o stat= 2>/dev/null | grep -c '^Z')
    check "no zombie children" "$zombies" "0"
else
    echo "  SKIP no zombie children: ps --ppid is not available"
fi
kill "$shpid" 2>/dev/null
wait "$shpid" 2>/dev/null

# 9. A background job in a script, with its output redirected to a file.
printf 'echo from_bg > %s/bgfile &\nsleep 0.6\ncat %s/bgfile\n' "$tmp" "$tmp" \
    | run
check "a redirected background job wrote the file" "$(cat "$tmp/bgfile")" \
    "from_bg"
check "the shell read the file back" "$(tail -n 1 "$tmp/out")" "from_bg"

exit $fail
