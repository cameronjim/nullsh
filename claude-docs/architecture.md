# nullsh architecture

Updated at the end of every phase. Current as of: Phase 0 (skeleton).

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
| `src/alloc/` | nsh_malloc/free/realloc/calloc, later strategies + heap builtin | libc wrapper |
| `src/util/` | Str, Vec, NshError | skeleton |
| `src/shell/` | lexer, expand, parser, exec, builtins, history, later jobs/signals/redirect | Phase 1 |
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

## Build

`make` (release), `make debug` (ASan+UBSan, -g -O0), `make test` (build debug, run all unit test binaries, then integration scripts), `make clean`. Objects live under `build/`, never beside sources.
