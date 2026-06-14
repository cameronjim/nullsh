// Tests for the history ring and its file format.

#define _POSIX_C_SOURCE 200809L

#include "history.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../alloc/alloc.h"
#include "../../tests/harness.h"

// A per-process file name, so a stale or parallel run cannot interfere.
static void temp_path(char *out, size_t out_size, const char *tag) {
    snprintf(out, out_size, "/tmp/nullsh_hist_%s_%ld", tag, (long)getpid());
}

TEST(init_rejects_zero_capacity) {
    History h;
    ASSERT_EQ(history_init(&h, 0), NSH_ERR_INVALID);
}

TEST(init_starts_empty) {
    History h;
    ASSERT_EQ(history_init(&h, 4), NSH_OK);
    ASSERT_EQ(h.cap, 4);
    ASSERT_EQ(history_count(&h), 0);
    ASSERT_TRUE(history_get(&h, 0) == NULL);
    history_free(&h);
}

TEST(add_keeps_oldest_first_ordering) {
    History h;
    ASSERT_EQ(history_init(&h, 8), NSH_OK);
    history_add(&h, "one");
    history_add(&h, "two");
    history_add(&h, "three");
    ASSERT_EQ(history_count(&h), 3);
    ASSERT_STR_EQ(history_get(&h, 0), "one");
    ASSERT_STR_EQ(history_get(&h, 1), "two");
    ASSERT_STR_EQ(history_get(&h, 2), "three");
    ASSERT_TRUE(history_get(&h, 3) == NULL);
    history_free(&h);
}

TEST(add_copies_the_caller_buffer) {
    History h;
    ASSERT_EQ(history_init(&h, 4), NSH_OK);
    char scratch[8];
    memcpy(scratch, "ls -l", 6);
    history_add(&h, scratch);
    memcpy(scratch, "wiped", 6);
    ASSERT_STR_EQ(history_get(&h, 0), "ls -l");
    history_free(&h);
}

TEST(add_ignores_null_and_empty) {
    History h;
    ASSERT_EQ(history_init(&h, 4), NSH_OK);
    history_add(&h, NULL);
    history_add(&h, "");
    ASSERT_EQ(history_count(&h), 0);
    history_add(&h, "real");
    history_add(&h, NULL);
    history_add(&h, "");
    ASSERT_EQ(history_count(&h), 1);
    ASSERT_STR_EQ(history_get(&h, 0), "real");
    history_free(&h);
}

TEST(consecutive_duplicate_is_dropped) {
    History h;
    ASSERT_EQ(history_init(&h, 8), NSH_OK);
    history_add(&h, "pwd");
    history_add(&h, "pwd");
    history_add(&h, "pwd");
    ASSERT_EQ(history_count(&h), 1);
    ASSERT_STR_EQ(history_get(&h, 0), "pwd");
    history_free(&h);
}

TEST(non_consecutive_duplicate_is_kept) {
    History h;
    ASSERT_EQ(history_init(&h, 8), NSH_OK);
    history_add(&h, "pwd");
    history_add(&h, "ls");
    history_add(&h, "pwd");
    ASSERT_EQ(history_count(&h), 3);
    ASSERT_STR_EQ(history_get(&h, 0), "pwd");
    ASSERT_STR_EQ(history_get(&h, 1), "ls");
    ASSERT_STR_EQ(history_get(&h, 2), "pwd");
    history_free(&h);
}

TEST(full_ring_evicts_oldest) {
    History h;
    ASSERT_EQ(history_init(&h, 3), NSH_OK);
    history_add(&h, "a");
    history_add(&h, "b");
    history_add(&h, "c");
    history_add(&h, "d");
    history_add(&h, "e");
    ASSERT_EQ(history_count(&h), 3);
    ASSERT_STR_EQ(history_get(&h, 0), "c");
    ASSERT_STR_EQ(history_get(&h, 1), "d");
    ASSERT_STR_EQ(history_get(&h, 2), "e");
    ASSERT_TRUE(history_get(&h, 3) == NULL);
    history_free(&h);
}

// After wrapping, the newest entry is not the highest ring slot.
TEST(duplicate_check_survives_wraparound) {
    History h;
    ASSERT_EQ(history_init(&h, 2), NSH_OK);
    history_add(&h, "a");
    history_add(&h, "b");
    history_add(&h, "c");
    history_add(&h, "c");
    ASSERT_EQ(history_count(&h), 2);
    ASSERT_STR_EQ(history_get(&h, 0), "b");
    ASSERT_STR_EQ(history_get(&h, 1), "c");
    history_free(&h);
}

