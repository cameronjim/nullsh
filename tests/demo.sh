#!/bin/sh
# Replays the demo transcript in README.md against a built nullsh and diffs the
# result, so the README cannot rot without failing. Deliberately not under
# tests/integration/, because make test must not depend on the README.
# Usage: demo.sh [path to nullsh binary], default build/nullsh.

set -u

NULLSH="${1:-build/nullsh}"
case "$NULLSH" in
    /*) ;;
    *) NULLSH="$(pwd)/$NULLSH" ;;
esac

if [ ! -x "$NULLSH" ]; then
    echo "demo: $NULLSH is not there, run make first"
    exit 1
fi

# The transcript drives these through the shell, so a missing one is a skip and
# not a failure: nothing about nullsh has rotted if the box lacks coreutils.
for tool in tr wc printf head grep sleep; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "demo: SKIP, $tool is not installed"
        exit 0
    }
done
[ -r /usr/bin/ls ] || {
    echo "demo: SKIP, /usr/bin/ls is not readable and the transcript inspects it"
    exit 0
}

tmp=$(mktemp -d) || exit 1
cleanup() { rm -rf "$tmp"; }
trap cleanup EXIT INT TERM

# HOME is the scratch dir so the developer's ~/.nullsh_history is untouched, and
# the strategy is forced because the transcript shows firstfit at the start.
run_transcript() {
    cd "$tmp" || exit 1
    HOME="$tmp" NSH_ALLOC_STRATEGY=firstfit "$NULLSH" \
        >"$tmp/out.raw" 2>"$tmp/err.raw" <<'TRANSCRIPT'
echo hello world | tr a-z A-Z
echo one > notes.txt
echo two >> notes.txt
cat < notes.txt | wc -l
sleep 3 &
jobs
fg
heap stats
heap strategy buddy
heap strategy
inspect --sections /usr/bin/ls | grep .text
printf '\140\005\141\000\360\051\321\025\022\010' > five.ch8
export NSH_EMU_HEADLESS=100
emu five.ch8 | head -6
netmon
exit
TRANSCRIPT
}

# Pids, heap byte counts and ELF addresses move every run, so they collapse to
# placeholders. Everything else has to match the README exactly.
normalize() {
    sed -E \
        -e 's/^\[1\] [0-9]+$/[1] PID/' \
        -e 's/^(arena size|used bytes|free bytes|live blocks|free blocks|largest free|total mallocs|total frees)( +)[0-9]+$/\1\2N/' \
        -e '/\.text/ { s/0x[0-9a-fA-F]+/0xADDR/g; s/^\[[0-9]+\]/[NN]/; s/[[:space:]]+/ /g; }' \
        "$1"
}

run_transcript

normalize "$tmp/out.raw" >"$tmp/out"
normalize "$tmp/err.raw" >"$tmp/err"

cat >"$tmp/out.expected" <<'EXPECTED'
HELLO WORLD
2
[1]  Running  sleep 3 &
sleep 3 &
[1]  Done  sleep 3 &
strategy       firstfit
arena size     N
used bytes     N
free bytes     N
live blocks    N
free blocks    N
largest free   N
total mallocs  N
total frees    N
buddy
[NN] .text PROGBITS 0xADDR 0xADDR 0xADDR AX
####............................................................
#...............................................................
####............................................................
...#............................................................
####............................................................
................................................................
EXPECTED

cat >"$tmp/err.expected" <<'EXPECTED'
[1] PID
nullsh: netmon: usage: netmon IFACE [--filter tcp|udp] [--port N]
EXPECTED

fail=0

report() {
    if diff -u "$2" "$3" >"$tmp/diff"; then
        echo "  ok   $1"
    else
        echo "  FAIL $1: the README transcript no longer matches"
        sed 's/^/        /' "$tmp/diff"
        fail=1
    fi
}

report "the transcript stdout matches README.md" "$tmp/out.expected" "$tmp/out"
report "the transcript stderr matches README.md" "$tmp/err.expected" "$tmp/err"

# The two commands the README shows outside the transcript block. A root run
# would really open a socket, so the refusal is only checked as a normal user.
if [ "$(id -u)" != 0 ]; then
    cd "$tmp" || exit 1
    printf 'netmon eth0\n' | HOME="$tmp" "$NULLSH" >"$tmp/nm.out" 2>"$tmp/nm.err"
    cat >"$tmp/nm.expected" <<'EXPECTED'
nullsh: netmon: eth0: needs root, try sudo nullsh
EXPECTED
    report "netmon without root refuses as README.md says" "$tmp/nm.expected" \
        "$tmp/nm.err"
else
    echo "  SKIP the netmon refusal check, this is running as root"
fi

if [ "$fail" -ne 0 ]; then
    echo "demo: README.md is out of date"
    exit 1
fi
echo "demo: README.md transcript verified"
exit 0
