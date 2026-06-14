# nullsh architecture

Updated at the end of every phase. Current as of: Phase 8 (polish).

## Big picture

nullsh is a single binary. The REPL in `main.c` drives a pipeline of small modules:

```
read line -> lexer -> expand -> parser -> exec
                                            |- builtin dispatch (in-process)
                                            |- fork/exec external programs
```

Everything allocates through `src/alloc/` (`nsh_malloc` and friends). Until Phase 2 those are checked wrappers over libc malloc; after Phase 2 they are a real allocator over mmap and the wrapper is gone. Call sites never change.

The learning tools (`inspect`, `netmon`, `emu`, `heap`) are builtins. They live in their own directories and touch the shell only through the builtin dispatch table, so each one is a self-contained subsystem.

## Module map

| Directory | Owns | Status |
|---|---|---|
| `src/alloc/` | nsh_* over mmap arenas, firstfit + buddy strategies, guard canaries, heap builtin | done |
| `src/util/` | Str, Vec, line reader, NshError | done |
| `src/shell/` | lexer, expand, parser, exec, spawn, redirect, jobs, signals, builtins, history | done |
| `src/inspect/` | elf.c defensive parser, print.c formatting, inspect builtin | done |
| `src/emu/` | chip-8 cpu (pure), display renderers, keypad map, raw terminal, emu loop | done |
| `src/netmon/` | AF_PACKET capture, defensive decode, filter, print, netmon builtin | done |
| `tests/` | harness.h, harness_selftest.c, demo.sh, integration/ scripts | done |

`src/main.c` owns the REPL: prompt, `line_read`, lex, parse, exec, the job reap
cycle before each prompt, and loading and saving `~/.nullsh_history`. Unit tests
do not live in `tests/`; each one sits beside its module as `foo_test.c`.

## Decisions and why

- **Flat pipeline grammar, no AST.** The shell grammar is `pipeline := cmd ('|' cmd)* ['&']` plus redirects. A vector of Command structs represents it fully. An AST earns its keep only with `&&`, `if`, subshells, which are out of scope.
- **Builtins run in-process only when alone.** A builtin inside a pipeline forks like an external command; only a lone builtin mutates the shell, with its redirects applied around it via fd save/restore. This keeps `cd` correct and pipelines uniform.
- **Job control is terminal ownership, not scheduling.** Every launch lands in its own process group (parent and child both setpgid to close the race); the terminal's foreground group is swapped with tcsetpgrp around each foreground wait and always swapped back, even on a stop. Only the handoff is tty-gated: groups and reaping work in scripts too, which is a documented divergence from bash (which disables job control when non-interactive).
- **The SIGCHLD handler sets one flag.** All real work (waitpid WNOHANG|WUNTRACED|WCONTINUED loop, job table updates, Done notifications) happens in the REPL before each prompt, because almost nothing is async-signal-safe.
- **Pipe fds are tracked in one flat array and closed everywhere.** Each child closes every pipe fd after wiring its own two; the parent closes all of them after the fork loop. A middle pipeline child sees exactly fds 0, 1, 2, verified by the /proc/self/fd probe in review.
- **One global for jobs, one for signal flags.** POSIX forces both. Everything else is passed as explicit arguments; there is no shell "context singleton".
- **Errors are NshError return codes.** Functions that can fail return NshError and write results through out-params. The REPL prints `nsh_error_str()` messages and never exits on user error.
- **Tests link module objects, not the whole shell.** Each `foo_test.c` builds with just what it needs, which keeps modules honestly decoupled; main.c is excluded from test links.
- **Out of memory aborts.** The nsh_* allocators never return NULL; a failed allocation prints and aborts. Call sites stay clean and a shell that cannot allocate a prompt buffer has nothing useful left to do anyway.
- **Expansion happens at execution time, not parse time.** The parser stores unexpanded word tokens; exec expands them against the current environment and `$?`. No word splitting after expansion: a variable holding spaces stays one argv entry (a documented nullsh simplification).
- **The parser consumes the token list.** Word tokens move into Command structs, operator tokens are freed, and the list is left valid and empty. One owner at every moment, which is why the suite stays clean under ASan.
- **PATH search is hand rolled in the child.** Not execvp: the error accounting is the lesson. Only a search that hit nothing but EACCES reports 126; a non-runnable file (ENOEXEC) stops the search rather than being masked as not found.
- **One arena per strategy, frees route by address.** `heap strategy buddy` switches where new allocations come from; existing blocks free back to whichever arena contains them. No memory ever moves, so live pointers stay valid across a switch.
- **The allocator guards itself because ASan cannot.** ASan interposes libc malloc, which nullsh no longer uses, so alloc.c wraps every block in canaries (checked on free and realloc, abort on mismatch) and poisons freed payloads with 0xDD. The suite proves both by corrupting blocks in sacrificial forked children.
- **make test runs everything twice**, once per strategy via NSH_ALLOC_STRATEGY, so every shell test doubles as an allocator test.
- **A word is a list of quoting segments, not a string.** `token.h` gives each `Token` a vector of `WordSeg`, each with its own `expand` flag, so `"a"'b'c` is three segments and expansion applies per segment. Single quotes stay literal without a second lexing pass, and the parser never has to know how a word was quoted.
- **The controlling terminal lives on a high fd.** `main.c` dups stdin to fd 10 or above with `F_DUPFD_CLOEXEC` at startup. A lone builtin's redirect of fd 0 would otherwise cost the shell the terminal it needs for `tcsetpgrp`.
- **inspect maps the file read only and copies out what it keeps.** `elf_open` mmaps, validates every offset and size against the file length before following it, and copies section and symbol names into nsh-allocated strings. Symbols come from `.symtab` and fall back to `.dynsym`, with a flag recording which, because a stripped PIE binary only has the latter. 64-bit little-endian only; anything else is a clean NSH_ERR_INVALID.
- **The CHIP-8 core is pure.** `cpu.c` has no I/O, no clock and a seeded xorshift32 generator, and the quirk set it commits to is written down at the top of `cpu.h`. That is what makes opcode-by-opcode unit tests possible. It is also why `emu` has a headless mode: `NSH_EMU_HEADLESS=N` runs exactly N cycles with no terminal, no timers and no sleeping, then dumps the framebuffer as ASCII, so a rom always paints the same frame in a test.
- **The emulator restores the terminal from inside the handler.** `term_emergency_restore` uses only `tcsetattr`, `fcntl` and `write`. The SIGTSTP handler restores cooked mode, re-raises under `SIG_DFL`, then re-arms itself, so Ctrl-Z out of `emu` never leaves a wrecked terminal behind, and SIGCONT sets a flag the loop turns back into raw mode.
- **An interrupted capture read is not an error.** `capture_recv` returns 0 on EINTR so the netmon loop re-checks its stop flag; the SIGINT handler is installed without SA_RESTART for exactly that reason, and netmon restores the previous disposition on the way out because in the foreground it is running inside a shell that ignores SIGINT.
- **Non-IPv4 frames decode successfully.** `decode_frame` sets `has_ip` false for ARP, IPv6 and anything else rather than failing, so netmon's malformed counter means malformed and not merely uninteresting.
- **History records the raw line before parsing.** A line that fails to lex is still recallable, and a repeat of the newest entry is dropped so holding Enter does not fill the ring.

## Build

`make` (release), `make debug` (ASan+UBSan, -g -O0), `make test` (build debug, run all unit test binaries, then integration scripts, once per allocator strategy), `make test-net` (the one integration script that needs a raw socket), `make demo` (replay the README transcript and diff it), `make clean`. Objects live under `build/`, never beside sources.
