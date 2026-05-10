// Tests for the heap builtin: subcommand statuses, error shapes, and switching.

#include "heap_builtin.h"

#include <stdbool.h>
#include <string.h>

#include "../../tests/harness.h"
#include "alloc.h"

static void shell_start(Shell *sh) {
    (void)history_init(&sh->history, 16);
    sh->last_status = 0;
    sh->want_exit = false;
    sh->exit_code = 0;
}

static void shell_stop(Shell *sh) {
    history_free(&sh->history);
}

// Every strategy-changing test ends here, so test order cannot leak state.
static void restore_firstfit(void) {
    (void)alloc_set_strategy("firstfit");
}

TEST(bare_heap_and_stats_succeed) {
    Shell sh;
    shell_start(&sh);

    char *bare[] = {"heap", NULL};
    ASSERT_EQ(heap_builtin(&sh, 1, bare), 0);

    char *stats[] = {"heap", "stats", NULL};
    ASSERT_EQ(heap_builtin(&sh, 2, stats), 0);

    shell_stop(&sh);
}

TEST(heap_dump_succeeds) {
    Shell sh;
    shell_start(&sh);

    char *argv[] = {"heap", "dump", NULL};
    ASSERT_EQ(heap_builtin(&sh, 2, argv), 0);

    shell_stop(&sh);
}

TEST(heap_strategy_without_a_name_succeeds) {
    Shell sh;
    shell_start(&sh);

    char *argv[] = {"heap", "strategy", NULL};
    ASSERT_EQ(heap_builtin(&sh, 2, argv), 0);
    ASSERT_TRUE(alloc_strategy_name() != NULL);

    shell_stop(&sh);
}

TEST(heap_rejects_unknown_and_overlong_subcommands) {
    Shell sh;
    shell_start(&sh);

    char *unknown[] = {"heap", "frobnicate", NULL};
    ASSERT_EQ(heap_builtin(&sh, 2, unknown), 1);

    char *stats_extra[] = {"heap", "stats", "x", NULL};
    ASSERT_EQ(heap_builtin(&sh, 3, stats_extra), 1);

    char *dump_extra[] = {"heap", "dump", "x", NULL};
    ASSERT_EQ(heap_builtin(&sh, 3, dump_extra), 1);

    char *strategy_extra[] = {"heap", "strategy", "firstfit", "buddy", NULL};
    ASSERT_EQ(heap_builtin(&sh, 4, strategy_extra), 1);

    char *empty[] = {"heap", "", NULL};
    ASSERT_EQ(heap_builtin(&sh, 2, empty), 1);

    shell_stop(&sh);
}

TEST(heap_rejects_an_unknown_strategy_and_keeps_the_active_one) {
    Shell sh;
    shell_start(&sh);

    const char *before = alloc_strategy_name();
    char *argv[] = {"heap", "strategy", "slab", NULL};
    ASSERT_EQ(heap_builtin(&sh, 3, argv), 1);
    ASSERT_STR_EQ(alloc_strategy_name(), before);

    shell_stop(&sh);
}

TEST(heap_strategy_buddy_switches_the_live_arena) {
    Shell sh;
    shell_start(&sh);

    char *to_buddy[] = {"heap", "strategy", "buddy", NULL};
    ASSERT_EQ(heap_builtin(&sh, 3, to_buddy), 0);
    ASSERT_STR_EQ(alloc_strategy_name(), "buddy");

    AllocStats before;
    ASSERT_EQ(alloc_get_stats(&before), NSH_OK);
    ASSERT_STR_EQ(before.strategy, "buddy");

    void *p = nsh_malloc(4096);
    AllocStats after;
    ASSERT_EQ(alloc_get_stats(&after), NSH_OK);
    nsh_free(p);
    restore_firstfit();

    ASSERT_STR_EQ(after.strategy, "buddy");
    ASSERT_TRUE(after.arena_size > 0);
    ASSERT_TRUE(after.used_bytes > before.used_bytes);
    ASSERT_TRUE(after.live_blocks > before.live_blocks);
    ASSERT_TRUE(after.total_mallocs > before.total_mallocs);

    shell_stop(&sh);
}

TEST(heap_strategy_firstfit_switches_back) {
    Shell sh;
    shell_start(&sh);

    char *to_buddy[] = {"heap", "strategy", "buddy", NULL};
    ASSERT_EQ(heap_builtin(&sh, 3, to_buddy), 0);

    char *to_firstfit[] = {"heap", "strategy", "firstfit", NULL};
    ASSERT_EQ(heap_builtin(&sh, 3, to_firstfit), 0);
    ASSERT_STR_EQ(alloc_strategy_name(), "firstfit");

    AllocStats before;
    ASSERT_EQ(alloc_get_stats(&before), NSH_OK);
    void *p = nsh_malloc(4096);
    AllocStats after;
    ASSERT_EQ(alloc_get_stats(&after), NSH_OK);
    nsh_free(p);
    restore_firstfit();

    ASSERT_STR_EQ(after.strategy, "firstfit");
    ASSERT_TRUE(after.used_bytes > before.used_bytes);

    shell_stop(&sh);
}

TEST_MAIN()
