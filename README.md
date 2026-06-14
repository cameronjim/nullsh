# nullsh

nullsh is an educational Unix shell written in pure C17 with zero dependencies.
Everything it needs it builds itself: a custom memory allocator, a lexer and
parser, job control, an ELF inspector, a live packet viewer, and a CHIP-8
virtual machine. The point is to read the whole thing and understand every
line, so there is no third party code and nothing hidden behind a library.

## Build

```
make
```

That produces `build/nullsh` with `-O2`. Use `make debug` for a build with
`-g -O0` and AddressSanitizer plus UBSan at `build/nullsh-debug`, and
`make clean` to delete `build/`.

## Test

```
make test
```

This builds the debug binary, compiles and runs every unit test next to the
code it covers, then runs the integration scripts in `tests/integration/`
against the debug shell. Any failure fails the whole run.
