#!/bin/sh
# End to end tests for the emu builtin: the headless framebuffer dump of a
# hand assembled rom, and the five ways emu refuses to run.
# Usage: 07_emu.sh <path to nullsh binary>, supplied by make test.

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

run_headless() {
    HOME="$tmp" NSH_EMU_HEADLESS="$1" "$NULLSH" >"$tmp/out" 2>"$tmp/err"
}

# 60 05  V0 = 5          61 00  V1 = 0
# F0 29  I = font for V0 (the 5 glyph)
# D1 15  draw 5 rows at (V1, V1)
# 12 08  jump to itself at 0x208
printf '\140\005\141\000\360\051\321\025\022\010' >"$tmp/rom.ch8"

dots() { printf "%${1}s" '' | tr ' ' '.'; }
row_f0="####$(dots 60)"
row_80="#$(dots 63)"
row_10="$(dots 3)#$(dots 60)"
blank="$(dots 64)"

# 1. A frame is exactly 32 rows of 64 cells.
printf 'emu %s/rom.ch8\n' "$tmp" | run_headless 100
check "the dump is 32 lines" "$(wc -l <"$tmp/out" | tr -d ' ')" "32"
check "every line is 64 cells wide" \
    "$(awk '{ print length($0) }' "$tmp/out" | sort -u | tr '\n' ' ')" "64 "

# 2. The five rows of the font glyph for 5: F0 80 F0 10 F0.
check "row 1 is the F0 bar" "$(sed -n 1p "$tmp/out")" "$row_f0"
check "row 2 is the 80 stem" "$(sed -n 2p "$tmp/out")" "$row_80"
check "row 3 is the F0 bar" "$(sed -n 3p "$tmp/out")" "$row_f0"
check "row 4 is the 10 stem" "$(sed -n 4p "$tmp/out")" "$row_10"
check "row 5 is the F0 bar" "$(sed -n 5p "$tmp/out")" "$row_f0"
check "nothing is lit below the glyph" \
    "$(sed -n '6,32p' "$tmp/out" | sort -u)" "$blank"
check "the dump goes to stdout only" "$(wc -c <"$tmp/err" | tr -d ' ')" "0"

# 3. The infinite loop at the end means more cycles paint the same picture.
cp "$tmp/out" "$tmp/first"
printf 'emu %s/rom.ch8\n' "$tmp" | run_headless 4000
if cmp -s "$tmp/first" "$tmp/out"; then
    echo "  ok   the frame is stable once the rom reaches its loop"
else
    echo "  FAIL 4000 cycles painted a different frame"
    fail=1
fi

# 4. Zero cycles leaves the screen dark, and the status is 0.
printf 'emu %s/rom.ch8\necho $?\n' "$tmp" | run_headless 0
check "zero cycles draws nothing" "$(sed -n '1,32p' "$tmp/out" | sort -u)" "$blank"
check "a clean run leaves 0" "$(tail -n 1 "$tmp/out")" "0"

# 5. A rom that is not there.
printf 'emu %s/no_such_rom\necho $?\necho still here\n' "$tmp" | run_headless 10
check_contains "a missing rom is reported" "$tmp/err" "^nullsh: emu: .*no_such_rom: "
check "a missing rom leaves 1" "$(sed -n 1p "$tmp/out")" "1"
check "the shell survives a missing rom" "$(sed -n 2p "$tmp/out")" "still here"

# 6. A rom too large for the 3584 bytes below 4096.
dd if=/dev/zero of="$tmp/big.ch8" bs=1 count=5000 2>/dev/null
printf 'emu %s/big.ch8\necho $?\n' "$tmp" | run_headless 10
check_contains "an oversized rom is reported" "$tmp/err" "^nullsh: emu: .*big.ch8: rom is too large"
check "an oversized rom leaves 1" "$(sed -n 1p "$tmp/out")" "1"

# 7. An empty rom has nothing to execute.
: >"$tmp/empty.ch8"
printf 'emu %s/empty.ch8\necho $?\n' "$tmp" | run_headless 10
check_contains "an empty rom is reported" "$tmp/err" "^nullsh: emu: .*empty.ch8: empty rom"
check "an empty rom leaves 1" "$(sed -n 1p "$tmp/out")" "1"

# 8. Misuse: no rom at all, and two roms.
printf 'emu\necho $?\n' | run_headless 10
check_contains "no argument prints a usage line" "$tmp/err" "^nullsh: emu: usage: emu ROMFILE\$"
check "no argument leaves 1" "$(sed -n 1p "$tmp/out")" "1"

printf 'emu %s/rom.ch8 %s/rom.ch8\necho $?\n' "$tmp" "$tmp" | run_headless 10
check_contains "two roms print a usage line" "$tmp/err" "^nullsh: emu: usage: emu ROMFILE\$"
check "two roms leave 1" "$(sed -n 1p "$tmp/out")" "1"

# 9. A cycle count that is not a number.
printf 'emu %s/rom.ch8\necho $?\n' "$tmp" | run_headless "twelve"
check_contains "a bad cycle count prints a usage line" "$tmp/err" \
    "^nullsh: emu: usage: emu ROMFILE\$"
check "a bad cycle count leaves 1" "$(sed -n 1p "$tmp/out")" "1"

printf 'emu %s/rom.ch8\necho $?\n' "$tmp" | run_headless "12x"
check "a trailing character in the cycle count leaves 1" \
    "$(sed -n 1p "$tmp/out")" "1"

# 10. Without the headless variable emu wants a terminal, and this harness
# feeds the shell from a pipe.
printf 'emu %s/rom.ch8\necho $?\necho still here\n' "$tmp" | run
check_contains "emu refuses without a terminal" "$tmp/err" \
    "^nullsh: emu: needs a terminal\$"
check "no terminal leaves 1" "$(sed -n 1p "$tmp/out")" "1"
check "the shell survives the refusal" "$(sed -n 2p "$tmp/out")" "still here"

exit $fail
