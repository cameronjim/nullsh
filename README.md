# nullsh

nullsh is an educational Unix shell written in pure C17 with zero third-party
dependencies. It exists so that every layer under a running program can be read
instead of assumed: process creation and PATH resolution, a memory allocator
that replaces malloc for the entire process, pipes and file descriptor plumbing,
terminal ownership and job control, an ELF file parser, a CHIP-8 CPU emulator,
a raw-socket packet decoder, and a DNS client that writes its own query bytes.
Everything it needs it builds itself, so there is no library boundary to stop
reading at.

`docs/manual.md` is the user manual: every piece of syntax, every key binding,
every builtin and every deliberate limitation, in tables.

## Build and run

```
make                 # release build, -O2, produces build/nullsh
./build/nullsh       # start the shell
make debug           # -g -O0 with AddressSanitizer and UBSan, build/nullsh-debug
make test            # build debug, run every unit test, then the integration scripts
sudo make test-net   # the one integration test that needs a raw socket
make demo            # replay the transcript below and fail if the README has rotted
make clean           # delete build/
```

Linux only. nullsh calls `mmap`, `AF_PACKET`, `tcsetpgrp`, `setpgid` and the
Linux `/proc` conventions directly, so it does not build on macOS or BSD. It was
developed and tested on Ubuntu 24.04 under WSL2 with gcc 13.3. Objects and
binaries all land under `build/`, never beside the sources.

## Demo

Everything below is a real session, copied out of a pty recording. Process ids,
heap byte counts and ELF addresses differ from run to run. Everything else is
byte for byte what nullsh prints.

```
nullsh:~$ echo hello world | tr a-z A-Z
HELLO WORLD
nullsh:~$ echo one > notes.txt
nullsh:~$ echo two >> notes.txt
nullsh:~$ cat < notes.txt | wc -l
2
nullsh:~$ sleep 3 &
[1] 426
nullsh:~$ jobs
[1]  Running  sleep 3 &
nullsh:~$ fg
sleep 3 &
[1]  Done  sleep 3 &
nullsh:~$ heap stats
strategy       firstfit
arena size     16777216
used bytes     10544
free bytes     16766656
live blocks    27
free blocks    3
largest free   16766544
total mallocs  216
total frees    189
nullsh:~$ heap strategy buddy
nullsh:~$ heap strategy
buddy
nullsh:~$ inspect --sections /usr/bin/ls | grep .text
[16]  .text                 PROGBITS    0x0000000000004d70  0x00004d70  0x00014032  AX
nullsh:~$ printf '\140\005\141\000\360\051\321\025\022\010' > five.ch8
nullsh:~$ export NSH_EMU_HEADLESS=100
nullsh:~$ emu five.ch8 | head -6
####............................................................
#...............................................................
####............................................................
...#............................................................
####............................................................
................................................................
nullsh:~$ netmon
nullsh: netmon: usage: netmon IFACE [--filter tcp|udp] [--port N]
nullsh:~$ exit
```

Notes on the last three commands. The ten bytes written to `five.ch8` are a
hand assembled CHIP-8 program that loads the font glyph for the digit 5 and
draws it at the top left, then jumps to itself. `NSH_EMU_HEADLESS=100` tells
`emu` to run exactly 100 cycles with no terminal, no timers and no sleeping,
then dump the 64 by 32 framebuffer as ASCII, which is what makes the picture
reproducible. Without that variable `emu` takes over the terminal and runs at
about 700 instructions per second until you press Esc.

`netmon` with no arguments prints its usage line, which is all it can do as a
normal user. A real capture opens a raw `AF_PACKET` socket and needs root:

```
sudo ./build/nullsh
nullsh:~$ netmon eth0 --filter udp --port 53
```

Without root it refuses cleanly rather than failing halfway:

```
nullsh:~$ netmon eth0
nullsh: netmon: eth0: needs root, try sudo nullsh
```

`make demo` reruns every command in the transcript against the built shell and
compares the deterministic parts, so the transcript cannot quietly go stale.

## What is inside

### Shell core

`src/shell/lexer.c`, `expand.c`, `parser.c`, `exec.c`, `spawn.c`, `builtin.c`,
`history.c`. A line becomes tokens, tokens become a syntax tree, each pipeline
in that tree is a vector of `Command` structs, and each command becomes a `fork`
plus a hand rolled PATH search ending in `execve`. Teaches the fork and exec
model: the child is a copy of the parent until `execve` replaces its image,
which is why `cd` has to run in the parent and why the exit status conventions
(127 for not found, 126 for found but not runnable) exist at all. Quoting is
tracked per run inside a word, so `"a"'b'c` carries three segments and only the
expandable ones see `$VAR`.

### Pipes and redirection

`src/shell/redirect.c` plus the pipeline loop in `exec.c`. Teaches file
descriptor inheritance and the close discipline. `pipe()` hands back two fds,
`dup2` rewires a child's 0, 1 or 2 before `execve`, and fork copies the table to
every child. The part that bites people is closing: a reader sees EOF only when
every write end in every process is closed, so each child closes all pipe fds
after wiring its own two and the parent closes all of them after the fork loop.
A middle stage of a pipeline ends up holding exactly fds 0, 1 and 2.

