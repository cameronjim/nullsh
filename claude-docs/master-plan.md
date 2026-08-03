# nullsh — Master Plan

## Context

CJ (computer engineering student) wants to build **nullsh**: an educational Unix shell in pure C17 with zero dependencies, as a portfolio piece for learning systems programming. It ships a real shell (pipes, redirection, job control), a custom memory allocator that replaces malloc for the whole process, an ELF binary inspector (`inspect`), a packet-header viewer (`netmon`), and a CHIP-8 emulator (`emu`).

Decisions made with the user:
- **Environment**: this machine is Windows 10 with no WSL. Phase 0 installs WSL2 + Ubuntu. All compilation, testing, and execution happen inside WSL. The repo lives at `~/code/nullsh` **inside the WSL filesystem** (not `/mnt/c/...` — 9P filesystem I/O is slow and breaks some POSIX semantics like file locking; the Windows path `C:\Users\CJ\code\nullsh` can hold a README pointer or be retired).
- **Build order**: shell core first (living program from day one), allocator second, then pipes/redirects, job control, and the three learning tools.
- **Agent strategy**: planning/architecture thinking by Fable (main session); implementation fanned out to Opus agents where modules are independent.
- **Quality over quantity**: each major element gets its own plan document in-repo under `claude-docs/plans/`, written just-in-time before that element starts, seeded from the outlines below. We execute one element at a time.

## Ground rules (from the spec — bake into claude-docs/code-style.md)

- C17, `-std=c17 -Wall -Wextra -Werror -pedantic`, POSIX headers only, no third-party libs.
- Every change ships tests in the same commit. Homegrown assert harness; unit tests in `*_test.c` beside source; integration tests are shell scripts that drive nullsh and diff output.
- `make debug` = `-g -O0 -fsanitize=address,undefined`. Targets: `make`, `make test`, `make clean`, `make debug`, `make release`. Makefile only.
- No non-test file over ~500 lines. `//` comments only, single-line only (no stacked blocks), strictly minimal; one-line module header comment on every file. `#pragma once`.
- Explicit error handling: every syscall and allocation checked; errors propagate via return codes; the shell never segfaults on bad input.
- No global mutable state except signal flags (`volatile sig_atomic_t`) and the job list. `snake_case` functions, `PascalCase` structs, `SCREAMING_SNAKE` constants, module-name prefixes on public functions, `static` by default.
- No em dashes, no "utilize"/"leverage" in any prose.
- **Allocator caveat during Phase 0–1**: modules use `nsh_malloc` from day one, but until Phase 2 those are thin wrappers over libc malloc. Phase 2 replaces the internals; call sites never change.

## Phase 0 — Environment + repo skeleton (small, do once)

1. Install WSL2 + Ubuntu: `wsl --install -d Ubuntu` from elevated PowerShell, reboot, create Unix user, `sudo apt install build-essential gdb git`. Verify `gcc --version`, ASan works on a hello-world.
2. Claude Code runs against the WSL filesystem (open the repo from within WSL so Bash tool + Linux gcc are native).
3. `git init`, first commit: directory layout, Makefile, test harness, claude-docs.

Repo layout:
```
nullsh/
  Makefile
  src/
    main.c            // entry, REPL loop
    alloc/            // nsh_malloc (wrapper now, real later)
    shell/            // lexer, parser, exec, builtins, jobs, history
    inspect/          // ELF viewer
    netmon/           // packet viewer
    emu/              // CHIP-8
    util/             // nsh_string, dynamic array, error codes
  tests/
    harness.h         // ASSERT/RUN_TEST macros, main() generator
    integration/      // *.sh scripts + expected output
  claude-docs/
    architecture.md  code-style.md  testing.md
    plans/           // one plan doc per element, written just-in-time
```

Test harness (`tests/harness.h`): ~60 lines. `TEST(name)` registers a function, `ASSERT_EQ/ASSERT_TRUE/ASSERT_STR_EQ` macros print file:line on failure, a `main` runs all tests and exits nonzero on any failure. `make test` builds every `*_test.c` against its module and runs them, then runs `tests/integration/*.sh`.

Makefile: pattern rules, `SRCS := $(shell find src -name '*.c' ! -name '*_test.c')`, debug/release flag sets, per-module test binaries under `build/`.

## Phase 1 — Shell core (first element built; full plan doc: claude-docs/plans/01-shell-core.md)

**What it does**: prompt → read line → tokenize → parse → expand variables → find program on PATH → fork/exec → wait → prompt again. Plus `cd`, `exit`, `help`, `export`, `unset`, `history` builtins and line history.

