// Tests for the ELF printer: every column, every name table, and empty tables.

#define _POSIX_C_SOURCE 200809L

#include "print.h"

#include <stdlib.h>
#include <string.h>

#include "../../tests/harness.h"

#define LINE_MAX_LEN 256

typedef void (*PrintFn)(const ElfFile *, FILE *);

// NULL when the memory stream could not be opened. Caller frees with free.
static char *render(PrintFn fn, const ElfFile *f) {
    char *buf = NULL;
    size_t len = 0;
    FILE *out = open_memstream(&buf, &len);
    if (out == NULL) {
        return NULL;
    }
    fn(f, out);
    fclose(out);
    return buf;
}

static const char *line_of(const char *text, size_t n, char *buf) {
    const char *p = text;
    for (size_t i = 0; i < n && p != NULL; i++) {
        p = strchr(p, '\n');
        if (p != NULL) {
            p++;
        }
    }
    if (p == NULL) {
        buf[0] = '\0';
        return buf;
    }
    const char *end = strchr(p, '\n');
    size_t len = (end == NULL) ? strlen(p) : (size_t)(end - p);
    if (len >= LINE_MAX_LEN) {
        len = LINE_MAX_LEN - 1;
    }
    memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

static size_t line_count(const char *text) {
    size_t n = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\n') {
            n++;
        }
    }
    return n;
}

TEST(header_names_the_class_type_machine_entry_and_counts) {
    ElfFile f = {0};
    f.hdr.type = 3;
    f.hdr.machine = 0x3e;
    f.hdr.entry = 0x1060;
    f.hdr.phnum = 13;
    f.hdr.shnum = 31;
    f.hdr.shstrndx = 30;

    char *text = render(elf_print_header, &f);
    ASSERT_TRUE(text != NULL);
    char line[LINE_MAX_LEN];

    ASSERT_STR_EQ(line_of(text, 0, line), "class     ELF64 little endian");
    ASSERT_STR_EQ(line_of(text, 1, line), "type      DYN (0x3)");
    ASSERT_STR_EQ(line_of(text, 2, line), "machine   x86-64 (0x3e)");
    ASSERT_STR_EQ(line_of(text, 3, line), "entry     0x0000000000001060");
    ASSERT_STR_EQ(line_of(text, 4, line), "sections  31");
    ASSERT_STR_EQ(line_of(text, 5, line), "segments  13");
    ASSERT_STR_EQ(line_of(text, 6, line), "shstrndx  30");
    ASSERT_EQ(line_count(text), 7);

    free(text);
}

TEST(header_names_the_other_object_kinds) {
    ElfFile f = {0};
    char line[LINE_MAX_LEN];

    f.hdr.type = 1;
    char *rel = render(elf_print_header, &f);
    ASSERT_TRUE(rel != NULL);
    ASSERT_STR_EQ(line_of(rel, 1, line), "type      REL (0x1)");
    free(rel);

    f.hdr.type = 2;
    char *exec = render(elf_print_header, &f);
    ASSERT_TRUE(exec != NULL);
    ASSERT_STR_EQ(line_of(exec, 1, line), "type      EXEC (0x2)");
    free(exec);

    f.hdr.type = 4;
    char *core = render(elf_print_header, &f);
    ASSERT_TRUE(core != NULL);
    ASSERT_STR_EQ(line_of(core, 1, line), "type      CORE (0x4)");
    free(core);
}

TEST(header_falls_back_to_hex_for_unknown_type_and_machine) {
    ElfFile f = {0};
    f.hdr.type = 0xfe00;
    f.hdr.machine = 0x28;

    char *text = render(elf_print_header, &f);
    ASSERT_TRUE(text != NULL);
    char line[LINE_MAX_LEN];

    ASSERT_STR_EQ(line_of(text, 1, line), "type      0xfe00");
    ASSERT_STR_EQ(line_of(text, 2, line), "machine   0x28");

    free(text);
}

TEST(section_rows_name_types_and_spell_out_wax_flags) {
    ElfSection sections[] = {
        {".text", 1, 0x6, 0x1050, 0x1050, 0x123, 0, 0},
        {".data", 1, 0x3, 0x4000, 0x3000, 0x10, 0, 0},
        {".bss", 8, 0x3, 0x4010, 0x3010, 0x40, 0, 0},
        {".comment", 1, 0x0, 0, 0x3010, 0x2b, 1, 0},
        {".weird", 0x70000001, 0x7, 0x5000, 0x4000, 0x8, 0, 0},
    };
    ElfFile f = {0};
    f.sections = sections;
    f.nsections = 5;

    char *text = render(elf_print_sections, &f);
    ASSERT_TRUE(text != NULL);
    char line[LINE_MAX_LEN];

    ASSERT_STR_EQ(
        line_of(text, 1, line),
        "[ 0]  .text                 PROGBITS    0x0000000000001050  "
        "0x00001050  0x00000123  AX");
    ASSERT_STR_EQ(
        line_of(text, 2, line),
        "[ 1]  .data                 PROGBITS    0x0000000000004000  "
        "0x00003000  0x00000010  WA");
    ASSERT_STR_EQ(
        line_of(text, 3, line),
        "[ 2]  .bss                  NOBITS      0x0000000000004010  "
        "0x00003010  0x00000040  WA");
    ASSERT_STR_EQ(
        line_of(text, 4, line),
        "[ 3]  .comment              PROGBITS    0x0000000000000000  "
        "0x00003010  0x0000002b  -");
    ASSERT_STR_EQ(
        line_of(text, 5, line),
        "[ 4]  .weird                0x70000001  0x0000000000005000  "
        "0x00004000  0x00000008  WAX");
    ASSERT_EQ(line_count(text), 6);

    free(text);
}

