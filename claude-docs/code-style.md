# nullsh code style

These rules are fixed. Every commit follows all of them.

## Language and build

- Pure C17. Compiles clean with `-std=c17 -Wall -Wextra -Werror -pedantic` on gcc and clang.
- Only POSIX system headers and the C standard library. No third-party code. If something is needed, write it.
- Allocation goes through `nsh_malloc` / `nsh_free` / `nsh_realloc` / `nsh_calloc` everywhere. Never call libc `malloc` directly outside `src/alloc/`.
- Makefile only. Targets: `make`, `make test`, `make clean`, `make debug`, `make release`.

## Structure

- No file over ~500 lines of non-test code. Split along responsibility lines before adding, not after. Test files are exempt.
- Headers use `#pragma once`.
- `static` is the default. Only the public API goes in the header.
- No global mutable state except where POSIX forces it: signal flags are `volatile sig_atomic_t`, and the job table is global by nature. Everything else is passed explicitly.

## Naming

- `snake_case` functions, `PascalCase` structs, `SCREAMING_SNAKE` constants, enums, macros.
- Module-public functions carry the module prefix: `alloc_init`, `lexer_scan`, `elf_inspect`, `netmon_start`.

## Comments

- `//` only, never `/* */`.
- One line per comment. Never stack `//` lines into paragraph blocks.
- Strictly minimal and simple. A comment earns its place only for an invariant, a platform quirk, or syscall behavior being relied on. Never narrate the obvious, never explain what plain code already says.
- Every `.c` and `.h` opens with exactly one `//` line naming what the module does.

## Errors

- Every syscall checked. Every allocation checked for NULL.
- Errors propagate with return codes (the `NshError` enum in `util/`), never with fprintf-and-pray.
- nullsh never segfaults on bad input. Bad input gets a clear message and a return to the prompt.

## Tests

- Every change ships tests in the same commit. New feature, new tests. Bug fix, regression test.
- Unit tests live in `*_test.c` next to the source they test. Integration tests are shell scripts in `tests/integration/`.
- Development builds run with AddressSanitizer and UBSan. A test that passes without sanitizers but fails with them is a real bug.

## Prose

- Docs, comments, and user-facing text: direct, specific, human. No em dashes. No "utilize" or "leverage".
