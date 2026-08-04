// Tests for the ELF parser: a hand-built binary, every corruption of it, and real files.

#define _POSIX_C_SOURCE 200809L

#include "elf.h"

#include <elf.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../tests/harness.h"

#define PATH_BUF 4096
#define CMD_BUF 8192
#define TMP_BUF 64

// The minimal fixture: ehdr, shstrtab, .text bytes, then three section headers.
#define MIN_SIZE 320
#define MIN_STRTAB_OFF 0x40
#define MIN_STRTAB_SIZE 17
#define MIN_TEXT_OFF 0x60
#define MIN_TEXT_SIZE 4
#define MIN_SHOFF 0x80
#define MIN_SHDR1 (MIN_SHOFF + 64)
#define MIN_SHDR2 (MIN_SHOFF + 128)

// The symbol fixture: a .o shape with .symtab, .strtab and no program headers.
#define SYM_SIZE_TOTAL 512
#define SYM_TEXT_OFF 0x40
#define SYM_STRTAB_OFF 0x50
#define SYM_STRTAB_SIZE 6
#define SYM_SYMTAB_OFF 0x60
#define SYM_SYMTAB_SIZE 48
#define SYM_SHSTR_OFF 0x90
#define SYM_SHSTR_SIZE 33
#define SYM_SHOFF 0xC0
#define SYM_SHDR_SYMTAB (SYM_SHOFF + 128)

static char g_tmp[TMP_BUF];

static void put16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void put32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (unsigned char)((v >> (8 * i)) & 0xff);
    }
}

static void put64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (unsigned char)((v >> (8 * i)) & 0xff);
    }
}

// One 64-byte section header written field by field at its spec offsets.
static void put_shdr(unsigned char *sh, uint32_t name, uint32_t type,
                     uint64_t flags, uint64_t addr, uint64_t offset,
                     uint64_t size, uint32_t link, uint64_t align,
                     uint64_t entsize) {
    put32(sh + 0, name);
    put32(sh + 4, type);
    put64(sh + 8, flags);
    put64(sh + 16, addr);
    put64(sh + 24, offset);
    put64(sh + 32, size);
    put32(sh + 40, link);
    put32(sh + 44, 0);
    put64(sh + 48, align);
    put64(sh + 56, entsize);
}