TEST(section_rows_cover_the_remaining_type_names) {
    ElfSection sections[] = {
        {".symtab", 2, 0, 0, 0, 0, 24, 0}, {".strtab", 3, 0, 0, 0, 0, 0, 0},
        {".rela.dyn", 4, 2, 0, 0, 0, 24, 0}, {".dynamic", 6, 3, 0, 0, 0, 16, 0},
        {".note.abi", 7, 2, 0, 0, 0, 0, 0}, {".dynsym", 11, 2, 0, 0, 0, 24, 0},
    };
    ElfFile f = {0};
    f.sections = sections;
    f.nsections = 6;

    char *text = render(elf_print_sections, &f);
    ASSERT_TRUE(text != NULL);
    char line[LINE_MAX_LEN];

    ASSERT_TRUE(strstr(line_of(text, 1, line), "SYMTAB") != NULL);
    ASSERT_TRUE(strstr(line_of(text, 2, line), "STRTAB") != NULL);
    ASSERT_TRUE(strstr(line_of(text, 3, line), "RELA") != NULL);
    ASSERT_TRUE(strstr(line_of(text, 4, line), "DYNAMIC") != NULL);
    ASSERT_TRUE(strstr(line_of(text, 5, line), "NOTE") != NULL);
    ASSERT_TRUE(strstr(line_of(text, 6, line), "DYNSYM") != NULL);

    free(text);
}

TEST(segment_rows_spell_out_rwx_and_the_gnu_types) {
    ElfSegment segments[] = {
        {1, 0x5, 0x1000, 0x1000, 0x200, 0x200, 0x1000},
        {0x6474e551, 0x6, 0, 0, 0, 0, 0x10},
        {0x6474e552, 0x4, 0x2d80, 0x3d80, 0x280, 0x280, 0x1},
        {0x6474e550, 0x4, 0x2004, 0x2004, 0x44, 0x44, 0x4},
        {0x6474e553, 0x0, 0x338, 0x338, 0x20, 0x20, 0x8},
    };
    ElfFile f = {0};
    f.segments = segments;
    f.nsegments = 5;

    char *text = render(elf_print_segments, &f);
    ASSERT_TRUE(text != NULL);
    char line[LINE_MAX_LEN];

    ASSERT_STR_EQ(
        line_of(text, 1, line),
        "[ 0]  LOAD          r-x    0x00001000  0x0000000000001000  "
        "0x00000200  0x00000200  0x1000");
    ASSERT_STR_EQ(
        line_of(text, 2, line),
        "[ 1]  GNU_STACK     rw-    0x00000000  0x0000000000000000  "
        "0x00000000  0x00000000  0x10");
    ASSERT_STR_EQ(
        line_of(text, 3, line),
        "[ 2]  GNU_RELRO     r--    0x00002d80  0x0000000000003d80  "
        "0x00000280  0x00000280  0x1");
    ASSERT_STR_EQ(
        line_of(text, 4, line),
        "[ 3]  GNU_EH_FRAME  r--    0x00002004  0x0000000000002004  "
        "0x00000044  0x00000044  0x4");
    ASSERT_STR_EQ(
        line_of(text, 5, line),
        "[ 4]  0x6474e553    ---    0x00000338  0x0000000000000338  "
        "0x00000020  0x00000020  0x8");
    ASSERT_EQ(line_count(text), 6);

    free(text);
}

TEST(segment_rows_cover_the_plain_types) {
    ElfSegment segments[] = {
        {6, 0x4, 0x40, 0x40, 0x2d8, 0x2d8, 8},
        {3, 0x4, 0x318, 0x318, 0x1c, 0x1c, 1},
        {4, 0x4, 0x338, 0x338, 0x30, 0x30, 8},
        {2, 0x6, 0x2dd8, 0x3dd8, 0x200, 0x200, 8},
    };
    ElfFile f = {0};
    f.segments = segments;
    f.nsegments = 4;

    char *text = render(elf_print_segments, &f);
    ASSERT_TRUE(text != NULL);
    char line[LINE_MAX_LEN];

    ASSERT_TRUE(strstr(line_of(text, 1, line), "PHDR") != NULL);
    ASSERT_TRUE(strstr(line_of(text, 2, line), "INTERP") != NULL);
    ASSERT_TRUE(strstr(line_of(text, 3, line), "NOTE") != NULL);
    ASSERT_TRUE(strstr(line_of(text, 4, line), "DYNAMIC") != NULL);

    free(text);
}

