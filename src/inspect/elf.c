// Defensive ELF64 little-endian parser: map the file, bounds-check every field, copy names out.

#define _POSIX_C_SOURCE 200809L

#include "elf.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../alloc/alloc.h"

// Spec sizes. A file may declare a larger entsize but never a smaller one.
#define EHDR_SIZE 64
#define SHDR_SIZE 64
#define PHDR_SIZE 56
#define SYM_SIZE 24

// e_ident slots.
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1

#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_NOBITS 8
#define SHT_DYNSYM 11

#define SHN_UNDEF 0

// Elf64_Ehdr layout.
#define EH_TYPE 16
#define EH_MACHINE 18
#define EH_ENTRY 24
#define EH_PHOFF 32
#define EH_SHOFF 40
#define EH_PHENTSIZE 54
#define EH_PHNUM 56
#define EH_SHENTSIZE 58
#define EH_SHNUM 60
#define EH_SHSTRNDX 62

// Elf64_Shdr layout.
#define SH_NAME 0
#define SH_TYPE 4
#define SH_FLAGS 8
#define SH_ADDR 16
#define SH_OFFSET 24
#define SH_SIZE 32
#define SH_LINK 40
#define SH_ENTSIZE 56

// Elf64_Phdr layout.
#define PH_TYPE 0
#define PH_FLAGS 4
#define PH_OFFSET 8
#define PH_VADDR 16
#define PH_FILESZ 32
#define PH_MEMSZ 40
#define PH_ALIGN 48

// Elf64_Sym layout.
#define ST_NAME 0
#define ST_INFO 4
#define ST_SHNDX 6
#define ST_VALUE 8
#define ST_SIZE 16

// Byte readers, so nothing depends on host struct padding or alignment.
static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

// The one rule: off + n must not leave the mapping, and must not wrap doing it.
static bool in_bounds(uint64_t len, uint64_t off, uint64_t n) {
    return off <= len && n <= len - off;
}

static bool fits_mul(uint64_t count, uint64_t each, uint64_t *total) {
    if (each != 0 && count > UINT64_MAX / each) {
        return false;
    }
    *total = count * each;
    return true;
}

// Names must terminate inside the table; an unterminated one is a corrupt file.
static NshError copy_name(const unsigned char *tab, uint64_t tab_len,
                          uint32_t idx, char **out) {
    *out = NULL;
    if (tab == NULL) {
        *out = nsh_calloc(1, 1);
        return NSH_OK;
    }
    if (idx >= tab_len) {
        return NSH_ERR_INVALID;
    }
    uint64_t end = idx;
    while (end < tab_len && tab[end] != '\0') {
        end++;
    }
    if (end == tab_len) {
        return NSH_ERR_INVALID;
    }
    size_t n = (size_t)(end - idx);
    char *s = nsh_malloc(n + 1);
    memcpy(s, tab + idx, n);
    s[n] = '\0';
    *out = s;
    return NSH_OK;
}

static NshError map_file(const char *path, ElfFile *f) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return NSH_ERR_IO;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        (void)close(fd);
        return NSH_ERR_IO;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
        (void)close(fd);
        return NSH_ERR_INVALID;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    // The mapping outlives the descriptor, so the fd goes back either way.
    (void)close(fd);
    if (p == MAP_FAILED) {
        return NSH_ERR_IO;
    }
    f->data = p;
    f->len = (size_t)st.st_size;
    return NSH_OK;
}

static NshError read_ehdr(ElfFile *f, uint16_t *shentsize, uint16_t *phentsize) {
    if (f->len < EHDR_SIZE) {
        return NSH_ERR_INVALID;
    }
    const unsigned char *e = f->data;
    if (e[0] != 0x7f || e[1] != 'E' || e[2] != 'L' || e[3] != 'F') {
        return NSH_ERR_INVALID;
    }
    if (e[EI_CLASS] != ELFCLASS64 || e[EI_DATA] != ELFDATA2LSB ||
        e[EI_VERSION] != EV_CURRENT) {
        return NSH_ERR_INVALID;
    }
    f->hdr.type = rd16(e + EH_TYPE);
    f->hdr.machine = rd16(e + EH_MACHINE);
    f->hdr.entry = rd64(e + EH_ENTRY);
    f->hdr.phoff = rd64(e + EH_PHOFF);
    f->hdr.shoff = rd64(e + EH_SHOFF);
    f->hdr.phnum = rd16(e + EH_PHNUM);
    f->hdr.shnum = rd16(e + EH_SHNUM);
    f->hdr.shstrndx = rd16(e + EH_SHSTRNDX);
    *shentsize = rd16(e + EH_SHENTSIZE);
    *phentsize = rd16(e + EH_PHENTSIZE);
    return NSH_OK;
}

