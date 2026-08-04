#!/bin/sh
# End to end tests for the netmon builtin that need no raw socket: every way
# the arguments can be wrong, and the refusal a normal user gets.
# Usage: 08_netmon_noroot.sh <path to nullsh binary>, supplied by make test.

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

# HOME points at the scratch dir so the developer's history file is untouched.
run() {
    HOME="$tmp" "$NULLSH" >"$tmp/out" 2>"$tmp/err"
}

USAGE="^nullsh: netmon: usage: netmon IFACE "

# Every one of these has to be rejected before any socket is opened, or a run
# as root would block here forever.
bad_args() {
    printf '%s\necho $?\necho still here\n' "$1" | run
    check_contains "$1 prints a usage line" "$tmp/err" "$USAGE"
    check "$1 leaves 1" "$(sed -n 1p "$tmp/out")" "1"
    check "the shell survives $1" "$(sed -n 2p "$tmp/out")" "still here"
}

# 1. No interface at all.
bad_args 'netmon'

# 2. An option where the interface belongs.
bad_args 'netmon --filter tcp'

# 3. Filters that are not tcp or udp.
bad_args 'netmon lo --filter icmp'
bad_args 'netmon lo --filter'

# 4. Ports that are not 0..65535.
bad_args 'netmon lo --port 65536'
bad_args 'netmon lo --port 99999'
bad_args 'netmon lo --port -1'
bad_args 'netmon lo --port abc'
bad_args 'netmon lo --port 80x'
bad_args 'netmon lo --port'

# 5. Words and flags netmon does not take.
bad_args 'netmon lo extra'
bad_args 'netmon lo --wat'

# 6. The refusal a normal user gets from socket(AF_PACKET). Under root the
# capture would run instead, so that case only runs unprivileged.
if [ "$(id -u)" = 0 ]; then
    echo "  SKIP the needs-root message (this suite is running as root)"
else
    printf 'netmon lo\necho $?\necho still here\n' | run
    check_contains "a normal user is told to use sudo" "$tmp/err" \
        "^nullsh: netmon: lo: needs root, try sudo nullsh\$"
    check "the refusal leaves 1" "$(sed -n 1p "$tmp/out")" "1"
    check "the shell survives the refusal" "$(sed -n 2p "$tmp/out")" "still here"
fi

# 7. netmon is listed by help.
printf 'help\n' | run
check_contains "help lists netmon" "$tmp/out" \
    "^  netmon IFACE     decode packets, --filter tcp|udp, --port N\$"

exit $fail