TEST(symbol_rows_name_binds_and_types_under_the_symtab_label) {
    ElfSymbol symbols[] = {
        {"", 0, 0, 0, 0, 0},
        {"hello.c", 0, 0, 0, 4, 0xfff1},
        {".text", 0x1050, 0, 0, 3, 12},
        {"main", 0x1149, 23, 1, 2, 12},
        {"counter", 0x4010, 8, 2, 1, 24},
        {"odd", 0x2000, 4, 13, 10, 3},
    };
    ElfFile f = {0};
    f.symbols = symbols;
    f.nsymbols = 6;
    f.symbols_dynamic = false;

    char *text = render(elf_print_symbols, &f);
    ASSERT_TRUE(text != NULL);
    char line[LINE_MAX_LEN];

    ASSERT_STR_EQ(line_of(text, 0, line), "symbol table: .symtab (6 entries)");
    ASSERT_STR_EQ(
        line_of(text, 2, line),
        "[  0]  0x0000000000000000         0  LOCAL   NOTYPE   ");
    ASSERT_STR_EQ(
        line_of(text, 3, line),
        "[  1]  0x0000000000000000         0  LOCAL   FILE     hello.c");
    ASSERT_STR_EQ(
        line_of(text, 4, line),
        "[  2]  0x0000000000001050         0  LOCAL   SECTION  .text");
    ASSERT_STR_EQ(
        line_of(text, 5, line),
        "[  3]  0x0000000000001149        23  GLOBAL  FUNC     main");
    ASSERT_STR_EQ(
        line_of(text, 6, line),
        "[  4]  0x0000000000004010         8  WEAK    OBJECT   counter");
    ASSERT_STR_EQ(
        line_of(text, 7, line),
        "[  5]  0x0000000000002000         4  0xd     0xa      odd");
    ASSERT_EQ(line_count(text), 8);

    free(text);
}

TEST(symbols_from_dynsym_say_so) {
    ElfSymbol symbols[] = {
        {"puts", 0, 0, 1, 2, 0},
    };
    ElfFile f = {0};
    f.symbols = symbols;
    f.nsymbols = 1;
    f.symbols_dynamic = true;

    char *text = render(elf_print_symbols, &f);
    ASSERT_TRUE(text != NULL);
    char line[LINE_MAX_LEN];

    ASSERT_STR_EQ(line_of(text, 0, line), "symbol table: .dynsym (1 entries)");
    ASSERT_EQ(line_count(text), 3);

    free(text);
}

TEST(empty_tables_say_so_instead_of_printing_a_heading) {
    ElfFile f = {0};

    char *sections = render(elf_print_sections, &f);
    ASSERT_TRUE(sections != NULL);
    ASSERT_STR_EQ(sections, "no sections\n");
    free(sections);

    char *segments = render(elf_print_segments, &f);
    ASSERT_TRUE(segments != NULL);
    ASSERT_STR_EQ(segments, "no segments\n");
    free(segments);

    char *symbols = render(elf_print_symbols, &f);
    ASSERT_TRUE(symbols != NULL);
    ASSERT_STR_EQ(symbols, "no symbols\n");
    free(symbols);
}

// A count with a live pointer and a pointer with a zero count are both empty.
TEST(a_null_table_pointer_with_a_count_is_still_empty) {
    ElfSection sections[] = {{".text", 1, 0x6, 0, 0, 0, 0, 0}};
    ElfFile f = {0};
    f.sections = sections;
    f.nsections = 0;

    char *text = render(elf_print_sections, &f);
    ASSERT_TRUE(text != NULL);
    ASSERT_STR_EQ(text, "no sections\n");
    free(text);
}

TEST(the_section_heading_names_every_column) {
    ElfSection sections[] = {{".text", 1, 0x6, 0, 0, 0, 0, 0}};
    ElfFile f = {0};
    f.sections = sections;
    f.nsections = 1;

    char *text = render(elf_print_sections, &f);
    ASSERT_TRUE(text != NULL);
    char line[LINE_MAX_LEN];
    const char *head = line_of(text, 0, line);

    ASSERT_TRUE(strstr(head, "nr") != NULL);
    ASSERT_TRUE(strstr(head, "name") != NULL);
    ASSERT_TRUE(strstr(head, "type") != NULL);
    ASSERT_TRUE(strstr(head, "addr") != NULL);
    ASSERT_TRUE(strstr(head, "offset") != NULL);
    ASSERT_TRUE(strstr(head, "size") != NULL);
    ASSERT_TRUE(strstr(head, "flags") != NULL);

    free(text);
}

TEST_MAIN()
