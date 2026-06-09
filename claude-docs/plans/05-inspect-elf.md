# Phase 5: inspect, the ELF viewer

## Goal

`inspect /usr/bin/ls` prints the file header story; `--sections`, `--segments`, `--symbols` print the tables. Same information as readelf, rebuilt from the spec to learn how compiled programs are organized. Defensive parsing throughout: a truncated or malicious file gets an error, never a crash.

## Concepts this phase teaches

### One file, two views

An ELF binary is a single byte stream described two ways. SECTIONS are the linker's view: .text (code), .rodata (constants), .data (initialized globals), .bss (zeroed globals, occupies no file bytes), .symtab (symbol table). SEGMENTS (program headers) are the loader's view: which byte ranges to mmap where, with what permissions. Sections are for building; segments are for running. readelf -S vs readelf -l.

### The header at offset 0

Sixteen ident bytes open the file: 0x7f 'E' 'L' 'F', then class (32/64 bit), endianness, version. After that: type (ET_EXEC fixed-address executable, ET_DYN position-independent executable or shared library, ET_REL relocatable .o), machine (EM_X86_64), the entry point address, and the offsets/counts of the two header tables. Modern /usr/bin/ls is ET_DYN, not ET_EXEC, because distros build everything position-independent for address randomization.

### Everything is offsets, names included

A section header does not contain its name; it contains an offset into one designated string-table section (shstrtab). Symbols point into .strtab the same way. Parsing is a scavenger hunt: header, then section table, then the string table it names, then resolve. nullsh copies every name out into nsh-allocated strings during parsing so nothing downstream touches raw file bytes.

### Never trust a length field

Every offset and size in the file is attacker-controlled data. The rule: before reading N bytes at offset O, check O and N against the mapped file length, including overflow (O + N can wrap). The parser returns NSH_ERR_INVALID with no partial state on any violation. The unit tests corrupt every field that gates a read.

### mmap fits the theme

The file is mapped read-only with mmap, the same syscall the loader itself uses to load the program for real. Scope: 64-bit little-endian ELF only (everything on this machine); anything else is a clean "unsupported" error.

## Contracts (elf.h written by Fable, committed with this plan)

Data-only structs (ElfFile, ElfSection, ElfSegment, ElfSymbol) plus elf_open/elf_close. Parsing fills nsh-allocated arrays with names already resolved. Symbols come from .symtab when present, else .dynsym, recorded in a flag.

## Waves (parallel clones)

- Agent A: src/inspect/elf.c + elf_test.c. The parser and validator. Unit tests parse hand-built ELF byte arrays (a minimal valid 64-bit ELF crafted in the test, byte by byte, which is itself a lesson) plus truncation/corruption of every gating field; also parses /bin/true or the test binary itself and sanity-checks against known properties. Cross-check struct offsets against <elf.h> constants in one test.
- Agent B: src/inspect/print.c/h (pure formatting of the structs; unit tests hand-build ElfFile) + src/inspect/inspect.c/h (builtin entry, flag parsing, open/print/close; no tests of its own in the clone since elf.c is absent there) + tests/integration/06_inspect.sh (compiles small fixtures with gcc at test time: a PIE binary, a .o, a static binary if glibc-static is available else skip that case; runs inspect through the real shell, greps expected rows; diffs section count against readelf -S when readelf exists). B does NOT register the builtin and does NOT touch the Makefile; the orchestrator does the two-line glue at integration.

## The builtin

```
inspect FILE              header summary: class, type, machine, entry, table counts
inspect --sections FILE   name, type, addr, offset, size per section
inspect --segments FILE   type, perms, offset, vaddr, filesz, memsz per segment
inspect --symbols FILE    value, size, bind, type, name (symtab or dynsym, labeled)
```

Errors: unreadable file, not ELF, unsupported class/endianness, truncated: one clear stderr line, status 1.

## Exit criteria

Dual-pass suite green including 06_inspect.sh; `inspect /usr/bin/ls | grep .text` works end to end (pipes phase pays off); Fable verifies output against readelf by hand; lowercase tldr commit.
