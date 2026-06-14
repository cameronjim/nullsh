# nullsh testing

## The harness

`tests/harness.h` is the whole framework. No dependencies.

```c
#include "../../tests/harness.h"

TEST(lexer_splits_on_spaces) {
    TokenList tl;
    ASSERT_EQ(lexer_scan("echo hi", &tl), NSH_OK);
    ASSERT_EQ(tl.count, 2);
    ASSERT_STR_EQ(tl.tokens[0].text, "echo");
}

TEST_MAIN()
```

- `TEST(name)` registers the function at load time via `__attribute__((constructor))`.
- Assertion macros print file:line and both values on failure, then fail the test and return.
- `TEST_MAIN()` runs everything and exits nonzero on any failure, so `make test` fails loudly.

## Unit tests

- One `foo_test.c` beside each `foo.c`. It links against the module's objects, not the whole shell.
- Test the pure logic hard: lexer, parser, expand, allocator internals, ELF parsing, packet decoding, CHIP-8 opcodes. These are functions from bytes to structs and are cheap to test exhaustively.
- Edge cases are the point: empty input, unterminated quote, zero-size alloc, truncated ELF header, short packet. The rule is that nullsh never crashes on bad input, and these tests are the proof.

## Integration tests

Shell scripts in `tests/integration/`. Each script:
1. Runs the debug `nullsh` binary with commands fed on stdin.
2. Compares actual output to expected output, line by line or with `diff`.
3. Prints one `ok` or `FAIL` line per check and exits nonzero on any mismatch.

Conventions every script follows: `NULLSH=$1` is passed by `make test` so scripts never guess the binary path, `HOME` points at a `mktemp -d` scratch dir so the developer's own `~/.nullsh_history` is never touched, and a trap removes that dir on exit.

## What one `make test` actually runs

The whole suite runs twice, once with `NSH_ALLOC_STRATEGY=firstfit` and once with `buddy`, because after Phase 2 every shell test is also an allocator test. One pass is 508 unit test cases across 26 test binaries plus 213 integration checks across 9 scripts, so `make test` is 1442 checks. Nothing stops early: a failing binary or script is recorded and the run continues, then the whole thing exits 1 with `SUITE FAILED`.

Two things sit outside `make test`:

- `make test-net` runs `tests/integration/08_netmon.sh`, which backgrounds a netmon on `lo`, sends itself a UDP datagram, then interrupts it and checks the decoded line and the summary. It needs a raw socket, so run it as `sudo make test-net`. Without root the script prints `SKIP` and exits 0 rather than failing, which is why it can stay in the repo without being a landmine.
- `make demo` runs `tests/demo.sh`, which replays the README transcript against the release binary and diffs the result, with pids, heap byte counts and ELF addresses normalized away first. It is deliberately not in `make test`: the suite must not go red because a document drifted.

## Running one thing on its own

Unit test binaries are named after their source path with the slashes turned into underscores:

```
make build/tests/src_netmon_decode_test
./build/tests/src_netmon_decode_test
NSH_ALLOC_STRATEGY=buddy ./build/tests/src_netmon_decode_test
```

Integration scripts take the binary as their one argument:

```
make debug
sh tests/integration/03_pipes.sh ./build/nullsh-debug
```

## The tests that need a terminal

`tests/integration/05_jobs_tty.sh` is the only script that runs nullsh on a real pty. It starts the shell under `script(1)` with a fifo on stdin, finds the shell's pid with `ps`, and delivers SIGTSTP and SIGINT to the foreground child's process group, which is exactly what the tty driver does when you press Ctrl-Z or Ctrl-C. It then asserts that the job shows up `Stopped`, that `fg` runs it to completion, and that Ctrl-C left the shell alive.

Every prerequisite it has is a SKIP and never a failure: no `script(1)`, no `mkfifo`, no `ps --ppid`, or the shell never appearing under `script`. Sleep-based timing on a pty is not something to turn a build red. The rest of job control is covered without a terminal by `04_jobs.sh`, because nullsh keeps process groups and reaping working in scripts too.

## Tests that expect a crash

Some contracts can only be proven by dying. Those tests fork a sacrificial child, silence its stderr, do the illegal thing, and assert on the wait status from the parent:

- `alloc_test.c` corrupts one guard byte past the payload, and again one byte before it, then frees the block and asserts `WIFSIGNALED` with `SIGABRT`. This is the only way the canary contract can be checked at all, since ASan interposes on libc malloc and cannot see inside an arena nullsh mmapped itself. Double free, an interior pointer, a pointer belonging to no arena, and arena exhaustion are all checked the same way.
- `signals_test.c` forks a child that raises SIGINT, with and without `signals_reset_child`, to prove the shell's ignore disposition is installed and that children get the default back.

A forked child inside a test binary must leave through `_exit`, never `return` or `exit`, or it re-enters the harness and reports phantom results.

## Mutation testing the decoders

The pure byte-to-struct modules, `netmon/decode.c` above all, were checked by mutation rather than by counting coverage. The practice: change one thing in the module by hand (drop an `ntohs`, relax a `<=` to `<`, delete a length check, read the IHL field as bytes instead of 4-byte words), rebuild, and confirm at least one test fails. A mutant that survives means the suite has a hole, and the fix is a new test. `decode_test.c` reached 32 cases that way, and the same pass was run over `elf.c` and the CHIP-8 opcode decoder in `cpu.c`. It is a manual practice, not a Makefile target.

## Sanitizers

`make debug` and `make test` compile with `-fsanitize=address,undefined -g -O0`.

- ASan catches use-after-free, buffer overflow, leaks at exit.
- UBSan catches signed overflow, misaligned loads, shift abuse.
- ASan cannot see inside `nsh_malloc` blocks, because it interposes on libc malloc and nullsh no longer uses it for shell allocations. The allocator carries its own canaries and freed-memory poisoning instead, and the suite runs under both strategies so both arenas get exercised.

## Rules

- Same commit as the change. No exceptions.
- A failing test is fixed or the change is reverted. The suite is never left red.
- When a bug is found by hand, the fix lands with a regression test that fails on the old code.
