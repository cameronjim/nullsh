// The heap builtin: stats, strategy reporting and switching, and the arena dump.

#include "heap_builtin.h"

#include <stdio.h>
#include <string.h>

#include "alloc.h"

// Widest key is "total mallocs", so every value starts in the same column.
#define HEAP_KEY_WIDTH 13

static void heap_row(const char *key, unsigned long long value) {
    printf("%-*s  %llu\n", HEAP_KEY_WIDTH, key, value);
}

static void heap_usage(void) {
    fputs("nullsh: heap: usage: heap [stats | strategy [NAME] | dump]\n", stderr);
}

static int heap_print_stats(void) {
    AllocStats st;
    if (alloc_get_stats(&st) != NSH_OK) {
        fputs("nullsh: heap: stats unavailable\n", stderr);
        return 1;
    }
    printf("%-*s  %s\n", HEAP_KEY_WIDTH, "strategy", st.strategy);
    heap_row("arena size", (unsigned long long)st.arena_size);
    heap_row("used bytes", (unsigned long long)st.used_bytes);
    heap_row("free bytes", (unsigned long long)st.free_bytes);
    heap_row("live blocks", (unsigned long long)st.live_blocks);
    heap_row("free blocks", (unsigned long long)st.free_blocks);
    heap_row("largest free", (unsigned long long)st.largest_free);
    heap_row("total mallocs", st.total_mallocs);
    heap_row("total frees", st.total_frees);
    return 0;
}

static int heap_strategy(int argc, char **argv) {
    if (argc == 2) {
        printf("%s\n", alloc_strategy_name());
        return 0;
    }
    if (alloc_set_strategy(argv[2]) != NSH_OK) {
        fprintf(stderr, "nullsh: heap: unknown strategy %s (firstfit, buddy)\n",
                argv[2]);
        return 1;
    }
    return 0;
}

int heap_builtin(Shell *sh, int argc, char **argv) {
    (void)sh;
    if (argc < 2) {
        return heap_print_stats();
    }
    if (strcmp(argv[1], "stats") == 0 && argc == 2) {
        return heap_print_stats();
    }
    if (strcmp(argv[1], "strategy") == 0 && argc <= 3) {
        return heap_strategy(argc, argv);
    }
    if (strcmp(argv[1], "dump") == 0 && argc == 2) {
        alloc_dump(stdout);
        return 0;
    }
    heap_usage();
    return 1;
}