TEST(capacity_one_holds_only_the_newest) {
    History h;
    ASSERT_EQ(history_init(&h, 1), NSH_OK);
    history_add(&h, "first");
    history_add(&h, "second");
    ASSERT_EQ(history_count(&h), 1);
    ASSERT_STR_EQ(history_get(&h, 0), "second");
    history_free(&h);
}

TEST(save_then_load_reproduces_entries) {
    char path[128];
    temp_path(path, sizeof(path), "roundtrip");

    History h;
    ASSERT_EQ(history_init(&h, 8), NSH_OK);
    history_add(&h, "echo hi");
    history_add(&h, "cd /tmp");
    history_add(&h, "ls -la  'a b'");
    ASSERT_EQ(history_save(&h, path), NSH_OK);
    history_free(&h);

    History loaded;
    ASSERT_EQ(history_init(&loaded, 8), NSH_OK);
    ASSERT_EQ(history_load(&loaded, path), NSH_OK);
    ASSERT_EQ(history_count(&loaded), 3);
    ASSERT_STR_EQ(history_get(&loaded, 0), "echo hi");
    ASSERT_STR_EQ(history_get(&loaded, 1), "cd /tmp");
    ASSERT_STR_EQ(history_get(&loaded, 2), "ls -la  'a b'");
    history_free(&loaded);
    remove(path);
}

TEST(save_of_wrapped_ring_writes_logical_order) {
    char path[128];
    temp_path(path, sizeof(path), "wrapped");

    History h;
    ASSERT_EQ(history_init(&h, 3), NSH_OK);
    history_add(&h, "a");
    history_add(&h, "b");
    history_add(&h, "c");
    history_add(&h, "d");
    ASSERT_EQ(history_save(&h, path), NSH_OK);
    history_free(&h);

    History loaded;
    ASSERT_EQ(history_init(&loaded, 8), NSH_OK);
    ASSERT_EQ(history_load(&loaded, path), NSH_OK);
    ASSERT_EQ(history_count(&loaded), 3);
    ASSERT_STR_EQ(history_get(&loaded, 0), "b");
    ASSERT_STR_EQ(history_get(&loaded, 1), "c");
    ASSERT_STR_EQ(history_get(&loaded, 2), "d");
    history_free(&loaded);
    remove(path);
}

TEST(load_appends_to_existing_entries) {
    char path[128];
    temp_path(path, sizeof(path), "append");

    History src;
    ASSERT_EQ(history_init(&src, 4), NSH_OK);
    history_add(&src, "from file");
    ASSERT_EQ(history_save(&src, path), NSH_OK);
    history_free(&src);

    History h;
    ASSERT_EQ(history_init(&h, 4), NSH_OK);
    history_add(&h, "typed first");
    ASSERT_EQ(history_load(&h, path), NSH_OK);
    ASSERT_EQ(history_count(&h), 2);
    ASSERT_STR_EQ(history_get(&h, 0), "typed first");
    ASSERT_STR_EQ(history_get(&h, 1), "from file");
    history_free(&h);
    remove(path);
}

TEST(load_of_missing_file_is_ok_and_adds_nothing) {
    char path[128];
    temp_path(path, sizeof(path), "missing");
    remove(path);

    History h;
    ASSERT_EQ(history_init(&h, 4), NSH_OK);
    ASSERT_EQ(history_load(&h, path), NSH_OK);
    ASSERT_EQ(history_count(&h), 0);
    history_free(&h);
}

// A directory is the portable stand-in for a path that cannot be read.
TEST(load_of_unreadable_path_is_io_error) {
    History h;
    ASSERT_EQ(history_init(&h, 4), NSH_OK);
    ASSERT_EQ(history_load(&h, "/tmp"), NSH_ERR_IO);
    ASSERT_EQ(history_count(&h), 0);
    history_free(&h);
}

TEST(save_to_unwritable_path_is_io_error) {
    History h;
    ASSERT_EQ(history_init(&h, 4), NSH_OK);
    history_add(&h, "x");
    ASSERT_EQ(history_save(&h, "/tmp/nullsh_no_such_dir_here/hist"),
              NSH_ERR_IO);
    history_free(&h);
}

