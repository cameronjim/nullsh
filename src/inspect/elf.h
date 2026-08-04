// Parsed, validated view of a 64-bit little-endian ELF file. Data only; printing lives elsewhere.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../util/error.h"

typedef struct {
    uint16_t type;       // ET_EXEC, ET_DYN, ET_REL, ...
    uint16_t machine;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint16_t phnum;
    uint16_t shnum;
    uint16_t shstrndx;
} ElfHeader;

typedef struct {
    char *name;      // nsh-allocated copy, "" when nameless
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint64_t entsize;
    uint32_t link;
} ElfSection;

typedef struct {
    uint32_t type;
    uint32_t flags;    // PF_R | PF_W | PF_X bits
    uint64_t offset;
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} ElfSegment;

typedef struct {
    char *name;           // nsh-allocated copy
    uint64_t value;
    uint64_t size;
    unsigned char bind;   // STB_*
    unsigned char type;   // STT_*
    uint16_t shndx;
} ElfSymbol;

typedef struct {
    unsigned char *data;   // mmap'd file, read only
    size_t len;
    ElfHeader hdr;
    ElfSection *sections;  // nsh-allocated array
    size_t nsections;
    ElfSegment *segments;
    size_t nsegments;
    ElfSymbol *symbols;    // from .symtab, else .dynsym
    size_t nsymbols;
    bool symbols_dynamic;  // true when symbols came from .dynsym
} ElfFile;

// NSH_ERR_IO when the file cannot be opened or mapped, NSH_ERR_INVALID for
// anything that is not valid supported ELF (wrong magic, 32-bit, big-endian,
// any offset or size that leaves the file). On error out holds no resources.
NshError elf_open(const char *path, ElfFile *out);

// Unmaps and frees everything; safe on a zeroed struct and safe to call twice.
void elf_close(ElfFile *f);
