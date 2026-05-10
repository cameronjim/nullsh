# nullsh architecture

Updated at the end of every phase. Current as of: Phase 2 (allocator).

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
| `src/shell/` | lexer, expand, parser, exec, builtins, history; jobs/signals/redirect later | Phase 1 done |
| `src/inspect/` | ELF parsing and printing | Phase 5 |
| `src/emu/` | CHIP-8 cpu, display, keypad | Phase 6 |
| `src/netmon/` | raw socket capture, header decode, print | Phase 7 |
| `tests/` | harness.h, integration scripts | Phase 0 |

## Decisions and why

- **Flat pipeline grammar, no AST.** The shell grammar is `pipeline := cmd ('|' cmd)* ['&']` plus redirects. A vector of Command structs represents it fully. An AST earns its keep only with `&&`, `if`, subshells, which are out of scope.
- **Builtins run in-process only when alone.** A builtin inside a pipeline (Phase 3) forks like an external command; only a lone builtin mutates the shell. This keeps `cd` correct and pipelines uniform.
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

## Build

`make` (release), `make debug` (ASan+UBSan, -g -O0), `make test` (build debug, run all unit test binaries, then integration scripts), `make clean`. Objects live under `build/`, never beside sources.
