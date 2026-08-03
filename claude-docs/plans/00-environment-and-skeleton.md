# Phase 0: Environment and repo skeleton

## Goal

A Linux toolchain on this Windows 10 machine and an empty but buildable nullsh repo with the Makefile, test harness, and docs in place. At the end of this phase, `make test` runs and passes with zero real code.

## Part A: WSL2 + Ubuntu

nullsh is Linux-only code. fork/exec, process groups, ELF, and AF_PACKET raw sockets do not exist on Windows. WSL2 runs a real Linux kernel in a lightweight VM, so everything works natively, including sanitizers and raw sockets.

Steps:
1. `wsl --install -d Ubuntu` from an elevated shell. Installs the WSL platform, the virtual machine feature, and Ubuntu.
2. Reboot Windows. Required before the VM platform is usable.
3. First launch of Ubuntu asks for a Unix username and password. This is separate from the Windows account.
4. `sudo apt update && sudo apt install -y build-essential gdb git`
5. Verify: `gcc --version` shows gcc 11+, and a hello-world compiled with `-fsanitize=address,undefined` runs clean.

Why the repo lives in the WSL filesystem (`~/code/nullsh`), not `/mnt/c/...`:
- `/mnt/c` goes through a network filesystem protocol (9P). Compiles are several times slower.
- Some POSIX behavior (permissions, file locking, case sensitivity) is emulated poorly on `/mnt/c`.
- Windows can still browse the repo at `\\wsl$\Ubuntu\home\<user>\code\nullsh`.

The `C:\Users\CJ\code\nullsh` folder holds these planning docs until the WSL repo exists, then they move into it and this folder becomes a pointer.

## Part B: Repo skeleton

```
nullsh/
  Makefile
  README.md
  src/
    main.c            entry point, REPL loop
    alloc/            nsh_malloc and friends (libc wrapper until Phase 2)
    shell/            lexer, parser, exec, builtins, jobs, history
    inspect/          ELF viewer (Phase 5)
    netmon/           packet viewer (Phase 7)
    emu/              CHIP-8 (Phase 6)
    util/             growable string, growable array, error codes
  tests/
    harness.h         assertion macros and test runner
    integration/      shell scripts that drive nullsh and diff output
  claude-docs/
    architecture.md   how the pieces fit, updated every phase
    code-style.md     the rules, from the project spec
    testing.md        how to write and run tests
    plans/            one plan per element, like this file
```

### Test harness

`tests/harness.h`, roughly 60 lines, no dependencies:
- `TEST(name)` defines and registers a test function.
- `ASSERT_TRUE(x)`, `ASSERT_EQ(a,b)`, `ASSERT_STR_EQ(a,b)` print file:line and the values on failure, mark the test failed, and return.
- `TEST_MAIN()` expands to a `main` that runs every registered test and exits nonzero if any failed.

Registration trick: a fixed-size static array of function pointers plus `__attribute__((constructor))` registrars, which gcc and clang both support. No linker scripts, no frameworks.

### Makefile

Targets: `make` (default, same as release), `make debug`, `make release`, `make test`, `make clean`.
- `SRCS := $(shell find src -name '*.c' ! -name '*_test.c')`
- Objects and binaries under `build/` so `make clean` is `rm -rf build`.
- `debug` flags: `-std=c17 -Wall -Wextra -Werror -pedantic -g -O0 -fsanitize=address,undefined`
- `release` flags: `-std=c17 -Wall -Wextra -Werror -pedantic -O2`
- `make test`: for each `*_test.c`, link it with its module objects (debug flags) into `build/tests/<name>` and run it. Then run every `tests/integration/*.sh` with the freshly built debug `nullsh` on PATH.

### Allocator placeholder

`src/alloc/alloc.h` declares `nsh_malloc`, `nsh_free`, `nsh_realloc`, `nsh_calloc`. `src/alloc/alloc.c` implements them as checked wrappers over libc malloc for now. Every other module calls only `nsh_*` from day one, so Phase 2 swaps internals without touching call sites.

## Exit criteria

- `wsl` opens Ubuntu, gcc + ASan verified.
- Repo initialized with git, skeleton committed.
- `make test` builds and runs a trivial harness self-test and a trivial integration script, both green.
