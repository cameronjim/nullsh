# Phase 6: emu, the CHIP-8 virtual machine

## Goal

`emu rom.ch8` runs CHIP-8 programs in the terminal: 64x32 display drawn with Unicode half blocks, 16-key hex keypad on 1234/qwer/asdf/zxcv, sound and delay timers at 60 Hz, every standard opcode. ESC leaves. Ctrl-Z suspends and restores the terminal correctly (job control integration).

## Concepts this phase teaches

### Fetch, decode, execute

A CPU is a loop: read the 2 bytes at PC, split them into nibbles, branch on the pattern, mutate registers/memory, advance PC. CHIP-8 has 35 opcodes, 16 8-bit registers (V0..VF), an index register I, a 16-deep call stack, and 4 KB of memory. Writing the interpreter demystifies what silicon does.

### The memory map is history

ROMs load at 0x200 because the first 512 bytes of a real 1970s COSMAC VIP held the interpreter itself. The built-in font sprites (0-F) live below 0x200 (we use 0x50, the common convention).

### XOR drawing and the collision flag

Sprites draw by XOR: drawing over a lit pixel turns it off and sets VF=1. Games detect hits with it, and it makes erasing a sprite the same operation as drawing it.

### Decoupled time

The CPU runs at roughly 700 instructions/sec but both timers tick at exactly 60 Hz. One loop, two clocks, reconciled with clock_gettime(CLOCK_MONOTONIC). Running opcodes faster must not make timers faster; the tests pin this.

### The terminal is the display

Raw mode via tcsetattr (no echo, no line buffering, nonblocking reads), ANSI escapes to home the cursor, two vertical pixels per character cell using half blocks. The terminal MUST be restored on every exit path: normal quit, Ctrl-Z (SIGTSTP), resume (SIGCONT), and Ctrl-C. A shell tool that wrecks the terminal is a failed phase.

### Quirks are documented decisions

CHIP-8 dialects disagree. nullsh picks: 8xy6/8xyE shift VX itself (VY ignored), Fx55/Fx65 leave I unchanged, BNNN jumps to NNN+V0, sprites drawing at the screen edge clip (no wrap of the sprite body; the start coordinate wraps modulo 64/32). Stated in cpu.h and asserted by tests.

## Contracts (cpu.h written by Fable, committed with this plan)

Chip8 struct with all state including framebuffer, key bitmask, Fx0A wait latch, and an xorshift rng (no rand(): determinism for tests). cpu_init / cpu_load_rom / cpu_step / cpu_tick_timers / cpu_key_event. cpu.c does ZERO I/O: no reads, no prints, no time. That is what makes it exhaustively testable.

## Waves (parallel clones)

- Agent A: src/emu/cpu.c + cpu_test.c. Every opcode tested by hand-assembling short programs into memory and stepping: arithmetic with carry/borrow VF semantics, shifts per our quirks, BCD (Fx33), memory load/store leaving I fixed, jump/call/ret and stack overflow/underflow as NSH_ERR_INVALID, Fx0A blocking latch, DXYN clipping and collision, timer decrement, deterministic CXKK via the seeded rng, bad opcode rejection, PC out of bounds rejection.
- Agent B: src/emu/display.c/h (render the framebuffer into a caller Str/buffer using half blocks, plus diff-friendly full redraw; pure, testable), src/emu/keypad.c/h (char to key mapping, pure), src/emu/term.c/h (raw mode enter/exit, SIGTSTP/SIGCONT/atexit restoration; thin, reviewed rather than unit-tested).
- Wave 2 (single agent, main repo): src/emu/emu.c main loop + builtin registration + Makefile objects + integration test. Headless mode for tests: env NSH_EMU_HEADLESS=<cycles> runs the ROM that many cpu steps with no terminal, then prints the framebuffer as 32 lines of # and . and exits; the integration script assembles a tiny ROM with printf (draw two sprites, loop) and diffs the expected picture. ESC handling, 700 Hz pacing, 60 Hz timers in the interactive path.

## Exit criteria

Dual-pass suite green; headless framebuffer test exact; Fable reviews term.c restoration paths and runs a real ROM in a tty; notes.md and architecture.md updated; lowercase tldr commit.