// The declared entsize is the stride, the way readelf walks the tables.
static NshError table_bounds(const ElfFile *f, uint64_t off, uint16_t count,
                             uint16_t entsize, uint16_t min_entsize) {
    if (entsize < min_entsize) {
        return NSH_ERR_INVALID;
    }
    uint64_t total = 0;
    if (!fits_mul(count, entsize, &total) || !in_bounds(f->len, off, total)) {
        return NSH_ERR_INVALID;
    }
    return NSH_OK;
}

static NshError find_shstrtab(const ElfFile *f, const unsigned char *table,
                              uint16_t entsize, const unsigned char **strtab,
                              uint64_t *strtab_len) {
    *strtab = NULL;
    *strtab_len = 0;
    if (f->hdr.shstrndx == SHN_UNDEF) {
        return NSH_OK;
    }
    const unsigned char *sh = table + (uint64_t)f->hdr.shstrndx * entsize;
    uint64_t off = rd64(sh + SH_OFFSET);
    uint64_t size = rd64(sh + SH_SIZE);
    if (rd32(sh + SH_TYPE) == SHT_NOBITS || !in_bounds(f->len, off, size)) {
        return NSH_ERR_INVALID;
    }
    *strtab = f->data + off;
    *strtab_len = size;
    return NSH_OK;
}

static NshError read_sections(ElfFile *f, uint16_t entsize) {
    uint16_t count = f->hdr.shnum;
    if (count == 0) {
        // A relocatable object can legally have no section table at all.
        return f->hdr.shstrndx == SHN_UNDEF ? NSH_OK : NSH_ERR_INVALID;
    }
    if (f->hdr.shstrndx >= count) {
        return NSH_ERR_INVALID;
    }
    NshError err = table_bounds(f, f->hdr.shoff, count, entsize, SHDR_SIZE);
    if (err != NSH_OK) {
        return err;
    }

    const unsigned char *table = f->data + f->hdr.shoff;
    const unsigned char *strtab = NULL;
    uint64_t strtab_len = 0;
    err = find_shstrtab(f, table, entsize, &strtab, &strtab_len);
    if (err != NSH_OK) {
        return err;
    }

    f->sections = nsh_calloc(count, sizeof *f->sections);
    f->nsections = count;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *sh = table + (uint64_t)i * entsize;
        ElfSection *s = &f->sections[i];
        s->type = rd32(sh + SH_TYPE);
        s->flags = rd64(sh + SH_FLAGS);
        s->addr = rd64(sh + SH_ADDR);
        s->offset = rd64(sh + SH_OFFSET);
        s->size = rd64(sh + SH_SIZE);
        s->entsize = rd64(sh + SH_ENTSIZE);
        s->link = rd32(sh + SH_LINK);
        // SHT_NOBITS (.bss) owns no file bytes, so its size may run past EOF.
        if (s->type != SHT_NOBITS && !in_bounds(f->len, s->offset, s->size)) {
            return NSH_ERR_INVALID;
        }
        err = copy_name(strtab, strtab_len, rd32(sh + SH_NAME), &s->name);
        if (err != NSH_OK) {
            return err;
        }
    }
    return NSH_OK;
}

static NshError read_segments(ElfFile *f, uint16_t entsize) {
    uint16_t count = f->hdr.phnum;
    if (count == 0) {
        return NSH_OK;
    }
    NshError err = table_bounds(f, f->hdr.phoff, count, entsize, PHDR_SIZE);
    if (err != NSH_OK) {
        return err;
    }

    const unsigned char *table = f->data + f->hdr.phoff;
    f->segments = nsh_calloc(count, sizeof *f->segments);
    f->nsegments = count;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *ph = table + (uint64_t)i * entsize;
        ElfSegment *g = &f->segments[i];
        g->type = rd32(ph + PH_TYPE);
        g->flags = rd32(ph + PH_FLAGS);
        g->offset = rd64(ph + PH_OFFSET);
        g->vaddr = rd64(ph + PH_VADDR);
        g->filesz = rd64(ph + PH_FILESZ);
        g->memsz = rd64(ph + PH_MEMSZ);
        g->align = rd64(ph + PH_ALIGN);
        if (!in_bounds(f->len, g->offset, g->filesz)) {
            return NSH_ERR_INVALID;
        }
    }
    return NSH_OK;
}

