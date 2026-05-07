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
1. Runs the debug `nullsh` binary with commands fed on stdin or via `-c` once that exists.
2. Compares actual output to expected output with `diff`.
3. Exits nonzero on mismatch.

Convention: `NULLSH=$1` is passed by `make test` so scripts never guess the binary path. Scripts that need privileges (netmon) live behind `make test-net` and skip themselves when not root.

## Sanitizers

`make debug` and `make test` compile with `-fsanitize=address,undefined -g -O0`.

- ASan catches use-after-free, buffer overflow, leaks at exit.
- UBSan catches signed overflow, misaligned loads, shift abuse.
- Once the real allocator lands in Phase 2, ASan cannot see inside `nsh_malloc` blocks, so the allocator carries its own canaries and freed-memory poisoning. Run the suite both ways.

## Rules

- Same commit as the change. No exceptions.
- A failing test is fixed or the change is reverted. The suite is never left red.
- When a bug is found by hand, the fix lands with a regression test that fails on the old code.