### Job control

`src/shell/jobs.c`, `signals.c`, `spawn.c`. Teaches that job control is about
terminal ownership, not scheduling. Every pipeline gets its own process group.
The terminal has one foreground process group at a time, set with `tcsetpgrp`,
and the tty driver sends keyboard signals only to that group. That single fact
is why Ctrl-C kills your command and spares the shell. Parent and child both
call `setpgid` to close the race between them, the SIGCHLD handler sets one flag
and nothing else, and the REPL does the real `waitpid` work before each prompt
because almost nothing is safe to call inside a handler.

### The interpreter

`src/shell/ast.c`, `parser.c`, `eval.c`, `func.c`, `run.c`. This is what makes
nullsh a small language instead of a command runner: `;`, `&&`, `||`, `!`,
`if`/`elif`/`else`, `while`, `for`, functions with arguments, `break`,
`continue`, `return`, comments, multi-line input and script files with
positional parameters. Teaches why a tree and not a list. `a | b` is a sequence,
so a vector holds it completely, but `a && b` is a decision, whether `b` runs
depends on the result of `a`, and decisions nest arbitrarily deep. Recursive
descent turns the tokens into nodes that know their children and the evaluator
walks it top down. Three details carry most of the lesson. Keywords are
contextual, so the lexer stays dumb and only the parser decides that a word in
command position is `if`, which is why `echo if` prints "if". Unfinished is not
invalid, so running out of tokens returns a distinct error that becomes the `> `
continuation prompt instead of a complaint. And `break` deep inside an `if`
inside a `while` cannot be a return value, so it sets a flag on the shell that
every list evaluator checks and the loop consumes, which is exactly what real
shells do.

### The allocator

`src/alloc/alloc.c`, `firstfit.c`, `buddy.c`, `heap_builtin.c`. Teaches what
malloc actually is. Memory comes from the kernel once, as a 16 MiB `mmap` arena
per strategy, and the allocator is pure bookkeeping on top. First-fit threads a
free list through the free memory itself, splits on allocate and coalesces
neighbors on free. Buddy rounds every request up to a power of two and finds a
block's partner with `addr XOR size`, which makes merging arithmetic instead of
searching. Each block sits between canary bytes that are checked on free, and
freed payloads are poisoned, because AddressSanitizer interposes on libc malloc
and cannot see inside an arena nullsh mapped itself. The whole shell runs on
this allocator: every string, token, job and framebuffer in the process comes
out of it, so `heap stats` moves while you use the shell and `heap strategy
buddy` switches where new blocks come from without moving a single live pointer.

### inspect

`src/inspect/elf.c`, `print.c`, `inspect.c`. Teaches that an ELF file carries
two parallel descriptions of itself: sections, which are the linker's view
(`.text`, `.data`, `.bss`, `.symtab`), and segments, which are the loader's view
of which byte ranges get mapped where with which permissions. `--sections` and
`--segments` print them side by side so the difference is concrete. The parser
maps the file read only and validates every offset and length against the file
size before following it, which is the same defensive discipline netmon needs,
practiced on a file instead of a wire.

### emu

`src/emu/cpu.c`, `display.c`, `keypad.c`, `term.c`, `emu.c`. A CHIP-8 virtual
machine. Teaches fetch, decode, execute: read two bytes at the program counter,
switch on the nibbles, act, advance. Sprites are drawn by XOR, so drawing over a
lit pixel clears it and raises the collision flag, which is how CHIP-8 games do
hit detection. There are two clocks, not one: instructions run at about 700 Hz
while the delay and sound timers tick at exactly 60 Hz, and keeping them
separate against a monotonic clock is the whole timing lesson. `cpu.c` is pure
logic with no I/O, no time and a seeded xorshift generator, which is why it can
be unit tested opcode by opcode.

### netmon

`src/netmon/capture.c`, `decode.c`, `filter.c`, `print.c`, `netmon.c`. Teaches
encapsulation by peeling it apart: an Ethernet header says the payload is IPv4,
the IPv4 header says the payload is TCP or UDP, and each layer's length field
decides where the next one starts. Multi-byte fields on the wire are big-endian
regardless of the host, and every read assembles them with explicit byte
shifts, because writing the shifts is the lesson `ntohs` would hide. Capture
uses `socket(AF_PACKET, SOCK_RAW, ...)`, which needs CAP_NET_RAW and therefore
root. Nothing off the wire is trusted: every layer is bounds checked before a
field is read, and a frame that does not add up is counted as malformed instead
of crashing the shell.

### resolve