static const ElfSection *pick_symtab(ElfFile *f) {
    for (size_t i = 0; i < f->nsections; i++) {
        if (f->sections[i].type == SHT_SYMTAB) {
            return &f->sections[i];
        }
    }
    for (size_t i = 0; i < f->nsections; i++) {
        if (f->sections[i].type == SHT_DYNSYM) {
            f->symbols_dynamic = true;
            return &f->sections[i];
        }
    }
    return NULL;
}

static NshError read_symbols(ElfFile *f) {
    const ElfSection *tab = pick_symtab(f);
    if (tab == NULL) {
        return NSH_OK;
    }
    if (tab->entsize < SYM_SIZE || tab->type == SHT_NOBITS) {
        return NSH_ERR_INVALID;
    }
    if (tab->link >= f->nsections) {
        return NSH_ERR_INVALID;
    }
    const ElfSection *str = &f->sections[tab->link];
    if (str->type != SHT_STRTAB || !in_bounds(f->len, str->offset, str->size)) {
        return NSH_ERR_INVALID;
    }
    const unsigned char *names = f->data + str->offset;

    uint64_t count = tab->size / tab->entsize;
    if (count == 0) {
        return NSH_OK;
    }
    if (count > SIZE_MAX / sizeof(ElfSymbol)) {
        return NSH_ERR_INVALID;
    }
    f->symbols = nsh_calloc((size_t)count, sizeof *f->symbols);
    f->nsymbols = (size_t)count;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t at = i * tab->entsize;
        if (!in_bounds(tab->size, at, SYM_SIZE)) {
            return NSH_ERR_INVALID;
        }
        const unsigned char *e = f->data + tab->offset + at;
        ElfSymbol *sym = &f->symbols[i];
        sym->value = rd64(e + ST_VALUE);
        sym->size = rd64(e + ST_SIZE);
        sym->bind = (unsigned char)(e[ST_INFO] >> 4);
        sym->type = (unsigned char)(e[ST_INFO] & 0xf);
        sym->shndx = rd16(e + ST_SHNDX);
        NshError err = copy_name(names, str->size, rd32(e + ST_NAME),
                                 &sym->name);
        if (err != NSH_OK) {
            return err;
        }
    }
    return NSH_OK;
}

NshError elf_open(const char *path, ElfFile *out) {
    if (path == NULL || out == NULL) {
        return NSH_ERR_INVALID;
    }
    memset(out, 0, sizeof *out);

    ElfFile f;
    memset(&f, 0, sizeof f);
    NshError err = map_file(path, &f);
    if (err != NSH_OK) {
        return err;
    }

    uint16_t shentsize = 0;
    uint16_t phentsize = 0;
    err = read_ehdr(&f, &shentsize, &phentsize);
    if (err == NSH_OK) {
        err = read_sections(&f, shentsize);
    }
    if (err == NSH_OK) {
        err = read_segments(&f, phentsize);
    }
    if (err == NSH_OK) {
        err = read_symbols(&f);
    }
    if (err != NSH_OK) {
        elf_close(&f);
        return err;
    }
    *out = f;
    return NSH_OK;
}

void elf_close(ElfFile *f) {
    if (f == NULL) {
        return;
    }
    for (size_t i = 0; i < f->nsections; i++) {
        nsh_free(f->sections[i].name);
    }
    nsh_free(f->sections);
    for (size_t i = 0; i < f->nsymbols; i++) {
        nsh_free(f->symbols[i].name);
    }
    nsh_free(f->symbols);
    nsh_free(f->segments);
    if (f->data != NULL) {
        // Nothing useful is left to do if unmapping our own mapping fails.
        (void)munmap(f->data, f->len);
    }
    memset(f, 0, sizeof *f);
}