TEST(save_of_empty_history_creates_empty_file) {
    char path[128];
    temp_path(path, sizeof(path), "empty");

    History h;
    ASSERT_EQ(history_init(&h, 4), NSH_OK);
    ASSERT_EQ(history_save(&h, path), NSH_OK);
    history_free(&h);

    History loaded;
    ASSERT_EQ(history_init(&loaded, 4), NSH_OK);
    ASSERT_EQ(history_load(&loaded, path), NSH_OK);
    ASSERT_EQ(history_count(&loaded), 0);
    history_free(&loaded);
    remove(path);
}

// The load path must not read into a fixed buffer.
TEST(very_long_line_survives_save_and_load) {
    char path[128];
    temp_path(path, sizeof(path), "longline");

    const size_t n = 5000;
    char *big = nsh_malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        big[i] = (char)('a' + (i % 26));
    }
    big[n] = '\0';

    History h;
    ASSERT_EQ(history_init(&h, 4), NSH_OK);
    history_add(&h, "short");
    history_add(&h, big);
    history_add(&h, "after");
    ASSERT_EQ(history_save(&h, path), NSH_OK);
    history_free(&h);

    History loaded;
    ASSERT_EQ(history_init(&loaded, 4), NSH_OK);
    ASSERT_EQ(history_load(&loaded, path), NSH_OK);
    ASSERT_EQ(history_count(&loaded), 3);
    ASSERT_STR_EQ(history_get(&loaded, 0), "short");
    ASSERT_EQ(strlen(history_get(&loaded, 1)), n);
    ASSERT_STR_EQ(history_get(&loaded, 1), big);
    ASSERT_STR_EQ(history_get(&loaded, 2), "after");
    history_free(&loaded);
    nsh_free(big);
    remove(path);
}

TEST(load_reads_final_line_without_newline) {
    char path[128];
    temp_path(path, sizeof(path), "nonewline");

    FILE *f = fopen(path, "w");
    ASSERT_TRUE(f != NULL);
    ASSERT_TRUE(fputs("alpha\nbeta", f) != EOF);
    ASSERT_EQ(fclose(f), 0);

    History h;
    ASSERT_EQ(history_init(&h, 4), NSH_OK);
    ASSERT_EQ(history_load(&h, path), NSH_OK);
    ASSERT_EQ(history_count(&h), 2);
    ASSERT_STR_EQ(history_get(&h, 0), "alpha");
    ASSERT_STR_EQ(history_get(&h, 1), "beta");
    history_free(&h);
    remove(path);
}

TEST(load_skips_blank_lines) {
    char path[128];
    temp_path(path, sizeof(path), "blanks");

    FILE *f = fopen(path, "w");
    ASSERT_TRUE(f != NULL);
    ASSERT_TRUE(fputs("\nalpha\n\n\nbeta\n\n", f) != EOF);
    ASSERT_EQ(fclose(f), 0);

    History h;
    ASSERT_EQ(history_init(&h, 4), NSH_OK);
    ASSERT_EQ(history_load(&h, path), NSH_OK);
    ASSERT_EQ(history_count(&h), 2);
    ASSERT_STR_EQ(history_get(&h, 0), "alpha");
    ASSERT_STR_EQ(history_get(&h, 1), "beta");
    history_free(&h);
    remove(path);
}

TEST(load_respects_capacity) {
    char path[128];
    temp_path(path, sizeof(path), "overflow");

    History src;
    ASSERT_EQ(history_init(&src, 8), NSH_OK);
    history_add(&src, "1");
    history_add(&src, "2");
    history_add(&src, "3");
    history_add(&src, "4");
    ASSERT_EQ(history_save(&src, path), NSH_OK);
    history_free(&src);

    History h;
    ASSERT_EQ(history_init(&h, 2), NSH_OK);
    ASSERT_EQ(history_load(&h, path), NSH_OK);
    ASSERT_EQ(history_count(&h), 2);
    ASSERT_STR_EQ(history_get(&h, 0), "3");
    ASSERT_STR_EQ(history_get(&h, 1), "4");
    history_free(&h);
    remove(path);
}

TEST(free_twice_is_safe) {
    History h;
    ASSERT_EQ(history_init(&h, 4), NSH_OK);
    history_add(&h, "a");
    history_free(&h);
    history_free(&h);
    ASSERT_EQ(history_count(&h), 0);
    ASSERT_TRUE(history_get(&h, 0) == NULL);
    history_add(&h, "ignored");
    ASSERT_EQ(history_count(&h), 0);
}

TEST_MAIN()
