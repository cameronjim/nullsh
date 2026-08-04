// The inspect builtin: open an ELF file and print the view the flags asked for.

#include "inspect.h"

#include <stdio.h>
#include <string.h>

#include "elf.h"
#include "print.h"

#define WANT_HEADER   0x1
#define WANT_SECTIONS 0x2
#define WANT_SEGMENTS 0x4
#define WANT_SYMBOLS  0x8

static void inspect_usage(void) {
    fputs("nullsh: inspect: usage: inspect [--sections | --segments | "
          "--symbols | --all] FILE\n",
          stderr);
}

// 0 when the word is not one of the four flags.
static unsigned flag_bit(const char *arg) {
    if (strcmp(arg, "--sections") == 0) {
        return WANT_SECTIONS;
    }
    if (strcmp(arg, "--segments") == 0) {
        return WANT_SEGMENTS;
    }
    if (strcmp(arg, "--symbols") == 0) {
        return WANT_SYMBOLS;
    }
    if (strcmp(arg, "--all") == 0) {
        return WANT_HEADER | WANT_SECTIONS | WANT_SEGMENTS | WANT_SYMBOLS;
    }
    return 0;
}

static void report_open_error(const char *path, NshError err) {
    if (err == NSH_ERR_IO) {
        fprintf(stderr, "nullsh: inspect: %s: cannot open\n", path);
    } else {
        fprintf(stderr, "nullsh: inspect: %s: not a supported elf file\n",
                path);
    }
}

// A blank line separates the parts, never leads and never trails.
static void print_part(const ElfFile *f, unsigned part, bool *first) {
    if (!*first) {
        fputc('\n', stdout);
    }
    *first = false;
    switch (part) {
    case WANT_SECTIONS:
        elf_print_sections(f, stdout);
        break;
    case WANT_SEGMENTS:
        elf_print_segments(f, stdout);
        break;
    case WANT_SYMBOLS:
        elf_print_symbols(f, stdout);
        break;
    default:
        elf_print_header(f, stdout);
        break;
    }
}

int inspect_builtin(Shell *sh, int argc, char **argv) {
    (void)sh;
    unsigned want = 0;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] == '-' && arg[1] != '\0') {
            unsigned bit = flag_bit(arg);
            if (bit == 0) {
                inspect_usage();
                return 1;
            }
            want |= bit;
            continue;
        }
        if (path != NULL) {
            inspect_usage();
            return 1;
        }
        path = arg;
    }

    if (path == NULL) {
        inspect_usage();
        return 1;
    }
    if (want == 0) {
        want = WANT_HEADER;
    }

    ElfFile f;
    NshError err = elf_open(path, &f);
    if (err != NSH_OK) {
        report_open_error(path, err);
        return 1;
    }

    bool first = true;
    if (want & WANT_HEADER) {
        print_part(&f, WANT_HEADER, &first);
    }
    if (want & WANT_SECTIONS) {
        print_part(&f, WANT_SECTIONS, &first);
    }
    if (want & WANT_SEGMENTS) {
        print_part(&f, WANT_SEGMENTS, &first);
    }
    if (want & WANT_SYMBOLS) {
        print_part(&f, WANT_SYMBOLS, &first);
    }

    elf_close(&f);
    return 0;
}