// A valid 64-bit little-endian ET_EXEC with three sections and no segments.
static void build_min(unsigned char *b) {
    memset(b, 0, MIN_SIZE);

    // 0x00 e_ident: magic, ELFCLASS64, ELFDATA2LSB, version 1, SysV ABI.
    b[0] = 0x7f;
    b[1] = 'E';
    b[2] = 'L';
    b[3] = 'F';
    b[4] = 2;
    b[5] = 1;
    b[6] = 1;

    // 0x10 the rest of the ehdr: type, machine, entry, table offsets, counts.
    put16(b + 16, 2);          // e_type ET_EXEC
    put16(b + 18, 0x3e);       // e_machine EM_X86_64
    put32(b + 20, 1);          // e_version
    put64(b + 24, 0x1000);     // e_entry
    put64(b + 32, 0);          // e_phoff
    put64(b + 40, MIN_SHOFF);  // e_shoff
    put32(b + 48, 0);          // e_flags
    put16(b + 52, 64);         // e_ehsize
    put16(b + 54, 56);         // e_phentsize
    put16(b + 56, 0);          // e_phnum
    put16(b + 58, 64);         // e_shentsize
    put16(b + 60, 3);          // e_shnum
    put16(b + 62, 2);          // e_shstrndx

    // 0x40 .shstrtab bytes: "" at 0, ".text" at 1, ".shstrtab" at 7.
    memcpy(b + MIN_STRTAB_OFF, "\0.text\0.shstrtab", MIN_STRTAB_SIZE);

    // 0x60 .text bytes, four of them, contents irrelevant.
    memcpy(b + MIN_TEXT_OFF, "\xb8\x00\x00\x00", MIN_TEXT_SIZE);

    // 0x80 section header table: null, .text, .shstrtab.
    put_shdr(b + MIN_SHOFF, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    put_shdr(b + MIN_SHDR1, 1, 1, 6, 0x1000, MIN_TEXT_OFF, MIN_TEXT_SIZE, 0, 16,
             0);
    put_shdr(b + MIN_SHDR2, 7, 3, 0, 0, MIN_STRTAB_OFF, MIN_STRTAB_SIZE, 0, 1,
             0);
}

// A relocatable shape carrying two symbols, the second one named "main".
static void build_sym(unsigned char *b) {
    memset(b, 0, SYM_SIZE_TOTAL);

    // 0x00 ehdr: ET_REL, no program headers, five sections.
    b[0] = 0x7f;
    b[1] = 'E';
    b[2] = 'L';
    b[3] = 'F';
    b[4] = 2;
    b[5] = 1;
    b[6] = 1;
    put16(b + 16, 1);          // e_type ET_REL
    put16(b + 18, 0x3e);
    put32(b + 20, 1);
    put64(b + 40, SYM_SHOFF);
    put16(b + 52, 64);
    put16(b + 58, 64);         // e_shentsize
    put16(b + 60, 5);          // e_shnum
    put16(b + 62, 4);          // e_shstrndx

    // 0x40 .text, 0x50 .strtab ("" at 0, "main" at 1), 0x60 two Elf64_Sym.
    memcpy(b + SYM_TEXT_OFF, "\xb8\x00\x00\x00", 4);
    memcpy(b + SYM_STRTAB_OFF, "\0main", SYM_STRTAB_SIZE);
    put32(b + SYM_SYMTAB_OFF + 24 + 0, 1);       // st_name "main"
    b[SYM_SYMTAB_OFF + 24 + 4] = 0x12;           // st_info GLOBAL FUNC
    put16(b + SYM_SYMTAB_OFF + 24 + 6, 1);       // st_shndx .text
    put64(b + SYM_SYMTAB_OFF + 24 + 8, 0x1000);  // st_value
    put64(b + SYM_SYMTAB_OFF + 24 + 16, 4);      // st_size

    // 0x90 .shstrtab: "" 0, ".text" 1, ".symtab" 7, ".strtab" 15, rest 23.
    memcpy(b + SYM_SHSTR_OFF, "\0.text\0.symtab\0.strtab\0.shstrtab",
           SYM_SHSTR_SIZE);

    // 0xC0 five section headers; .symtab links to .strtab at index 3.
    put_shdr(b + SYM_SHOFF, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    put_shdr(b + SYM_SHOFF + 64, 1, 1, 6, 0, SYM_TEXT_OFF, 4, 0, 16, 0);
    put_shdr(b + SYM_SHDR_SYMTAB, 7, 2, 0, 0, SYM_SYMTAB_OFF, SYM_SYMTAB_SIZE,
             3, 8, 24);
    put_shdr(b + SYM_SHOFF + 192, 15, 3, 0, 0, SYM_STRTAB_OFF, SYM_STRTAB_SIZE,
             0, 1, 0);
    put_shdr(b + SYM_SHOFF + 256, 23, 3, 0, 0, SYM_SHSTR_OFF, SYM_SHSTR_SIZE, 0,
             1, 0);
}

static void fixture_path(char *buf, size_t size) {
    snprintf(buf, size, "%s/fixture.elf", g_tmp);
}

// Writes the bytes to the scratch file and parses them back.
static NshError parse_bytes(const unsigned char *b, size_t len, ElfFile *f) {
    memset(f, 0, sizeof *f);
    char path[PATH_BUF];
    fixture_path(path, sizeof path);
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return NSH_ERR_IO;
    }
    if (len > 0 && fwrite(b, 1, len, fp) != len) {
        fclose(fp);
        return NSH_ERR_IO;
    }
    if (fclose(fp) != 0) {
        return NSH_ERR_IO;
    }
    return elf_open(path, f);
}

static bool holds_nothing(const ElfFile *f) {
    return f->data == NULL && f->len == 0 && f->sections == NULL &&
           f->nsections == 0 && f->segments == NULL && f->nsegments == 0 &&
           f->symbols == NULL && f->nsymbols == 0;
}

static const ElfSection *find_section(const ElfFile *f, const char *name) {
    for (size_t i = 0; i < f->nsections; i++) {
        if (strcmp(f->sections[i].name, name) == 0) {
            return &f->sections[i];
        }
    }
    return NULL;
}

static bool has_segment_type(const ElfFile *f, uint32_t type) {
    for (size_t i = 0; i < f->nsegments; i++) {
        if (f->segments[i].type == type) {
            return true;
        }
    }
    return false;
}

TEST(minimal_elf_parses_every_field) {
    unsigned char b[MIN_SIZE];
    build_min(b);
    ElfFile f;
    ASSERT_EQ(parse_bytes(b, sizeof b, &f), NSH_OK);

    ASSERT_EQ(f.len, MIN_SIZE);
    ASSERT_EQ(f.hdr.type, 2);
    ASSERT_EQ(f.hdr.machine, 0x3e);
    ASSERT_EQ(f.hdr.entry, 0x1000);
    ASSERT_EQ(f.hdr.phoff, 0);
    ASSERT_EQ(f.hdr.shoff, MIN_SHOFF);
    ASSERT_EQ(f.hdr.phnum, 0);
    ASSERT_EQ(f.hdr.shnum, 3);
    ASSERT_EQ(f.hdr.shstrndx, 2);

    ASSERT_EQ(f.nsegments, 0);
    ASSERT_TRUE(f.segments == NULL);
    ASSERT_EQ(f.nsymbols, 0);
    ASSERT_TRUE(f.symbols == NULL);
    ASSERT_EQ(f.symbols_dynamic, false);

    ASSERT_EQ(f.nsections, 3);
    ASSERT_STR_EQ(f.sections[0].name, "");
    ASSERT_EQ(f.sections[0].type, 0);
    ASSERT_EQ(f.sections[0].size, 0);

    ASSERT_STR_EQ(f.sections[1].name, ".text");
    ASSERT_EQ(f.sections[1].type, 1);
    ASSERT_EQ(f.sections[1].flags, 6);
    ASSERT_EQ(f.sections[1].addr, 0x1000);
    ASSERT_EQ(f.sections[1].offset, MIN_TEXT_OFF);
    ASSERT_EQ(f.sections[1].size, MIN_TEXT_SIZE);
    ASSERT_EQ(f.sections[1].entsize, 0);
    ASSERT_EQ(f.sections[1].link, 0);

    ASSERT_STR_EQ(f.sections[2].name, ".shstrtab");
    ASSERT_EQ(f.sections[2].type, 3);
    ASSERT_EQ(f.sections[2].offset, MIN_STRTAB_OFF);
    ASSERT_EQ(f.sections[2].size, MIN_STRTAB_SIZE);

    elf_close(&f);
    ASSERT_TRUE(holds_nothing(&f));
}

TEST(shnum_zero_means_no_section_table) {
    unsigned char b[MIN_SIZE];
    build_min(b);
    put16(b + 60, 0);
    put16(b + 62, 0);
    ElfFile f;
    ASSERT_EQ(parse_bytes(b, sizeof b, &f), NSH_OK);
    ASSERT_EQ(f.nsections, 0);
    ASSERT_TRUE(f.sections == NULL);
    ASSERT_EQ(f.nsymbols, 0);
    elf_close(&f);
}

TEST(shstrndx_undef_leaves_names_empty) {
    unsigned char b[MIN_SIZE];
    build_min(b);
    put16(b + 62, 0);
    ElfFile f;
    ASSERT_EQ(parse_bytes(b, sizeof b, &f), NSH_OK);
    ASSERT_EQ(f.nsections, 3);
    for (size_t i = 0; i < f.nsections; i++) {
        ASSERT_STR_EQ(f.sections[i].name, "");
    }
    elf_close(&f);
}

TEST(a_larger_declared_shentsize_is_the_stride) {
    // 72-byte headers: the table grows, the fields still land where declared.
    unsigned char b[MIN_SIZE + 24];
    memset(b, 0, sizeof b);
    build_min(b);
    put16(b + 58, 72);
    unsigned char shdrs[192];
    memcpy(shdrs, b + MIN_SHOFF, sizeof shdrs);
    memset(b + MIN_SHOFF, 0, sizeof b - MIN_SHOFF);
    for (int i = 0; i < 3; i++) {
        memcpy(b + MIN_SHOFF + i * 72, shdrs + i * 64, 64);
    }
    ElfFile f;
    ASSERT_EQ(parse_bytes(b, sizeof b, &f), NSH_OK);
    ASSERT_EQ(f.nsections, 3);
    ASSERT_STR_EQ(f.sections[1].name, ".text");
    ASSERT_STR_EQ(f.sections[2].name, ".shstrtab");
    elf_close(&f);
}

TEST(a_segment_table_is_read) {
    unsigned char b[MIN_SIZE];
    build_min(b);
    // One PT_LOAD program header parked in the unused 0x64..0x80 gap is too
    // small, so the header goes over .text and .text shrinks out of the way.
    put16(b + 56, 1);
    put64(b + 32, 0x40);
    put16(b + 54, 56);
    put_shdr(b + MIN_SHDR1, 1, 1, 6, 0x1000, MIN_TEXT_OFF, 0, 0, 16, 0);
    unsigned char ph[56];
    memset(ph, 0, sizeof ph);
    put32(ph + 0, 1);        // p_type PT_LOAD
    put32(ph + 4, 5);        // p_flags R+X
    put64(ph + 8, 0);        // p_offset
    put64(ph + 16, 0x1000);  // p_vaddr
    put64(ph + 32, 0x40);    // p_filesz
    put64(ph + 40, 0x80);    // p_memsz
    put64(ph + 48, 0x1000);  // p_align
    // The phdr overwrites the shstrtab bytes, so names come back empty.
    put16(b + 62, 0);
    memcpy(b + 0x40, ph, sizeof ph);

    ElfFile f;
    ASSERT_EQ(parse_bytes(b, sizeof b, &f), NSH_OK);
    ASSERT_EQ(f.nsegments, 1);
    ASSERT_EQ(f.segments[0].type, 1);
    ASSERT_EQ(f.segments[0].flags, 5);
    ASSERT_EQ(f.segments[0].offset, 0);
    ASSERT_EQ(f.segments[0].vaddr, 0x1000);
    ASSERT_EQ(f.segments[0].filesz, 0x40);
    ASSERT_EQ(f.segments[0].memsz, 0x80);
    ASSERT_EQ(f.segments[0].align, 0x1000);
    elf_close(&f);
}

TEST(symbols_come_from_symtab) {
    unsigned char b[SYM_SIZE_TOTAL];
    build_sym(b);
    ElfFile f;
    ASSERT_EQ(parse_bytes(b, sizeof b, &f), NSH_OK);
    ASSERT_EQ(f.hdr.type, 1);
    ASSERT_EQ(f.nsections, 5);
    ASSERT_EQ(f.symbols_dynamic, false);
    ASSERT_EQ(f.nsymbols, 2);

    // Index 0 is the null symbol and is kept, the way readelf shows it.
    ASSERT_STR_EQ(f.symbols[0].name, "");
    ASSERT_EQ(f.symbols[0].value, 0);
    ASSERT_EQ(f.symbols[0].bind, 0);
    ASSERT_EQ(f.symbols[0].type, 0);
    ASSERT_EQ(f.symbols[0].shndx, 0);

    ASSERT_STR_EQ(f.symbols[1].name, "main");
    ASSERT_EQ(f.symbols[1].value, 0x1000);
    ASSERT_EQ(f.symbols[1].size, 4);
    ASSERT_EQ(f.symbols[1].bind, 1);
    ASSERT_EQ(f.symbols[1].type, 2);
    ASSERT_EQ(f.symbols[1].shndx, 1);
    elf_close(&f);
}

static void c_bad_magic(unsigned char *b) { b[1] = 'X'; }
static void c_class32(unsigned char *b) { b[4] = 1; }
static void c_class_zero(unsigned char *b) { b[4] = 0; }
static void c_big_endian(unsigned char *b) { b[5] = 2; }
static void c_ident_version(unsigned char *b) { b[6] = 0; }
static void c_shoff_past_eof(unsigned char *b) { put64(b + 40, 0x100000); }
static void c_shoff_wraps(unsigned char *b) { put64(b + 40, UINT64_MAX - 8); }
static void c_shnum_huge(unsigned char *b) { put16(b + 60, 0xffff); }
static void c_shentsize_zero(unsigned char *b) { put16(b + 58, 0); }
static void c_shentsize_short(unsigned char *b) { put16(b + 58, 63); }
static void c_shstrndx_oob(unsigned char *b) { put16(b + 62, 3); }
static void c_shstrndx_max(unsigned char *b) { put16(b + 62, 0xffff); }

static void c_shstrtab_past_eof(unsigned char *b) {
    put64(b + MIN_SHDR2 + 24, MIN_SIZE);
    put64(b + MIN_SHDR2 + 32, 64);
}

static void c_shstrtab_is_nobits(unsigned char *b) {
    put32(b + MIN_SHDR2 + 4, 8);
}

static void c_section_past_eof(unsigned char *b) {
    put64(b + MIN_SHDR1 + 24, MIN_SIZE + 1);
}

static void c_section_size_wraps(unsigned char *b) {
    put64(b + MIN_SHDR1 + 24, MIN_SIZE - 1);
    put64(b + MIN_SHDR1 + 32, UINT64_MAX);
}

static void c_name_index_oob(unsigned char *b) {
    put32(b + MIN_SHDR1 + 0, 100);
}

static void c_name_unterminated(unsigned char *b) {
    // Kill the NUL that ends ".shstrtab" at the very end of the table.
    b[MIN_STRTAB_OFF + MIN_STRTAB_SIZE - 1] = 'X';
}

static void c_phoff_past_eof(unsigned char *b) {
    put16(b + 56, 1);
    put64(b + 32, 0x100000);
}

static void c_phentsize_short(unsigned char *b) {
    put16(b + 56, 1);
    put16(b + 54, 8);
    put64(b + 32, 0x40);
}

static void c_segment_filesz_past_eof(unsigned char *b) {
    put16(b + 56, 1);
    put64(b + 32, 0x40);
    put64(b + 0x40 + 32, UINT64_MAX);
}

typedef struct {
    const char *name;
    void (*fn)(unsigned char *);
} CorruptCase;

static const CorruptCase MIN_CASES[] = {
    {"bad_magic", c_bad_magic},
    {"class32", c_class32},
    {"class_zero", c_class_zero},
    {"big_endian", c_big_endian},
    {"ident_version", c_ident_version},
    {"shoff_past_eof", c_shoff_past_eof},
    {"shoff_wraps", c_shoff_wraps},
    {"shnum_huge", c_shnum_huge},
    {"shentsize_zero", c_shentsize_zero},
    {"shentsize_short", c_shentsize_short},
    {"shstrndx_oob", c_shstrndx_oob},
    {"shstrndx_max", c_shstrndx_max},
    {"shstrtab_past_eof", c_shstrtab_past_eof},
    {"shstrtab_is_nobits", c_shstrtab_is_nobits},
    {"section_past_eof", c_section_past_eof},
    {"section_size_wraps", c_section_size_wraps},
    {"name_index_oob", c_name_index_oob},
    {"name_unterminated", c_name_unterminated},
    {"phoff_past_eof", c_phoff_past_eof},
    {"phentsize_short", c_phentsize_short},
    {"segment_filesz_past_eof", c_segment_filesz_past_eof},
};

TEST(every_corruption_of_the_fixture_is_rejected) {
    for (size_t i = 0; i < sizeof MIN_CASES / sizeof MIN_CASES[0]; i++) {
        unsigned char b[MIN_SIZE];
        build_min(b);
        MIN_CASES[i].fn(b);
        ElfFile f;
        NshError err = parse_bytes(b, sizeof b, &f);
        if (err != NSH_ERR_INVALID) {
            printf("    case %s returned %d\n", MIN_CASES[i].name, (int)err);
        }
        elf_close(&f);
        ASSERT_EQ(err, NSH_ERR_INVALID);
        ASSERT_TRUE(holds_nothing(&f));
    }
}

static void s_entsize_short(unsigned char *b) {
    put64(b + SYM_SHDR_SYMTAB + 56, 16);
}

static void s_entsize_zero(unsigned char *b) {
    put64(b + SYM_SHDR_SYMTAB + 56, 0);
}

static void s_link_oob(unsigned char *b) {
    put32(b + SYM_SHDR_SYMTAB + 40, 9);
}

static void s_link_not_strtab(unsigned char *b) {
    put32(b + SYM_SHDR_SYMTAB + 40, 1);
}

static void s_symtab_past_eof(unsigned char *b) {
    put64(b + SYM_SHDR_SYMTAB + 24, SYM_SIZE_TOTAL - 8);
}

static void s_sym_name_oob(unsigned char *b) {
    put32(b + SYM_SYMTAB_OFF + 24, 999);
}

static void s_sym_name_unterminated(unsigned char *b) {
    b[SYM_STRTAB_OFF + SYM_STRTAB_SIZE - 1] = 'X';
}

static const CorruptCase SYM_CASES[] = {
    {"entsize_short", s_entsize_short},
    {"entsize_zero", s_entsize_zero},
    {"link_oob", s_link_oob},
    {"link_not_strtab", s_link_not_strtab},
    {"symtab_past_eof", s_symtab_past_eof},
    {"sym_name_oob", s_sym_name_oob},
    {"sym_name_unterminated", s_sym_name_unterminated},
};

TEST(every_corruption_of_the_symbol_fixture_is_rejected) {
    for (size_t i = 0; i < sizeof SYM_CASES / sizeof SYM_CASES[0]; i++) {
        unsigned char b[SYM_SIZE_TOTAL];
        build_sym(b);
        SYM_CASES[i].fn(b);
        ElfFile f;
        NshError err = parse_bytes(b, sizeof b, &f);
        if (err != NSH_ERR_INVALID) {
            printf("    case %s returned %d\n", SYM_CASES[i].name, (int)err);
        }
        elf_close(&f);
        ASSERT_EQ(err, NSH_ERR_INVALID);
        ASSERT_TRUE(holds_nothing(&f));
    }
}

TEST(every_truncation_is_rejected_without_crashing) {
    unsigned char b[MIN_SIZE];
    build_min(b);
    for (size_t len = 0; len < MIN_SIZE; len++) {
        ElfFile f;
        NshError err = parse_bytes(b, len, &f);
        if (err != NSH_ERR_INVALID) {
            printf("    length %zu returned %d\n", len, (int)err);
        }
        elf_close(&f);
        ASSERT_EQ(err, NSH_ERR_INVALID);
        ASSERT_TRUE(holds_nothing(&f));
    }
}

TEST(bad_paths_report_io_and_bad_arguments_report_invalid) {
    ElfFile f;
    ASSERT_EQ(elf_open("/nsh/no/such/file", &f), NSH_ERR_IO);
    ASSERT_TRUE(holds_nothing(&f));
    ASSERT_EQ(elf_open(g_tmp, &f), NSH_ERR_INVALID);
    ASSERT_TRUE(holds_nothing(&f));
    ASSERT_EQ(elf_open(NULL, &f), NSH_ERR_INVALID);
    ASSERT_EQ(elf_open("/bin/true", NULL), NSH_ERR_INVALID);
}

TEST(bin_true_parses_like_a_real_binary) {
    if (access("/bin/true", R_OK) != 0) {
        printf("    /bin/true missing, skipped\n");
        return;
    }
    ElfFile f;
    ASSERT_EQ(elf_open("/bin/true", &f), NSH_OK);
    ASSERT_TRUE(f.hdr.type == ET_DYN || f.hdr.type == ET_EXEC);
    ASSERT_EQ(f.hdr.machine, EM_X86_64);
    ASSERT_TRUE(f.nsections > 0);
    ASSERT_TRUE(find_section(&f, ".text") != NULL);
    ASSERT_TRUE(f.nsegments > 0);
    ASSERT_TRUE(has_segment_type(&f, PT_LOAD));

    // Whichever table it kept, the flag has to match where it came from.
    if (f.nsymbols > 0) {
        bool has_symtab = false;
        for (size_t i = 0; i < f.nsections; i++) {
            has_symtab = has_symtab || f.sections[i].type == SHT_SYMTAB;
        }
        ASSERT_EQ(f.symbols_dynamic, !has_symtab);
    }
    elf_close(&f);
    ASSERT_TRUE(holds_nothing(&f));
}

TEST(our_layout_matches_the_system_headers) {
    ASSERT_EQ(sizeof(Elf64_Ehdr), 64);
    ASSERT_EQ(sizeof(Elf64_Shdr), 64);
    ASSERT_EQ(sizeof(Elf64_Phdr), 56);
    ASSERT_EQ(sizeof(Elf64_Sym), 24);
    ASSERT_EQ(offsetof(Elf64_Ehdr, e_shoff), 0x28);
    ASSERT_EQ(offsetof(Elf64_Ehdr, e_phoff), 32);
    ASSERT_EQ(offsetof(Elf64_Ehdr, e_shnum), 60);
    ASSERT_EQ(offsetof(Elf64_Shdr, sh_offset), 24);
    ASSERT_EQ(offsetof(Elf64_Shdr, sh_entsize), 56);
    ASSERT_EQ(offsetof(Elf64_Phdr, p_filesz), 32);
    ASSERT_EQ(offsetof(Elf64_Sym, st_value), 8);

    if (access("/bin/true", R_OK) != 0) {
        printf("    /bin/true missing, skipped the field cross-check\n");
        return;
    }
    Elf64_Ehdr raw;
    FILE *fp = fopen("/bin/true", "rb");
    ASSERT_TRUE(fp != NULL);
    size_t got = fread(&raw, 1, sizeof raw, fp);
    fclose(fp);
    ASSERT_EQ(got, sizeof raw);

    ElfFile f;
    ASSERT_EQ(elf_open("/bin/true", &f), NSH_OK);
    ASSERT_EQ(f.hdr.shnum, raw.e_shnum);
    ASSERT_EQ(f.hdr.phnum, raw.e_phnum);
    ASSERT_EQ(f.hdr.shoff, raw.e_shoff);
    ASSERT_EQ(f.hdr.entry, raw.e_entry);
    ASSERT_EQ(f.hdr.shstrndx, raw.e_shstrndx);
    elf_close(&f);
}

TEST(the_test_binary_lists_its_own_symbols) {
    ElfFile f;
    ASSERT_EQ(elf_open("/proc/self/exe", &f), NSH_OK);
    ASSERT_TRUE(f.nsections > 0);
    ASSERT_TRUE(f.nsymbols > 0);
    bool found_main = false;
    for (size_t i = 0; i < f.nsymbols; i++) {
        found_main = found_main || strcmp(f.symbols[i].name, "main") == 0;
    }
    if (!found_main) {
        printf("    no \"main\" symbol, binary looks stripped\n");
    }
    elf_close(&f);
    ASSERT_TRUE(holds_nothing(&f));
}

TEST(close_is_idempotent_and_the_struct_is_reusable) {
    ElfFile f;
    memset(&f, 0, sizeof f);
    elf_close(&f);
    elf_close(NULL);

    unsigned char b[MIN_SIZE];
    build_min(b);
    ASSERT_EQ(parse_bytes(b, sizeof b, &f), NSH_OK);
    elf_close(&f);
    elf_close(&f);
    ASSERT_TRUE(holds_nothing(&f));

    // The same struct takes a second file, then a failing parse, then a third.
    ASSERT_EQ(elf_open("/bin/true", &f), NSH_OK);
    ASSERT_TRUE(f.nsections > 0);
    elf_close(&f);

    build_min(b);
    c_bad_magic(b);
    ASSERT_EQ(parse_bytes(b, sizeof b, &f), NSH_ERR_INVALID);
    ASSERT_TRUE(holds_nothing(&f));

    build_min(b);
    ASSERT_EQ(parse_bytes(b, sizeof b, &f), NSH_OK);
    ASSERT_EQ(f.nsections, 3);
    elf_close(&f);
    ASSERT_TRUE(holds_nothing(&f));
}

static void setup(void) {
    snprintf(g_tmp, sizeof g_tmp, "/tmp/nsh_elf_XXXXXX");
    if (mkdtemp(g_tmp) == NULL) {
        fprintf(stderr, "elf_test: mkdtemp failed\n");
        exit(1);
    }
}

static void teardown(void) {
    char cmd[CMD_BUF];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_tmp);
    if (system(cmd) != 0) {
        fprintf(stderr, "elf_test: could not remove %s\n", g_tmp);
    }
}

// Every case writes fixtures into the scratch directory, so main is spelled out.
int main(void) {
    setup();
    int failures = 0;
    for (int i = 0; i < nsh_test_count; i++) {
        int failed = 0;
        nsh_test_cases[i].fn(&failed);
        printf("  %s %s\n", failed ? "FAIL" : "ok  ", nsh_test_cases[i].name);
        failures += failed;
    }
    printf("  %d test(s), %d failed\n", nsh_test_count, failures);
    teardown();
    return failures == 0 ? 0 : 1;
}
