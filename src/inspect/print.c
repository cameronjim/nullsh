// Human readable rendering of a parsed ElfFile. Pure formatting, no file access.

#include "print.h"

#include <inttypes.h>

// ELF constants the printer names, spelled out here so <elf.h> stays out.
#define ET_REL  1
#define ET_EXEC 2
#define ET_DYN  3
#define ET_CORE 4

#define EM_X86_64 0x3e

#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_DYNAMIC  6
#define SHT_NOTE     7
#define SHT_NOBITS   8
#define SHT_DYNSYM   11

#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_PHDR    6

#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4

// Wide enough for "0x" plus eight hex digits plus the terminator.
#define HEX_BUF 16

#define KEY_WIDTH 8

static const char *hex_str(uint64_t v, char *buf, size_t cap) {
    snprintf(buf, cap, "0x%" PRIx64, v);
    return buf;
}

static const char *safe_name(const char *name) {
    return (name == NULL) ? "" : name;
}

static const char *file_type_name(uint16_t t) {
    switch (t) {
    case ET_REL:
        return "REL";
    case ET_EXEC:
        return "EXEC";
    case ET_DYN:
        return "DYN";
    case ET_CORE:
        return "CORE";
    default:
        return NULL;
    }
}

static const char *section_type_name(uint32_t t, char *buf, size_t cap) {
    switch (t) {
    case SHT_PROGBITS:
        return "PROGBITS";
    case SHT_SYMTAB:
        return "SYMTAB";
    case SHT_STRTAB:
        return "STRTAB";
    case SHT_RELA:
        return "RELA";
    case SHT_DYNAMIC:
        return "DYNAMIC";
    case SHT_NOTE:
        return "NOTE";
    case SHT_NOBITS:
        return "NOBITS";
    case SHT_DYNSYM:
        return "DYNSYM";
    default:
        return hex_str(t, buf, cap);
    }
}

static const char *segment_type_name(uint32_t t, char *buf, size_t cap) {
    switch (t) {
    case PT_LOAD:
        return "LOAD";
    case PT_DYNAMIC:
        return "DYNAMIC";
    case PT_INTERP:
        return "INTERP";
    case PT_NOTE:
        return "NOTE";
    case PT_PHDR:
        return "PHDR";
    case PT_GNU_EH_FRAME:
        return "GNU_EH_FRAME";
    case PT_GNU_STACK:
        return "GNU_STACK";
    case PT_GNU_RELRO:
        return "GNU_RELRO";
    default:
        return hex_str(t, buf, cap);
    }
}

static const char *bind_name(unsigned char b, char *buf, size_t cap) {
    switch (b) {
    case STB_LOCAL:
        return "LOCAL";
    case STB_GLOBAL:
        return "GLOBAL";
    case STB_WEAK:
        return "WEAK";
    default:
        return hex_str(b, buf, cap);
    }
}

static const char *symbol_type_name(unsigned char t, char *buf, size_t cap) {
    switch (t) {
    case STT_NOTYPE:
        return "NOTYPE";
    case STT_OBJECT:
        return "OBJECT";
    case STT_FUNC:
        return "FUNC";
    case STT_SECTION:
        return "SECTION";
    case STT_FILE:
        return "FILE";
    default:
        return hex_str(t, buf, cap);
    }
}

// Always in W A X order, a lone dash when the section carries none of them.
static const char *section_flag_letters(uint64_t flags, char *buf) {
    size_t n = 0;
    if (flags & SHF_WRITE) {
        buf[n++] = 'W';
    }
    if (flags & SHF_ALLOC) {
        buf[n++] = 'A';
    }
    if (flags & SHF_EXECINSTR) {
        buf[n++] = 'X';
    }
    if (n == 0) {
        buf[n++] = '-';
    }
    buf[n] = '\0';
    return buf;
}

static const char *segment_perm_letters(uint32_t flags, char *buf) {
    buf[0] = (flags & PF_R) ? 'r' : '-';
    buf[1] = (flags & PF_W) ? 'w' : '-';
    buf[2] = (flags & PF_X) ? 'x' : '-';
    buf[3] = '\0';
    return buf;
}

