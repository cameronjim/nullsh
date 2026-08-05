# nullsh architecture

Updated at the end of every phase. Current as of: Phase 9 (the interpreter).

## Big picture

nullsh is a single binary. The read-eval drivers in `shell/run.c` drive a pipeline of small modules:

```
read line(s) -> lexer -> parser -> AST -> eval -> exec
                                          |       |- builtin dispatch (in-process)
                                          |       |- function call (in-process, or in the stage child)
                                          |       |- fork/exec external programs
                                          |- control flow flags, loop depth, the function table
```

Expansion is not a stage of its own: `exec` calls into `expand.c` per word at the moment a command runs.

Everything allocates through `src/alloc/` (`nsh_malloc` and friends). Until Phase 2 those are checked wrappers over libc malloc; after Phase 2 they are a real allocator over mmap and the wrapper is gone. Call sites never change.

The learning tools (`inspect`, `netmon`, `emu`, `heap`) are builtins. They live in their own directories and touch the shell only through the builtin dispatch table, so each one is a self-contained subsystem.

## Module map

| Directory | Owns | Status |
|---|---|---|
| `src/alloc/` | nsh_* over mmap arenas, firstfit + buddy strategies, guard canaries, heap builtin | done |
| `src/util/` | Str, Vec, line reader, NshError | done |
| `src/shell/` | lexer, expand, parser, exec, spawn, redirect, jobs, signals, builtins, history, line editing | done |
| `src/shell/` (interpreter) | ast.c the tagged-union tree plus deep clone, parser.c recursive descent with INCOMPLETE detection, eval.c the tree walker and control flow, func.c the function table, run.c the read-eval drivers for all three input styles | done |
| `src/inspect/` | elf.c defensive parser, print.c formatting, inspect builtin | done |
| `src/emu/` | chip-8 cpu (pure), display renderers, keypad map, raw terminal, emu loop | done |
| `src/netmon/` | AF_PACKET capture, defensive decode, filter, print, netmon builtin | done |
| `tests/` | harness.h, harness_selftest.c, demo.sh, integration/ scripts | done |

`src/shell/run.c` owns the read-eval loops: the prompt, line editing, the PS2
continuation prompt, the line accumulator, lex, parse, eval, and the job reap
cycle before each prompt. `run_interactive` and `run_stream` are the two entry
points, and a script file is just `run_stream` over an opened `FILE *`.
`src/main.c` is now startup and teardown only: argument handling, signals, the
job table, the controlling terminal, loading and saving `~/.nullsh_history`, and
the exit status. Unit tests do not live in `tests/`; each one sits beside its
module as `foo_test.c`.

## Decisions and why

- **The flat pipeline grammar earned its AST in phase 9.** Through phase 8 the grammar was `pipeline := cmd ('|' cmd)* ['&']` plus redirects, and a vector of Command structs represented it fully, because `a | b` is a sequence. `a && b` is a decision instead: whether `b` runs depends on the result of `a`, and `if` nests arbitrarily deep, so the structure is recursive and a flat list cannot hold it. Phase 9 added conditionals, loops and functions, which is exactly the point where the tree starts paying for itself. `NODE_PIPELINE` embeds the old `Pipeline` by value, so exec_pipeline stays the single place processes are born and none of phases 1 through 4 had to move.
- **Keywords are contextual and the lexer stays dumb.** The lexer emits words and operators and never decides that a word means something. Only the parser knows that a bare one-segment word in command position is `if` or `done`, which is why `echo if` prints "if" and a file named `done` is an ordinary argument. Real shells work this way and the alternative, a keyword table in the lexer, would need the lexer to track grammar position. One divergence follows from it: the token model does not record whether a word was double quoted, so `"if"` in command position is still the keyword. That fact is in the manual's limitations table rather than being fixed.
- **INCOMPLETE is a different error from SYNTAX, and that difference is the continuation prompt.** `if true` with nothing after it is unfinished, not wrong. Running out of tokens inside any construct, after `&&`, `||`, `|` or `!`, or inside an unterminated quote returns `NSH_ERR_INCOMPLETE`; a real dead end like `fi` with no `if` stays `NSH_ERR_SYNTAX`. The driver in run.c treats the first as "print `> ` and read another line", joining lines with `\n` and re-lexing the whole buffer, and the second as an error with status 2. Without the split, the shell would have to guess.
- **Control flow is a flag on the Shell, not a return value.** `break` inside an `if` inside a `while` has to unwind several levels of the evaluator's recursion. Threading that through every return value would infect functions that have nothing to do with loops, so `break`, `continue` and `return` set `sh->flow` and every list evaluator checks it after each item and stops early. The loop evaluator consumes BREAK and CONTINUE, the function call consumes RETURN, and `sh->loop_depth` and `sh->func_depth` are what gate the three builtins.
- **Positional parameters swap like a stack around a function call.** `$1` inside a function is the function's first argument and `$1` after the call is whatever it was before. The evaluator saves `sh->argc/argv`, points them at the call's argv, runs the body, and restores them on every exit path. Scripts set the same two fields once at startup, so `$0`, `$1`..`$9` and `$#` are one mechanism serving both instead of two that can disagree.
- **The SIGINT watch exists because the shell ignores SIGINT.** Ctrl-C spares the prompt because the shell sets SIGINT to SIG_IGN and only the foreground process group receives the signal. A loop built entirely of builtins forks nothing, so nobody would receive it and `while true; do ...; done` would be unkillable. The evaluator turns on `signals_int_watch(1)` when loop depth goes 0 to 1 and off again at 1 to 0, checks `signals_int_take()` each iteration, and also reads a pipeline status of 130 as a stop request. Either path ends the loop with status 130.
- **Functions cannot shadow builtins.** Dispatch for a command word is builtin, then function, then PATH search. bash puts functions first; nullsh does not, so no script can define `exit` or `cd` and lock the shell out of its own controls. The cost is that a function named after a builtin is silently unreachable, which is written down in the manual rather than diagnosed.
- **A funcdef body is cloned into the function table.** A definition inside a loop or an `if` is evaluated every time control reaches it, so the table cannot borrow a pointer into a parse tree that the driver frees after each program. `ast_clone` deep-copies the body at definition time and `func_define` owns the copy, which also means a redefinition can free the old body without caring who is running.
- **The read-eval loop lives in run.c, not main.c.** Phase 9 turned one line into a buffer of lines and one input style into three, and the loop grew a PS2 prompt, an accumulator and an INCOMPLETE retry. Left in `main.c` it would have crossed the 500-line rule and stayed untestable, because `main.c` is excluded from every test link. `run.c` exposes `run_interactive` and `run_stream`, both taking a Shell, so integration tests drive the same code the binary does and `main.c` shrinks back to startup and teardown.
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