`src/resolve/dns.c`, `net.c`, `resolve.c`. Teaches a protocol from the client
side: netmon watches other programs' packets, resolve makes its own. The query
is built byte by byte from RFC 1035 (length-prefixed labels, big-endian fields
written with the same explicit shifts netmon reads with) and sent over UDP,
which is allowed to lose it, so the client owns the timeout and the resend.
Reply parsing survives the format's famous trap, compression pointers, by
requiring every pointer to jump strictly backwards and capping one name at 32
hops, so a hostile packet cannot loop the parser. Run netmon on the interface
in a background job and resolve in the foreground to watch your own query
cross the wire.

## Code rules

- Pure C17, built with `-std=c17 -Wall -Wextra -Werror -pedantic`, clean on gcc
  and clang.
- POSIX system headers and the C standard library only. No third-party code.
- All allocation goes through `nsh_malloc`, `nsh_free`, `nsh_realloc` and
  `nsh_calloc`. Nothing outside `src/alloc/` calls libc malloc.
- Makefile only. No generator, no build system.
- No non-test file over 500 lines. Headers use `#pragma once`. `static` is the
  default and only the public API reaches a header.
- No global mutable state except where POSIX forces it: the signal flags and the
  job table.
- `snake_case` functions, `PascalCase` structs, `SCREAMING_SNAKE` constants.
  Module-public functions carry their module prefix.
- `//` comments only, one line each, never stacked into paragraphs. Every `.c`
  and `.h` opens with exactly one comment line naming what the module does. A
  comment has to earn its place with an invariant, a platform quirk or a syscall
  behavior being relied on.
- Every syscall checked, every error propagated as an `NshError` return code.
  nullsh never segfaults on bad input.
- Every change ships its tests in the same commit.
- Prose stays direct and specific. No em dashes, no "utilize", no "leverage".

The full versions live in `claude-docs/code-style.md`,
`claude-docs/architecture.md` and `claude-docs/testing.md`.

## Layout

```
nullsh/
  Makefile
  README.md
  src/
    main.c              startup and teardown, then over to the drivers in run.c
    alloc/              nsh_malloc over mmap arenas, firstfit, buddy, heap builtin
    shell/              lexer, expand, parser, ast, eval, func, run, exec,
                        spawn, redirect, jobs, signals, builtins, history
    inspect/            ELF parser, formatting, the inspect builtin
    emu/                CHIP-8 cpu, display, keypad, raw terminal, emu builtin
    netmon/             AF_PACKET capture, decode, filter, print, netmon builtin
    resolve/            DNS wire codec, UDP exchange, the resolve builtin
    util/               Str, Vec, line reader, NshError
  tests/
    harness.h           the whole test framework
    harness_selftest.c  tests for the framework itself
    demo.sh             replays the README transcript
    integration/        shell scripts that drive the built shell and diff output
  docs/
    manual.md           the user manual
  claude-docs/
    architecture.md  code-style.md  testing.md  plans/
```

Unit tests are not in `tests/`. Each one sits beside the code it covers as
`foo_test.c`, and links only the objects that module actually needs.

## Tests

`make test` builds the debug binary and then runs the whole suite twice, once
with `NSH_ALLOC_STRATEGY=firstfit` and once with `buddy`, so every shell test
doubles as an allocator test. One pass is:

- 761 unit test cases across 31 test binaries.
- 377 integration checks across 12 scripts in `tests/integration/`.

That is 1138 checks per pass and 2276 per `make test`. `sudo make test-net` adds
2 more from `tests/integration/08_netmon.sh`, which is separate because it opens
a raw socket. The pty-driven job control script skips itself rather than failing
when `script(1)` or `ps --ppid` is missing.

## Limitations

These are deliberate. nullsh is a shell for learning how a shell works, not a
replacement for bash. The complete list is the limitations table at the end of
`docs/manual.md`.

- No globbing. `echo *.txt` prints `*.txt`.
- No subshells, no command substitution, no arithmetic, no `case` or `until`,
  no `$@` or `$*`. The scripting language stops where the lesson stops.
- Compound commands stand alone. An `if` or a loop takes no redirections, does
  not pipe, and does not chain with `&&`, and a list cannot be backgrounded.
  Operators connect pipelines only.
- Functions cannot shadow builtins. Dispatch checks builtins first, so the
  shell can never be locked out of its own controls.
- No `~user` form.
- No word splitting after expansion. A variable holding spaces stays one argv
  entry.
- No shell variable table. `export NAME=VALUE` writes straight to the
  environment, and a bare `export NAME` does nothing.
- `inspect` reads 64-bit little-endian ELF only. A 32-bit or big-endian file is
  rejected with a message, not parsed halfway.
- `netmon` decodes IPv4 only, over Ethernet, for TCP and UDP. No IPv6, no ARP
  detail, no promiscuous mode, and no BPF filtering in the kernel. Filtering
  happens after the frame is decoded.
- Interactive line editing covers arrow keys and history recall. It is not
  readline, and there is no completion.
- Out of memory aborts. `nsh_malloc` never returns NULL, so call sites stay
  clean, and a shell that cannot allocate a prompt buffer has nothing useful
  left to do.