void elf_print_header(const ElfFile *f, FILE *out) {
    char buf[HEX_BUF];
    const ElfHeader *h = &f->hdr;

    // The parser accepts nothing else, so this line is a constant.
    fprintf(out, "%-*s  %s\n", KEY_WIDTH, "class", "ELF64 little endian");

    const char *type = file_type_name(h->type);
    if (type != NULL) {
        fprintf(out, "%-*s  %s (0x%" PRIx16 ")\n", KEY_WIDTH, "type", type,
                h->type);
    } else {
        fprintf(out, "%-*s  %s\n", KEY_WIDTH, "type",
                hex_str(h->type, buf, sizeof buf));
    }

    if (h->machine == EM_X86_64) {
        fprintf(out, "%-*s  x86-64 (0x%" PRIx16 ")\n", KEY_WIDTH, "machine",
                h->machine);
    } else {
        fprintf(out, "%-*s  %s\n", KEY_WIDTH, "machine",
                hex_str(h->machine, buf, sizeof buf));
    }

    fprintf(out, "%-*s  0x%016" PRIx64 "\n", KEY_WIDTH, "entry", h->entry);
    fprintf(out, "%-*s  %" PRIu16 "\n", KEY_WIDTH, "sections", h->shnum);
    fprintf(out, "%-*s  %" PRIu16 "\n", KEY_WIDTH, "segments", h->phnum);
    fprintf(out, "%-*s  %" PRIu16 "\n", KEY_WIDTH, "shstrndx", h->shstrndx);
}

void elf_print_sections(const ElfFile *f, FILE *out) {
    if (f->sections == NULL || f->nsections == 0) {
        fputs("no sections\n", out);
        return;
    }

    fprintf(out, "%-4s  %-20s  %-10s  %-18s  %-10s  %-10s  %s\n", "nr", "name",
            "type", "addr", "offset", "size", "flags");

    for (size_t i = 0; i < f->nsections; i++) {
        const ElfSection *s = &f->sections[i];
        char type_buf[HEX_BUF];
        char flag_buf[4];
        fprintf(out,
                "[%2zu]  %-20s  %-10s  0x%016" PRIx64 "  0x%08" PRIx64
                "  0x%08" PRIx64 "  %s\n",
                i, safe_name(s->name),
                section_type_name(s->type, type_buf, sizeof type_buf), s->addr,
                s->offset, s->size, section_flag_letters(s->flags, flag_buf));
    }
}

void elf_print_segments(const ElfFile *f, FILE *out) {
    if (f->segments == NULL || f->nsegments == 0) {
        fputs("no segments\n", out);
        return;
    }

    fprintf(out, "%-4s  %-12s  %-5s  %-10s  %-18s  %-10s  %-10s  %s\n", "nr",
            "type", "perms", "offset", "vaddr", "filesz", "memsz", "align");

    for (size_t i = 0; i < f->nsegments; i++) {
        const ElfSegment *p = &f->segments[i];
        char type_buf[HEX_BUF];
        char perm_buf[4];
        fprintf(out,
                "[%2zu]  %-12s  %-5s  0x%08" PRIx64 "  0x%016" PRIx64
                "  0x%08" PRIx64 "  0x%08" PRIx64 "  0x%" PRIx64 "\n",
                i, segment_type_name(p->type, type_buf, sizeof type_buf),
                segment_perm_letters(p->flags, perm_buf), p->offset, p->vaddr,
                p->filesz, p->memsz, p->align);
    }
}

void elf_print_symbols(const ElfFile *f, FILE *out) {
    if (f->symbols == NULL || f->nsymbols == 0) {
        fputs("no symbols\n", out);
        return;
    }

    fprintf(out, "symbol table: %s (%zu entries)\n",
            f->symbols_dynamic ? ".dynsym" : ".symtab", f->nsymbols);
    fprintf(out, "%-5s  %-18s  %8s  %-6s  %-7s  %s\n", "nr", "value", "size",
            "bind", "type", "name");

    for (size_t i = 0; i < f->nsymbols; i++) {
        const ElfSymbol *s = &f->symbols[i];
        char bind_buf[HEX_BUF];
        char type_buf[HEX_BUF];
        fprintf(out, "[%3zu]  0x%016" PRIx64 "  %8" PRIu64 "  %-6s  %-7s  %s\n",
                i, s->value, s->size, bind_name(s->bind, bind_buf,
                sizeof bind_buf),
                symbol_type_name(s->type, type_buf, sizeof type_buf),
                safe_name(s->name));
    }
}
