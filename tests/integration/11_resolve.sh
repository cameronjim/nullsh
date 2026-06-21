#!/bin/sh
# End to end tests for the resolve builtin: every way the arguments can be
# wrong, the missing-nameserver refusal, the help line, and the two checks
# that need a real query on the wire.
# Usage: 11_resolve.sh <path to nullsh binary>, supplied by make test.

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
    if grep -Eq "$3" "$2"; then
        echo "  ok   $1"
    else
        echo "  FAIL $1: [$3] does not match a line of $2, saw:"
        sed 's/^/        /' "$2"
        fail=1
    fi
}

# Every run points NSH_RESOLV_CONF at a fixture, so no check ever depends on
# the machine's own /etc/resolv.conf unless it says so.
: > "$tmp/empty.conf"
printf 'nameserver fe80::1\nnameserver 2001:4860:4860::8888\n' > "$tmp/v6.conf"
conf="$tmp/empty.conf"

# HOME points at the scratch dir so the developer's history file is untouched.
run() {
    HOME="$tmp" NSH_RESOLV_CONF="$conf" "$NULLSH" >"$tmp/out" 2>"$tmp/err"
}

USAGE="^nullsh: resolve: usage: resolve NAME "

# Argument checking happens before resolv.conf is opened and before any
# socket exists, so every one of these is fast and needs no network.
bad_args() {
    printf '%s\necho $?\necho still here\n' "$1" | run
    check_contains "$1 prints a usage line" "$tmp/err" "$USAGE"
    check "$1 leaves 1" "$(sed -n 1p "$tmp/out")" "1"
    check "the shell survives $1" "$(sed -n 2p "$tmp/out")" "still here"
}

# 1. No name to look up.
bad_args 'resolve'
bad_args 'resolve --server 8.8.8.8'

# 2. More than one name.
bad_args 'resolve a.test b.test'

# 3. Flags resolve does not take.
bad_args 'resolve a.test --wat 1'
bad_args 'resolve a.test -s 8.8.8.8'

# 4. Flags with no value after them.
bad_args 'resolve a.test --server'
bad_args 'resolve a.test --port'
bad_args 'resolve a.test --timeout'
bad_args 'resolve a.test --tries'

# 5. Numbers that are not numbers, or are outside their range.
bad_args 'resolve a.test --port 0'
bad_args 'resolve a.test --port 65536'
bad_args 'resolve a.test --port abc'
bad_args 'resolve a.test --port 53x'
bad_args 'resolve a.test --timeout 0'
bad_args 'resolve a.test --timeout 60001'
bad_args 'resolve a.test --timeout nope'
bad_args 'resolve a.test --tries 0'
bad_args 'resolve a.test --tries 6'
bad_args 'resolve a.test --tries -1'

# 6. A conf file with no usable nameserver, and no --server to fall back on.
no_nameserver() {
    conf="$1"
    printf 'resolve example.com\necho $?\necho still here\n' | run
    check_contains "$2 asks for --server" "$tmp/err" \
        "^nullsh: resolve: no nameserver found; use --server\$"
    check "$2 leaves 1" "$(sed -n 1p "$tmp/out")" "1"
    check "the shell survives $2" "$(sed -n 2p "$tmp/out")" "still here"
}

no_nameserver "$tmp/empty.conf" "an empty resolv.conf"
no_nameserver "$tmp/v6.conf" "an ipv6-only resolv.conf"
no_nameserver "$tmp/no_such.conf" "a missing resolv.conf"

conf="$tmp/empty.conf"

# 7. resolve is listed by help.
printf 'help\n' | run
check_contains "help lists resolve" "$tmp/out" \
    "^  resolve NAME     dns lookup, --server IP, --port N,\$"
check_contains "the help line names the rest of the flags" "$tmp/out" \
    "^                   --timeout MS, --tries N\$"

# 8. DEFERRED. Everything below needs the real src/resolve/dns.c and
# src/resolve/net.c. Against the contracts-commit stubs dns_build_query
# refuses first, so these checks report the wrong message and FAIL. That is
# expected in the resolve agent's clone; the integrator runs them for real.

# Nothing listens on 127.0.0.1 port 1. Either the datagram is simply never
# answered, or the kernel turns the ICMP refusal into a socket error, so both
# reasons count as long as the status is 1.
printf 'resolve example.com --server 127.0.0.1 --port 1 --tries 1 --timeout 200\necho $?\n' | run
check_matches "[needs real dns/net] a dead server is reported" "$tmp/err" \
    "^nullsh: resolve: (no reply from 127\.0\.0\.1 after 1 try|socket failure talking to 127\.0\.0\.1)\$"
check "[needs real dns/net] a dead server leaves 1" "$(sed -n 1p "$tmp/out")" "1"

# 9. DEFERRED and skippable: one real lookup against the machine's own
# resolver. A sandbox without DNS skips instead of failing.
if ! grep -Eq '^[[:space:]]*nameserver[[:space:]]+[0-9]+\.' /etc/resolv.conf 2>/dev/null; then
    echo "  SKIP a real lookup (no ipv4 nameserver in /etc/resolv.conf)"
elif ! command -v getent >/dev/null 2>&1; then
    echo "  SKIP a real lookup (no getent to probe for working dns)"
elif ! getent hosts example.com >/dev/null 2>&1; then
    echo "  SKIP a real lookup (this environment cannot resolve example.com)"
else
    conf=/etc/resolv.conf
    printf 'resolve example.com\necho $?\n' | run
    check_matches "[needs real dns/net] the pinned header line" "$tmp/out" \
        "^;; id [0-9]+ flags qr( aa)?( tc)? rd( ra)? rcode NOERROR answers [0-9]+\$"
    check_matches "[needs real dns/net] an answer record" "$tmp/out" \
        "^[A-Za-z0-9.-]+\. [0-9]+ IN (A [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+|CNAME [A-Za-z0-9.-]+\.)\$"
    check "[needs real dns/net] a real lookup leaves 0" \
        "$(sed -n '$p' "$tmp/out")" "0"
    conf="$tmp/empty.conf"
fi

exit $fail
