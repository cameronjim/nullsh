// Human readable rendering of a parsed ElfFile. Pure formatting, no file access.

#pragma once

#include <stdio.h>

#include "elf.h"

// Class, type, machine, entry, table counts and shstrndx, one key per line.
void elf_print_header(const ElfFile *f, FILE *out);

// One row per section header, plus a column heading line.
void elf_print_sections(const ElfFile *f, FILE *out);

// One row per program header, plus a column heading line.
void elf_print_segments(const ElfFile *f, FILE *out);

// The table label, a column heading line, then one row per symbol.
void elf_print_symbols(const ElfFile *f, FILE *out);