**Key concepts to document in the plan doc**:
- fork/exec/wait model: `fork()` clones the process; child calls `execve()` which replaces its image with the new program; parent `waitpid()`s. Builtins run in the parent (a `cd` in a child would change the child's cwd and vanish).
- PATH resolution: split `$PATH` on `:`, try `execve` on each `dir/cmd`; distinguish ENOENT (keep looking) from EACCES (report).
- Exit status conventions: `WIFEXITED/WEXITSTATUS`, 127 for not-found, 126 for not-executable, `$?`.

**Modules** (each ≤500 lines, each with `*_test.c`):
- `shell/lexer.c` — line → token list. Handles single quotes (literal), double quotes (allow `$` expansion), backslash escapes, operators (`| < > >> 2> &` tokenized now, used in later phases). State machine, no regex.
- `shell/expand.c` — `$VAR`, `${VAR}`, `$?` expansion; happens inside double quotes, not single.
- `shell/parser.c` — token list → `Command` struct (argv, redirect slots, pipeline links, background flag). Grammar is flat: `pipeline := cmd ('|' cmd)*`, no need for a full AST.
- `shell/exec.c` — builtin dispatch table; fork/exec/waitpid; PATH search.
- `shell/builtin.c` — cd/exit/help/export/unset/history.
- `shell/history.c` — in-memory ring + `~/.nullsh_history` persistence. (Line editing with arrow keys = raw-terminal mode, deferred to a stretch phase; start with plain `fgets`-style reads via a checked read loop.)
- `util/` — `Str` growable string, `Vec` growable array, `nsh_error` enum + strings. Everything allocates via `nsh_malloc`.

**Tests**: lexer unit tests (quoting edge cases: `echo "a b"`, `echo 'a $HOME'`, unterminated quote → error not crash), parser tests, expand tests; integration scripts: run `echo hi`, `ls | head` (once pipes land), missing command prints error and exit 127.

**Exit criteria**: interactive prompt runs external commands with quoted args and env expansion; all builtins work; `make test` green under ASan.

## Phase 2 — Memory allocator (claude-docs/plans/02-allocator.md)

**What it does**: `nsh_malloc/free/realloc/calloc` backed by memory requested from the kernel directly (`sbrk` or `mmap`), with first-fit and buddy strategies switchable at runtime, plus the `heap` builtin (`stats`, `strategy`, `dump`).

**Key concepts for the plan doc**:
- Heap vs stack; `sbrk`/`mmap` as the only way userspace gets memory; malloc is a librarian, not a source.
- Block headers: size + free flag + strategy metadata stored *before* the pointer returned to the user; alignment to 16 bytes.
- First-fit free list: singly/doubly linked list of free blocks; split on allocate, **coalesce adjacent free blocks on free** (the classic bug source); fragmentation = free memory you can't use because it's in small pieces.
- Buddy system: pool of 2^k bytes; splitting a block yields two "buddies" whose addresses differ only in bit k, so finding your buddy for merging is `addr XOR size`. Internal fragmentation (rounding 33 bytes up to 64) vs external.
- Why realloc is tricky: grow-in-place when the next block is free, else allocate+copy+free.
- Poisoning freed memory (`0xDEADBEEF` fill) and canary bytes to catch use-after-free/overflow without ASan.

**Design**: `alloc/alloc.h` public API; `alloc/firstfit.c`, `alloc/buddy.c` behind a strategy vtable struct; `alloc/heap_builtin.c` for the shell command. Strategy switch drains only if the current arena is empty, otherwise new allocations go to the new strategy's arena while old blocks free back to their originating arena (each block header records its arena). Stats: bytes in use, free, block counts, largest free block, alloc/free counters.

**The swap**: nothing outside `alloc/` changes; the wrapper file is deleted and the real implementation takes over. Then the whole shell becomes the allocator's stress test.

**Tests**: unit tests for split/coalesce/alignment/realloc edge cases (size 0, NULL, shrink), randomized alloc/free soak test with a shadow tracking table, buddy XOR-merge tests; run entire existing integration suite on both strategies.

## Phase 3 — Pipes and redirection (claude-docs/plans/03-pipes-redirect.md)

**What it does**: `cmd1 | cmd2 | cmd3`, `>`, `>>`, `<`, `2>`, working for external commands **and** builtins (`inspect ... | grep ...`).

**Key concepts**:
- A pipe is a kernel byte buffer with a read fd and write fd (`pipe()`); `dup2(pipefd[1], STDOUT_FILENO)` rewires a child's stdout before exec. fd inheritance across fork is the whole mechanism.
- Why every unused pipe end must be closed in every process: a reader only sees EOF when *all* write ends are closed; forgotten fds cause hangs, the classic pipeline bug.
- Redirection = `open()` with the right flags (`O_TRUNC` vs `O_APPEND`) + `dup2`, done in the child so the parent's fds are untouched.
- Builtins in pipelines: a builtin mid-pipeline runs in a forked child (its side effects don't matter there); a builtin alone with redirects runs in the parent with fds saved/restored around it (so `cd /tmp > log` still changes the shell's dir).

**Modules**: `shell/redirect.c` (apply/save/restore fd sets), extend `exec.c` to walk the pipeline creating N-1 pipes and N children in one process group.

**Tests**: integration scripts are the star here: 3-stage pipelines, `2>` capturing stderr, append vs truncate, builtin piped into grep, a pipeline where the middle command fails.

## Phase 4 — Job control (claude-docs/plans/04-job-control.md)

**What it does**: `&` backgrounding, Ctrl-C/Ctrl-Z affecting only the foreground job, `jobs`, `fg`, `bg`, async "[1] Done" notifications.

**Key concepts (this is the "scheduling" story)**:
- The kernel schedules processes; the *shell* manages the terminal. Job control is about who owns the terminal, not who runs.
- Process groups: every pipeline becomes one group (`setpgid`); signals from the keyboard (SIGINT for Ctrl-C, SIGTSTP for Ctrl-Z) go to the **foreground process group** of the terminal, which the shell sets with `tcsetpgrp`. That's why Ctrl-C kills your program but not your shell.
- Sessions: the shell is session leader; the terminal has one foreground group at a time; background processes that read the terminal get SIGTTIN and stop.
- SIGCHLD + `waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED)` to reap and track state changes without blocking; handler only sets a `volatile sig_atomic_t` flag, the main loop does the real work (async-signal-safety: almost nothing is legal inside a handler).
- The race between fork and setpgid: both parent and child call `setpgid` to close it.
- `fg` = tcsetpgrp to the job's group + SIGCONT + blocking wait; `bg` = SIGCONT without terminal handover.
- The shell must ignore SIGINT/SIGTSTP/SIGTTOU itself and restore defaults in children.

**Modules**: `shell/jobs.c` (the one sanctioned global: job table with pgid, state, command string), `shell/signals.c` (handler installation, flag definitions), builtins fg/bg/jobs, hooks in exec.c and the REPL loop (check the reap flag before each prompt).

**Tests**: hardest to test; use integration scripts with `sleep` jobs driven via a pty helper or by scripting nullsh with here-docs plus `kill -INT/-TSTP` from outside; unit-test the job table logic directly.

## Phase 5 — `inspect`: ELF viewer (claude-docs/plans/05-inspect-elf.md)

**What it does**: reads an ELF file and prints file header, section headers, program headers, symbol table, string tables; `--sections`, `--symbols` flags. Long-term stretch: `.text` disassembly.

**Key concepts**:
- ELF is two parallel views of one file: **sections** (linker's view: `.text` code, `.data` initialized globals, `.bss` zeroed globals, `.rodata`, `.symtab`) and **segments/program headers** (loader's view: which byte ranges get mmapped where, with what permissions).
- The file header at offset 0: magic `\x7fELF`, class (32/64-bit), endianness, type (executable vs shared object vs relocatable — note modern `/usr/bin/ls` is ET_DYN because of PIE), entry point.
- String tables: names are stored as offsets into `.strtab`/`.shstrtab`, so parsing is "read header → find section header table → find shstrtab → resolve names".
- Read via `mmap` (fits the theme: the loader also mmaps), validate every offset/size against file length so a truncated or malicious file can't crash the shell. Define the structs ourselves from the spec rather than including `<elf.h>`... decision point: `<elf.h>` is a system header (allowed by the rules) but defining `Elf64Header` ourselves is more educational. Plan doc will pick: define our own, cross-check against `<elf.h>` in tests.

**Modules**: `inspect/elf.c` (parse + validate into structs), `inspect/print.c` (formatting), `inspect/inspect.c` (builtin entry, flag parsing). Output goes through stdout normally so pipelines work (already handled by Phase 3's builtin-in-pipeline support).

**Tests**: parse fixture binaries compiled in `make test` (a tiny static exe, a .o, a .so); golden-output diffs against expected text; fuzz-ish tests with truncated/corrupted headers asserting clean errors; compare section counts against `readelf -h` output in an integration script.

## Phase 6 — CHIP-8 emulator (claude-docs/plans/06-chip8.md)

(Ordered before netmon: no privileges needed, instantly gratifying, exercises the allocator and raw-terminal code that history line-editing can also reuse.)

**What it does**: `emu rom.ch8` runs a CHIP-8 program: 4KB memory, 16 8-bit registers, stack, two 60Hz timers, 64x32 monochrome display drawn with Unicode half-block characters, 16-key hex keypad mapped to `1234/qwer/asdf/zxcv`.

**Key concepts**:
- Fetch-decode-execute: each cycle reads 2 bytes at PC, decodes by nibble pattern (e.g. `8xy4` = add Vx+Vy with carry flag), executes, advances PC. This is what a real CPU does in silicon.
- Memory map: ROM loads at 0x200 (the first 512 bytes historically held the interpreter itself); font sprites live below 0x200.
- Sprite drawing is XOR: drawing over a set pixel clears it and sets the collision flag VF, which is how games detect hits.
- Timing: CPU runs ~500-1000 instructions/sec but timers tick at exactly 60Hz; decouple with a monotonic clock (`clock_gettime`).
- Terminal as display: raw mode via `tcsetattr` (no echo, no line buffering, nonblocking reads for the keypad), ANSI escape codes to reposition the cursor, `▀`/`▄`/`█` half-blocks to get 2 pixels per character cell. Restore the terminal on exit *and* on signals (integrates with job control: Ctrl-Z must restore cooked mode, fg must re-enter raw mode — SIGTSTP/SIGCONT handling inside a builtin).

**Modules**: `emu/cpu.c` (opcode interpreter, pure logic, no I/O — fully unit-testable), `emu/display.c`, `emu/keypad.c`, `emu/emu.c` (main loop, timing, terminal lifecycle).

**Tests**: cpu.c is a dream to test: load hand-assembled opcode sequences, step N cycles, assert register/memory/display state. Run public test ROMs (corax89 opcode test) headless and compare framebuffer hashes.

## Phase 7 — `netmon`: packet viewer (claude-docs/plans/07-netmon.md)

(Last: needs root, WSL2 network quirks, and depends on job control being solid since it runs as a backgroundable job.)

**What it does**: `netmon eth0 [--filter tcp|udp] [--port N]` opens a raw socket and prints one decoded line (or a header breakdown) per packet: Ethernet → IP → TCP/UDP layers.

**Key concepts**:
- Encapsulation: each layer wraps the next; parsing is peeling: Ethernet header (14 bytes: dst MAC, src MAC, EtherType) → EtherType 0x0800 says IPv4 → IP header (variable length via IHL field, protocol field says 6=TCP/17=UDP) → TCP header (ports, seq/ack, flags SYN/ACK/FIN/RST) or UDP (ports, length).
- `socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))` delivers whole frames from one interface; requires CAP_NET_RAW (hence sudo, or `setcap` on the binary — plan doc covers both and the security tradeoff).
- Network byte order: multi-byte fields on the wire are big-endian; `ntohs/ntohl` everywhere; this is *the* classic beginner bug.
- Never trust the wire: validate lengths at every layer before reading fields (same defensive-parsing discipline as ELF).
- WSL2 note: interface is usually `eth0` on a NAT'd virtual network; loopback capture and traffic generated from Windows-side apps behave differently; plan doc includes a "generate test traffic with curl/dig" section.
- Backgroundability: netmon is a long-running builtin; per the job-control design, long-running builtins fork, so it naturally becomes a job (`netmon eth0 &`, Ctrl-Z, fg all work for free).

**Modules**: `netmon/capture.c` (socket setup, read loop), `netmon/decode.c` (pure functions: bytes → structs, unit-testable with canned packet hex dumps), `netmon/print.c`, `netmon/netmon.c` (builtin entry, filters).

**Tests**: decode.c tested against hand-built and real captured byte arrays (store fixtures as hex in test files); filter logic unit tests; integration test needs root so it's a separate `make test-net` target that's skipped when not root.

## Phase 8 — Polish (claude-docs/plans/08-polish.md, later)

Raw-mode line editing (arrows, history recall) reusing emu's terminal code, `$(...)`? no — keep scope; README with asciinema-style demo text, final claude-docs pass, GitHub publish.

## Execution model per phase

For each phase: (1) Fable writes/refines the element's plan doc in `claude-docs/plans/`, (2) implementation split into independent modules and fanned out to Opus agents in parallel where files don't overlap (e.g. lexer/util/history are independent; exec depends on parser so it's sequenced), (3) each agent delivers module + `*_test.c` in the same change, (4) integration tests + ASan run + review pass by Fable before the phase closes, (5) update architecture.md.

## Verification

- Every phase ends with `make test` (ASan+UBSan debug build) green inside WSL.
- Manual smoke script per phase in the plan doc (exact commands to type at the nullsh prompt and expected output).
- Phase 2 onward: full suite runs under both allocator strategies.
- Definition of done for the project: the README demo transcript (`inspect /usr/bin/ls | grep .text`, `netmon eth0 --filter udp --port 53 &`, `jobs`, `fg`, `emu tetris.ch8`, `heap stats`) executes exactly as written.
