#!/bin/sh
# End to end test for the netmon capture path: a backgrounded netmon on lo
# decodes a UDP datagram this script sends, then stops on SIGINT and reports.
# Needs a raw socket, so it runs from make test-net and skips without root.
# Usage: 08_netmon.sh <path to nullsh binary>.

set -u

NULLSH="$1"
case "$NULLSH" in
    /*) ;;
    *) NULLSH="$(pwd)/$NULLSH" ;;
esac

if [ "$(id -u)" != 0 ]; then
    echo "SKIP: netmon needs root"
    exit 0
fi

PORT=9999

send_udp() {
    if command -v python3 >/dev/null 2>&1; then
        python3 -c "import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b'nullsh', ('127.0.0.1', $PORT))
s.close()"
        return $?
    fi
    if command -v bash >/dev/null 2>&1; then
        bash -c "echo nullsh > /dev/udp/127.0.0.1/$PORT"
        return $?
    fi
    return 1
}

if ! command -v python3 >/dev/null 2>&1 && ! command -v bash >/dev/null 2>&1; then
    echo "SKIP: no python3 and no bash to generate a udp datagram"
    exit 0
fi

tmp=$(mktemp -d)
fail=0

cleanup() { rm -rf "$tmp"; }
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

# The foreground sleep keeps the session alive while the test drives the job.
printf 'netmon lo --filter udp --port %s &\nsleep 3\n' "$PORT" |
    HOME="$tmp" "$NULLSH" >"$tmp/out" 2>"$tmp/err" &
shell_pid=$!

# The job line carries the pgid, which is the netmon child's own pid.
pgid=""
i=0
while [ "$i" -lt 30 ]; do
    pgid=$(sed -n 's/^\[1\] \([0-9][0-9]*\)$/\1/p' "$tmp/err" | head -n 1)
    if [ -n "$pgid" ]; then
        break
    fi
    sleep 0.1
    i=$((i + 1))
done

if [ -z "$pgid" ]; then
    echo "  FAIL netmon never announced a job, stderr was:"
    sed 's/^/        /' "$tmp/err"
    kill "$shell_pid" 2>/dev/null
    wait "$shell_pid" 2>/dev/null
    exit 1
fi

# The job line is printed by the parent right after the fork, so the child
# still needs a moment to have the socket bound.
sleep 0.5
send_udp
sleep 0.5

# SIGINT, not SIGTERM: the handler is what makes netmon print its summary.
kill -INT "$pgid" 2>/dev/null
sleep 0.5
wait "$shell_pid" 2>/dev/null

check_contains "the udp datagram is decoded" "$tmp/out" \
    "^IP 127\.0\.0\.1:[0-9][0-9]* > 127\.0\.0\.1:$PORT UDP len [0-9]"
check_contains "the summary lands on stderr" "$tmp/err" \
    "^[0-9][0-9]* packets, [0-9][0-9]* shown, [0-9][0-9]* malformed\$"

exit $fail
