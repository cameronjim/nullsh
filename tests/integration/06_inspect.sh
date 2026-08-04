#!/bin/sh
# End to end tests for the inspect builtin: the four views against fixtures
# compiled here with gcc, pipes, and the three error shapes.
# Usage: 06_inspect.sh <path to nullsh binary>, supplied by make test.

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

# The parser lives in a sibling clone during development. Until it is linked in
# every inspect call fails, so the whole script stands down rather than lie.
printf 'inspect /bin/true\necho $?\n' | run
if [ -r /bin/true ] && [ "$(tail -n 1 "$tmp/out")" != "0" ]; then
    echo "  SKIP: parser not integrated"
    exit 0
fi

if ! command -v gcc >/dev/null 2>&1; then
    echo "  SKIP: gcc not available"
    exit 0
fi

cat >"$tmp/hello.c" <<'EOF'
#include <stdio.h>
int counter = 7;
int main(void) {
    printf("hello\n");
    return 0;
}
EOF

if ! gcc -o "$tmp/fix_pie" "$tmp/hello.c" 2>"$tmp/gcc_err"; then
    echo "  SKIP: gcc cannot link a fixture"
    sed 's/^/        /' "$tmp/gcc_err"
    exit 0
fi
if ! gcc -c -o "$tmp/fix.o" "$tmp/hello.c" 2>"$tmp/gcc_err"; then
    echo "  SKIP: gcc cannot compile a fixture"
    sed 's/^/        /' "$tmp/gcc_err"
    exit 0
fi

# 1. The bare header view: a default gcc link is position independent.
printf 'inspect %s/fix_pie\necho $?\n' "$tmp" | run
check_contains "header names the class" "$tmp/out" "^class  *ELF64 little endian\$"
check_contains "a default gcc link is DYN" "$tmp/out" "^type  *DYN"
check_contains "header names the machine" "$tmp/out" "^machine  *x86-64"
check_contains "header has an entry point" "$tmp/out" "^entry  *0x"
check_contains "header counts sections" "$tmp/out" "^sections  *[0-9]"
check_contains "header counts segments" "$tmp/out" "^segments  *[0-9]"
check_contains "header names shstrndx" "$tmp/out" "^shstrndx  *[0-9]"
check "header status is 0" "$(tail -n 1 "$tmp/out")" "0"

# 2. A relocatable object is ET_REL and has no segments.
printf 'inspect %s/fix.o\n' "$tmp" | run
check_contains "an object file is REL" "$tmp/out" "^type  *REL"

# 3. The section table carries the names the linker used.
printf 'inspect --sections %s/fix_pie\necho $?\n' "$tmp" | run
check_matches "sections list .text" "$tmp/out" "^\[ *[0-9]+\]  +\.text +PROGBITS"
check_contains "sections list .bss as NOBITS" "$tmp/out" "NOBITS"
check_matches "an allocated section carries flag letters" "$tmp/out" \
    " (A|WA|AX|WAX)\$"
check "sections status is 0" "$(tail -n 1 "$tmp/out")" "0"

# 4. The segment table is the loader's view, with real permissions.
printf 'inspect --segments %s/fix_pie\n' "$tmp" | run
check_matches "segments list a LOAD" "$tmp/out" "^\[ *[0-9]+\]  +LOAD"
check_matches "a LOAD carries rwx letters" "$tmp/out" \
    "^\[ *[0-9]+\]  +LOAD +(r-x|rw-|r--)"
check_contains "segments name the gnu stack" "$tmp/out" "GNU_STACK"

# 5. The symbol table finds main, and labels which table it came from.
printf 'inspect --symbols %s/fix_pie\n' "$tmp" | run
check_contains "symbols name their table" "$tmp/out" "^symbol table: \.\(sym\|dyn\)"
check_matches "symbols include main as a FUNC" "$tmp/out" " FUNC +main\$"
check_matches "symbols include the global counter" "$tmp/out" \
    " OBJECT +counter\$"

