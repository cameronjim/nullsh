#!/bin/sh
# End to end tests for phase 9, the interpreter: operators, if, while, for,
# break, continue, return, functions, comments, multi-line input, script files
# with positional parameters, and the syntax errors that stop a stream.
# Usage: 10_interpreter.sh <path to nullsh binary>, supplied by make test.

set -u

NULLSH="$1"
case "$NULLSH" in
    /*) ;;
    *) NULLSH="$(pwd)/$NULLSH" ;;
esac
NSHNAME=$(basename "$NULLSH")

tmp=$(mktemp -d)
fail=0
shpid=""

cleanup() {
    [ -n "$shpid" ] && kill "$shpid" 2>/dev/null
    rm -rf "$tmp"
}
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
# ~/.nullsh_history is never touched.
run() {
    HOME="$tmp" "$NULLSH" >"$tmp/out" 2>"$tmp/err"
}

# stdout on one line, so a multi-line result is one string to compare.
flat() { tr '\n' ' ' <"$tmp/out"; }

echo "-- operators"

printf 'true && echo yes\n' | run
check "&& runs the right side after a success" "$(cat "$tmp/out")" "yes"

printf 'false && echo no\necho end\n' | run
check "&& skips the right side after a failure" "$(flat)" "end "

printf 'false || echo yes\n' | run
check "|| runs the right side after a failure" "$(cat "$tmp/out")" "yes"

printf 'true || echo no\necho end\n' | run
check "|| skips the right side after a success" "$(flat)" "end "

printf 'true && echo a && echo b\n' | run
check "a chain of three ands" "$(flat)" "a b "

printf 'false && echo a || echo b\n' | run
check "and then or picks the or" "$(cat "$tmp/out")" "b"

printf '! true\necho $?\n' | run
check "! inverts a success" "$(cat "$tmp/out")" "1"

printf '! false\necho $?\n' | run
check "! inverts a failure" "$(cat "$tmp/out")" "0"

printf 'echo a; echo b\n' | run
check "semicolon sequences two commands" "$(flat)" "a b "

printf 'false; echo $?\n' | run
check 'the status survives a semicolon' "$(cat "$tmp/out")" "1"

printf 'true && false\necho $?\n' | run
check 'the status is the last command the chain ran' "$(cat "$tmp/out")" "1"

printf 'false || true\necho $?\n' | run
check "an or chain ends on the side it ran" "$(cat "$tmp/out")" "0"

echo "-- if"

printf 'if true; then echo t; else echo f; fi\n' | run
check "if takes the then branch" "$(cat "$tmp/out")" "t"

printf 'if false; then echo t; else echo f; fi\n' | run
check "if takes the else branch" "$(cat "$tmp/out")" "f"

printf 'if false; then echo a; elif true; then echo b; else echo c; fi\n' | run
check "elif takes the middle branch" "$(cat "$tmp/out")" "b"

printf 'if false; then echo t; fi\necho $?\n' | run
check "an if that ran nothing leaves 0" "$(cat "$tmp/out")" "0"

echo "-- while"

# Three flag files drained one per pass, so the log line count is the count of
# iterations. rm on a missing file fails, which is what walks the || chain on.
mkdir "$tmp/w"
: >"$tmp/w/f1"
: >"$tmp/w/f2"
: >"$tmp/w/f3"
run <<NSH
while [ -e $tmp/w/f1 ]
do
echo tick >> $tmp/w/log
rm $tmp/w/f3 2> /dev/null || rm $tmp/w/f2 2> /dev/null || rm $tmp/w/f1 2> /dev/null
done
echo done \$?
NSH
check "the while body ran once per flag" "$(wc -l <"$tmp/w/log" | tr -d ' ')" "3"
check "the while loop drained every flag" "$(ls "$tmp/w" | tr '\n' ' ')" "log "
check "a finished while leaves the body's status" "$(cat "$tmp/out")" "done 0"

run <<'NSH'
while false
do
echo no
done
echo $?
NSH
check "a while whose body never ran leaves 0" "$(cat "$tmp/out")" "0"

mkdir "$tmp/wb"
: >"$tmp/wb/flag"
run <<NSH
while [ -e $tmp/wb/flag ]
do
echo spin
break
echo unreached
done
echo \$?
NSH
check "break leaves a while at once" "$(flat)" "spin 0 "

echo "-- for"

printf 'for x in a b c; do echo $x; done\n' | run
check "for walks its words in order" "$(flat)" "a b c "

run <<'NSH'
for x in
do
echo no
done
echo $?
NSH
check "a for with no words leaves 0" "$(cat "$tmp/out")" "0"

printf 'for x in a b; do echo $x; done\necho [$x]\n' | run
check "the loop variable outlives the loop" "$(flat)" "a b [b] "

printf 'for x in a b c; do echo $x; break; done\necho $?\n' | run
check "break leaves a for at once" "$(flat)" "a 0 "

printf 'for x in a b c; do [ $x = b ] && continue; echo $x; done\n' | run
check "continue skips the rest of the body" "$(flat)" "a c "

printf 'break\necho $?\n' | run
check_contains "a stray break says so" "$tmp/err" "break: only meaningful in a loop"
check "a stray break leaves 1" "$(cat "$tmp/out")" "1"

printf 'continue\necho $?\n' | run
check_contains "a stray continue says so" "$tmp/err" \
    "continue: only meaningful in a loop"
check "a stray continue leaves 1" "$(cat "$tmp/out")" "1"

printf 'return\necho $?\n' | run
check_contains "return outside a function says so" "$tmp/err" \
    "return: only meaningful in a function"
check "return outside a function leaves 1" "$(cat "$tmp/out")" "1"

echo "-- functions"

printf 'greet() { echo hello; }\ngreet\n' | run
check "a function is defined and called" "$(cat "$tmp/out")" "hello"

printf 'f() { echo $1 $2 count $#; }\nf one two\n' | run
check "a call fills in the positional parameters" "$(cat "$tmp/out")" \
    "one two count 2"

printf 'f() { echo first; }\nf() { echo second; }\nf\n' | run
check "a redefinition replaces the body" "$(cat "$tmp/out")" "second"

printf 'f() { return 7; }\nf\necho $?\n' | run
check "return carries its argument out" "$(cat "$tmp/out")" "7"

printf 'f() { false; return; }\nf\necho $?\n' | run
check "a bare return reuses the last status" "$(cat "$tmp/out")" "1"

printf 'f() { echo body; return; echo unreached; }\nf\n' | run
check "return stops the rest of the body" "$(cat "$tmp/out")" "body"

printf 'r() { r; }\nr\necho $?\n' | run
check_contains "runaway recursion is capped" "$tmp/err" \
    "function recursion too deep"
check "a capped recursion leaves 1" "$(cat "$tmp/out")" "1"

printf 'cd() { echo shadowed; }\ncd %s\npwd\n' "$tmp" | run
check "a function cannot shadow a builtin" "$(cat "$tmp/out")" \
    "$(cd "$tmp" && pwd -P)"

printf 'f() { echo piped; }\nf | cat\n' | run
check "a function works as a pipeline stage" "$(cat "$tmp/out")" "piped"

echo "-- scripts"

cat >"$tmp/args.nsh" <<'NSH'
#!/usr/bin/env nullsh
# a comment sitting in the middle of a script
f() { echo "in 0=$0 1=$1 n=$#"; }
echo "top 0=$0 1=$1 2=$2 n=$#"
f inner
echo "back 1=$1 n=$#"
exit 3
NSH
HOME="$tmp" "$NULLSH" "$tmp/args.nsh" one two >"$tmp/out" 2>"$tmp/err"
code=$?
check 'a script sees its path and its arguments' "$(sed -n 1p "$tmp/out")" \
    "top 0=$tmp/args.nsh 1=one 2=two n=2"
check 'the script path is unchanged inside a function body' \
    "$(sed -n 2p "$tmp/out")" \
    "in 0=$tmp/args.nsh 1=inner n=1"
check "the positionals come back after the call" "$(sed -n 3p "$tmp/out")" \
    "back 1=one n=2"
check "exit propagates out of a script" "$code" "3"
check "a script says nothing on stderr" \
    "$(wc -c <"$tmp/err" | tr -d ' ')" "0"

HOME="$tmp" "$NULLSH" "$tmp/nosuchscript.nsh" >"$tmp/out" 2>"$tmp/err"
code=$?
check "an unopenable script leaves 127" "$code" "127"
check_contains "an unopenable script names itself" "$tmp/err" \
    "nosuchscript.nsh: No such file or directory"

cat >"$tmp/plain.nsh" <<'NSH'
for w in a b
do
echo $w
done
NSH
HOME="$tmp" "$NULLSH" "$tmp/plain.nsh" >"$tmp/out" 2>"$tmp/err"
check "a script without a shebang runs too" "$(flat)" "a b "

# A script must not touch the history file the prompt owns.
rm -f "$tmp/.nullsh_history"
HOME="$tmp" "$NULLSH" "$tmp/plain.nsh" >"$tmp/out" 2>"$tmp/err"
if [ -f "$tmp/.nullsh_history" ]; then
    echo "  FAIL a script wrote the history file"
    fail=1
else
    echo "  ok   a script leaves the history file alone"
fi

echo "-- multi-line input"

run <<'NSH'
if true
then
echo hi
fi
NSH
check "an if spread over four lines" "$(cat "$tmp/out")" "hi"

mkdir "$tmp/m"
: >"$tmp/m/flag"
run <<NSH
while [ -e $tmp/m/flag ]
do
rm $tmp/m/flag
echo once
done
NSH
check "a while spread over lines" "$(cat "$tmp/out")" "once"

run <<'NSH'
echo "a
b"
NSH
check "a double quoted string spans a newline" "$(flat)" "a b "

run <<'NSH'
f() {
echo multi
}
f
NSH
check "a function defined over several lines" "$(cat "$tmp/out")" "multi"

echo "-- comments"

printf 'echo hi # tail\n' | run
check "a trailing comment is dropped" "$(cat "$tmp/out")" "hi"

printf '# nothing here\necho after\n' | run
check "a whole line comment runs nothing" "$(cat "$tmp/out")" "after"

printf 'echo "# quoted"\n' | run
check "a hash inside quotes stays literal" "$(cat "$tmp/out")" "# quoted"

printf 'echo a#b\n' | run
check "a hash inside a word stays literal" "$(cat "$tmp/out")" "a#b"

echo "-- syntax errors"

printf 'fi\n' | run
code=$?
check_contains "a closing keyword with nothing to close is an error" \
    "$tmp/err" "^nullsh: syntax error$"
check "a syntax error leaves 2" "$code" "2"

printf 'echo a\nfi\necho unreached\n' | run
check "a syntax error stops a non-interactive stream" "$(flat)" "a "

printf 'echo &&\n' | run
code=$?
check_contains "input that ends mid-chain says end of file" "$tmp/err" \
    "syntax error: unexpected end of file"
check "an unfinished chain leaves 2" "$code" "2"

printf 'if true\nthen\necho hi\n' | run
code=$?
check_contains "an unfinished if says end of file" "$tmp/err" \
    "syntax error: unexpected end of file"
check "an unfinished if leaves 2" "$code" "2"
check "an unfinished if runs nothing" "$(cat "$tmp/out")" ""

printf 'echo "unterminated\n' | run
code=$?
check_contains "an unterminated quote says end of file" "$tmp/err" \
    "syntax error: unexpected end of file"
check "an unterminated quote leaves 2" "$code" "2"

printf 'if true; then echo a; fi | cat\n' | run
code=$?
check_contains "a compound cannot be piped" "$tmp/err" "^nullsh: syntax error$"
check "a piped compound leaves 2" "$code" "2"

printf 'true && true &\n' | run
code=$?
check_contains "a list cannot be backgrounded" "$tmp/err" \
    "^nullsh: syntax error$"
check "a backgrounded list leaves 2" "$code" "2"

printf 'echo ${10}\n' | run
code=$?
check_contains "a two digit positional is a bad substitution" "$tmp/err" \
    "bad substitution"
check "a bad substitution leaves 2" "$code" "2"

echo "-- the continuation prompt on a real terminal"

pty_skip() {
    echo "  SKIP the PS2 prompt check: $1"
}

pty_check() {
    command -v script >/dev/null 2>&1 || {
        pty_skip "script(1) is not installed"
        return 0
    }
    command -v mkfifo >/dev/null 2>&1 || {
        pty_skip "mkfifo is not available"
        return 0
    }

    fifo="$tmp/in"
    log="$tmp/pty.log"
    mkfifo "$fifo" 2>/dev/null || {
        pty_skip "could not create a fifo"
        return 0
    }

    HOME="$tmp" script -qec "$NULLSH" /dev/null <"$fifo" >"$log" 2>&1 &
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
    if [ -z "$nsh" ]; then
        pty_skip "the shell never appeared under script(1)"
        return 0
    fi

    # A short prompt keeps every line well inside one terminal row.
    printf 'cd ~\n' >&3
    sleep 0.3
    printf 'touch %s/gate\n' "$tmp" >&3
    i=0
    while [ $i -lt 40 ] && [ ! -e "$tmp/gate" ]; do
        sleep 0.1
        i=$((i + 1))
    done
    if [ ! -e "$tmp/gate" ]; then
        pty_skip "a plainly typed line never ran"
        return 0
    fi

    # No redirect anywhere in the construct, so the only "> " in the log can
    # be the continuation prompt itself.
    printf 'if true\n' >&3
    sleep 0.3
    printf 'then\n' >&3
    sleep 0.3
    printf 'touch %s/ps2done\n' "$tmp" >&3
    sleep 0.3
    printf 'fi\n' >&3
    i=0
    while [ $i -lt 40 ] && [ ! -e "$tmp/ps2done" ]; do
        sleep 0.1
        i=$((i + 1))
    done
    printf 'exit 0\n' >&3
    exec 3>&-
    wait "$shpid" 2>/dev/null
    shpid=""

    if [ -e "$tmp/ps2done" ]; then
        echo "  ok   a multi-line if typed at a terminal runs"
    else
        echo "  FAIL a multi-line if typed at a terminal never ran"
        fail=1
    fi
    if grep -q '> ' "$log"; then
        echo "  ok   the continuation prompt appears"
    else
        echo "  FAIL the continuation prompt never appeared"
        fail=1
    fi
}

pty_check

exit $fail
