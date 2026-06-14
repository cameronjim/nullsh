#!/bin/sh
# Phase 8 line editing driven through a real pty: arrow keys, Home and End,
# the Ctrl-A/E/K/U/W bindings, and Up walking history. Every case is asserted
# through a file the edited command writes, never through the pty log, because
# the log also carries the editor's own redraws.
# A missing pty or a shell that never appears is a SKIP, never a failure.
# Usage: 09_edit.sh <path to nullsh binary>, supplied by make test.

set -u

NULLSH="$1"
case "$NULLSH" in
    /*) ;;
    *) NULLSH="$(pwd)/$NULLSH" ;;
esac
NSHNAME=$(basename "$NULLSH")

skip_all() {
    echo "  SKIP 09_edit: $1"
    exit 0
}

command -v script >/dev/null 2>&1 || skip_all "script(1) is not installed"
command -v mkfifo >/dev/null 2>&1 || skip_all "mkfifo is not available"
printf '\033' | od -An -to1 2>/dev/null | grep -q 033 ||
    skip_all "printf does not emit octal escapes"

tmp=$(mktemp -d)
fail=0
shpid=""

cleanup() {
    [ -n "$shpid" ] && kill "$shpid" 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT

# Raw bytes, no trailing newline: this is a keystroke, not a line.
send() { printf '%s' "$1" >&3; }
line() { printf '%s\n' "$1" >&3; }

# Repeats one keystroke, for the arrow runs.
repeat() {
    i=0
    while [ "$i" -lt "$2" ]; do
        send "$1"
        i=$((i + 1))
    done
}

ESC=$(printf '\033')
UP="$ESC[A"
DOWN="$ESC[B"
RIGHT="$ESC[C"
LEFT="$ESC[D"
DEL="$ESC[3~"
C_A=$(printf '\001')
C_E=$(printf '\005')
C_K=$(printf '\013')
C_U=$(printf '\025')
C_W=$(printf '\027')

# The shell writes its result into a file, so the check waits for that file to
# hold the expected text rather than guessing at a sleep length.
wait_file() {
    i=0
    while [ $i -lt 40 ]; do
        if [ -f "$tmp/$1" ] && [ "$(cat "$tmp/$1")" = "$2" ]; then
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

check_file() {
    if wait_file "$2" "$3"; then
        echo "  ok   $1"
    else
        got="(no file)"
        [ -f "$tmp/$2" ] && got=$(cat "$tmp/$2")
        echo "  FAIL $1: $2 holds [$got], expected [$3]"
        fail=1
    fi
}

fifo="$tmp/in"
out="$tmp/out"
mkfifo "$fifo" || skip_all "could not create a fifo"

HOME="$tmp" script -qec "$NULLSH" /dev/null <"$fifo" >"$out" 2>&1 &
shpid=$!
exec 3>"$fifo"

nsh=""
i=0
while [ $i -lt 40 ] && [ -z "$nsh" ]; do
    nsh=$(ps -eo pid=,comm= 2>/dev/null |
          awk -v n="$NSHNAME" '$2 == n { print $1; exit }')
    [ -n "$nsh" ] && break
    sleep 0.1
    i=$((i + 1))
done
[ -n "$nsh" ] || skip_all "the shell never appeared under script(1)"

# A short prompt keeps every test line well inside one terminal row, and it is
# the tilde expansion doing the cd.
line 'cd ~'
sleep 0.3

# 0. The gate: plain typing reaches the shell at all. Without this the pty is
# not delivering keystrokes and nothing below can mean anything.
line 'echo one > a'
wait_file a one || skip_all "a plainly typed line never ran"
echo "  ok   a typed line runs"

# 1. Up recalls it, Ctrl-A goes home, four Rights clear "echo", text splices in.
send "$UP"
sleep 0.3
send "$C_A"
repeat "$RIGHT" 4
send ' two'
send "$C_E"
line ''
check_file "up recalls, home and right position, insert splices" a "two one"

# 2. Ctrl-W kills the word left of the cursor.
send 'echo ww > zzz'
send "$C_W"
send 'b'
line ''
check_file "ctrl-w kills the word left of the cursor" b "ww"

# 3. Ctrl-U kills everything left of the cursor.
send 'garbage that should vanish'
send "$C_U"
send 'echo uu > c'
line ''
check_file "ctrl-u kills to the start of the line" c "uu"

# 4. Left arrows then Delete removes characters under the cursor.
send 'echo dd > dXY'
repeat "$LEFT" 2
send "$DEL$DEL"
line ''
check_file "left arrow then delete removes forward" d "dd"

# 5. Left arrows then Ctrl-K truncates from the cursor.
send 'echo kk > eJUNK'
repeat "$LEFT" 4
send "$C_K"
line ''
check_file "ctrl-k truncates from the cursor" e "kk"

# 6. Backspace at the end, the plainest edit there is.
send 'echo ff > fQ'
printf '\177' >&3
line ''
check_file "backspace removes the character before the cursor" f "ff"

# 7. Up twice then Down once lands on the entry in between.
send "$UP$UP"
sleep 0.3
send "$DOWN"
sleep 0.3
send "$C_A"
repeat "$RIGHT" 4
send ' gg'
send "$C_E"
printf '\010' >&3
send 'g'
line ''
check_file "up up down walks history both ways" g "gg ff"

line 'exit 0'
exec 3>&-
wait "$shpid" 2>/dev/null
shpid=""

exit $fail