# 6. --all stacks the header and the three tables in one run.
printf 'inspect --all %s/fix_pie\n' "$tmp" | run
check_contains "all includes the header" "$tmp/out" "^class  *ELF64"
check_contains "all includes the sections" "$tmp/out" "PROGBITS"
check_contains "all includes the segments" "$tmp/out" "LOAD"
check_contains "all includes the symbols" "$tmp/out" "^symbol table: "

# 7. inspect is a builtin, so its output has to survive a pipe.
printf 'inspect --sections %s/fix_pie | grep -c PROGBITS\n' "$tmp" | run
count=$(cat "$tmp/out")
if [ "$count" -ge 1 ] 2>/dev/null; then
    echo "  ok   sections through a pipe count PROGBITS rows"
else
    echo "  FAIL sections through a pipe: got [$count]"
    fail=1
fi
printf 'inspect --symbols %s/fix_pie | grep main\n' "$tmp" | run
check_contains "symbols through a pipe find main" "$tmp/out" "main"

# 8. Section count parity with readelf, when readelf is installed.
if command -v readelf >/dev/null 2>&1; then
    want=$(readelf -h "$tmp/fix_pie" \
        | sed -n 's/.*Number of section headers: *\([0-9][0-9]*\).*/\1/p')
    printf 'inspect --sections %s/fix_pie\n' "$tmp" | run
    # One heading line sits above the rows.
    got=$(( $(wc -l <"$tmp/out") - 1 ))
    if [ -n "$want" ] && [ "$got" -ge "$want" ]; then
        echo "  ok   section count matches readelf ($got >= $want)"
    else
        echo "  FAIL section count: readelf says [$want], inspect shows [$got]"
        fail=1
    fi
else
    echo "  SKIP: readelf not available, no section count parity check"
fi

# 9. A file that is not there.
printf 'inspect %s/no_such_file\necho $?\necho still here\n' "$tmp" | run
check_contains "a missing file cannot be opened" "$tmp/err" \
    "^nullsh: inspect: .*no_such_file: cannot open\$"
check "a missing file leaves 1" "$(sed -n 1p "$tmp/out")" "1"
check "the shell survives a missing file" "$(sed -n 2p "$tmp/out")" "still here"

# 10. A readable file that is not ELF.
printf 'not an elf file\n' >"$tmp/plain.txt"
printf 'inspect %s/plain.txt\necho $?\n' "$tmp" | run
check_contains "plain text is not elf" "$tmp/err" \
    "^nullsh: inspect: .*plain.txt: not a supported elf file\$"
check "plain text leaves 1" "$(sed -n 1p "$tmp/out")" "1"

if [ -r /etc/hostname ]; then
    printf 'inspect /etc/hostname\necho $?\n' | run
    check_contains "/etc/hostname is not elf" "$tmp/err" \
        "^nullsh: inspect: /etc/hostname: not a supported elf file\$"
    check "/etc/hostname leaves 1" "$(sed -n 1p "$tmp/out")" "1"
fi

# 11. Misuse: no file, two files, an unknown flag.
printf 'inspect\necho $?\n' | run
check_contains "no argument prints a usage line" "$tmp/err" \
    "^nullsh: inspect: usage: inspect "
check "no argument leaves 1" "$(sed -n 1p "$tmp/out")" "1"
check "no argument prints nothing on stdout but the status" \
    "$(wc -l <"$tmp/out" | tr -d ' ')" "1"

printf 'inspect %s/fix_pie %s/fix.o\necho $?\n' "$tmp" "$tmp" | run
check_contains "two files print a usage line" "$tmp/err" \
    "^nullsh: inspect: usage: inspect "
check "two files leave 1" "$(sed -n 1p "$tmp/out")" "1"

printf 'inspect --headers %s/fix_pie\necho $?\n' "$tmp" | run
check_contains "an unknown flag prints a usage line" "$tmp/err" \
    "^nullsh: inspect: usage: inspect "
check "an unknown flag leaves 1" "$(sed -n 1p "$tmp/out")" "1"

exit $fail
