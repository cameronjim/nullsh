// Unit tests for the growable string, with the emphasis on the promises the
// header makes: data is always a valid C string, capacity growth never loses
// bytes, and embedded NULs survive in len.

#include "str.h"

#include <string.h>

#include "../alloc/alloc.h"
#include "../../tests/harness.h"

TEST(init_gives_a_valid_empty_string) {
    Str s;
    str_init(&s);
    ASSERT_TRUE(s.data != NULL);
    ASSERT_EQ(s.len, 0);
    ASSERT_TRUE(s.cap > 0);
    ASSERT_EQ(s.data[0], '\0');
    ASSERT_EQ(strlen(s.data), 0);
    ASSERT_STR_EQ(s.data, "");
    str_free(&s);
}

TEST(push_appends_one_byte_at_a_time) {
    Str s;
    str_init(&s);
    str_push(&s, 'a');
    str_push(&s, 'b');
    str_push(&s, 'c');
    ASSERT_EQ(s.len, 3);
    ASSERT_STR_EQ(s.data, "abc");
    str_free(&s);
}

// The interesting part is every intermediate state, not just the end: after
// each push the buffer must still read as a C string of the right length.
TEST(push_stays_correct_across_growth_boundaries) {
    Str s;
    str_init(&s);
    const size_t n = 10000;
    for (size_t i = 0; i < n; i++) {
        str_push(&s, (char)('a' + (int)(i % 26)));
        ASSERT_EQ(s.len, i + 1);
        ASSERT_EQ(s.data[s.len], '\0');
        ASSERT_TRUE(s.cap > s.len);
    }
    ASSERT_EQ(strlen(s.data), n);
    for (size_t i = 0; i < n; i++) {
        ASSERT_EQ(s.data[i], (char)('a' + (int)(i % 26)));
    }
    str_free(&s);
}

TEST(append_concatenates_c_strings) {
    Str s;
    str_init(&s);
    str_append(&s, "hello");
    str_append(&s, ", ");
    str_append(&s, "world");
    ASSERT_EQ(s.len, 12);
    ASSERT_STR_EQ(s.data, "hello, world");
    str_free(&s);
}

TEST(append_grows_past_the_initial_capacity) {
    Str s;
    str_init(&s);
    const char *chunk = "0123456789abcdef";
    const size_t chunk_len = strlen(chunk);
    const size_t rounds = 1000;
    for (size_t i = 0; i < rounds; i++) {
        str_append(&s, chunk);
        ASSERT_EQ(s.len, (i + 1) * chunk_len);
        ASSERT_EQ(s.data[s.len], '\0');
    }
    ASSERT_EQ(strlen(s.data), rounds * chunk_len);
    for (size_t i = 0; i < rounds; i++) {
        ASSERT_EQ(memcmp(s.data + i * chunk_len, chunk, chunk_len), 0);
    }
    str_free(&s);
}

TEST(appending_an_empty_string_changes_nothing) {
    Str s;
    str_init(&s);
    str_append(&s, "abc");
    size_t cap_before = s.cap;
    str_append(&s, "");
    str_append_n(&s, "ignored", 0);
    str_append_n(&s, NULL, 0);
    ASSERT_EQ(s.len, 3);
    ASSERT_EQ(s.cap, cap_before);
    ASSERT_STR_EQ(s.data, "abc");
    str_free(&s);
}

TEST(append_to_an_empty_string_works) {
    Str s;
    str_init(&s);
    str_append(&s, "");
    ASSERT_EQ(s.len, 0);
    ASSERT_STR_EQ(s.data, "");
    str_append(&s, "x");
    ASSERT_STR_EQ(s.data, "x");
    str_free(&s);
}

TEST(append_n_copies_exactly_n_bytes) {
    Str s;
    str_init(&s);
    const char *src = "abcdefgh";
    str_append_n(&s, src, 3);
    ASSERT_EQ(s.len, 3);
    ASSERT_STR_EQ(s.data, "abc");
    str_append_n(&s, src + 5, 3);
    ASSERT_EQ(s.len, 6);
    ASSERT_STR_EQ(s.data, "abcfgh");
    str_free(&s);
}

// p is not required to be NUL terminated, so a slice of a larger buffer with
// no terminator of its own must copy cleanly.
TEST(append_n_accepts_unterminated_input) {
    Str s;
    str_init(&s);
    char raw[4] = {'w', 'x', 'y', 'z'};
    str_append_n(&s, raw, sizeof(raw));
    ASSERT_EQ(s.len, 4);
    ASSERT_STR_EQ(s.data, "wxyz");
    str_free(&s);
}

// An embedded NUL counts toward len even though the C-string view stops there.
TEST(append_n_preserves_embedded_nul_bytes) {
    Str s;
    str_init(&s);
    const char raw[] = {'a', '\0', 'b', '\0', 'c'};
    str_append_n(&s, raw, sizeof(raw));
    ASSERT_EQ(s.len, 5);
    ASSERT_EQ(memcmp(s.data, raw, sizeof(raw)), 0);
    ASSERT_EQ(s.data[5], '\0');
    ASSERT_EQ(strlen(s.data), 1);
    str_push(&s, 'd');
    ASSERT_EQ(s.len, 6);
    ASSERT_EQ(s.data[5], 'd');
    ASSERT_EQ(s.data[6], '\0');
    str_free(&s);
}

TEST(embedded_nuls_survive_a_growth_reallocation) {
    Str s;
    str_init(&s);
    const size_t rounds = 4000;
    for (size_t i = 0; i < rounds; i++) {
        const char pair[2] = {'\0', 'x'};
        str_append_n(&s, pair, sizeof(pair));
    }
    ASSERT_EQ(s.len, rounds * 2);
    for (size_t i = 0; i < rounds; i++) {
        ASSERT_EQ(s.data[i * 2], '\0');
        ASSERT_EQ(s.data[i * 2 + 1], 'x');
    }
    str_free(&s);
}

TEST(take_returns_the_content_and_resets_the_string) {
    Str s;
    str_init(&s);
    str_append(&s, "owned");
    char *out = str_take(&s);
    ASSERT_STR_EQ(out, "owned");
    ASSERT_TRUE(out != s.data);
    ASSERT_EQ(s.len, 0);
    ASSERT_TRUE(s.data != NULL);
    ASSERT_EQ(s.data[0], '\0');
    nsh_free(out);
    // The reset Str is a normal empty string, ready for reuse.
    str_append(&s, "again");
    ASSERT_STR_EQ(s.data, "again");
    str_free(&s);
}

TEST(take_from_an_empty_string_returns_an_empty_c_string) {
    Str s;
    str_init(&s);
    char *out = str_take(&s);
    ASSERT_TRUE(out != NULL);
    ASSERT_STR_EQ(out, "");
    nsh_free(out);
    str_free(&s);
}

TEST(take_survives_a_grown_buffer) {
    Str s;
    str_init(&s);
    const size_t n = 10000;
    for (size_t i = 0; i < n; i++) {
        str_push(&s, 'q');
    }
    char *out = str_take(&s);
    ASSERT_EQ(strlen(out), n);
    for (size_t i = 0; i < n; i++) {
        ASSERT_EQ(out[i], 'q');
    }
    nsh_free(out);
    str_free(&s);
}

TEST(clear_empties_the_string_and_keeps_the_capacity) {
    Str s;
    str_init(&s);
    str_append(&s, "some reasonably long content here");
    size_t cap_before = s.cap;
    str_clear(&s);
    ASSERT_EQ(s.len, 0);
    ASSERT_EQ(s.cap, cap_before);
    ASSERT_TRUE(s.data != NULL);
    ASSERT_STR_EQ(s.data, "");
    str_append(&s, "reused");
    ASSERT_STR_EQ(s.data, "reused");
    ASSERT_EQ(s.cap, cap_before);
    str_free(&s);
}

TEST(clear_on_an_already_empty_string_is_a_no_op) {
    Str s;
    str_init(&s);
    str_clear(&s);
    str_clear(&s);
    ASSERT_EQ(s.len, 0);
    ASSERT_STR_EQ(s.data, "");
    str_free(&s);
}

TEST(free_twice_is_safe) {
    Str s;
    str_init(&s);
    str_append(&s, "doomed");
    str_free(&s);
    ASSERT_TRUE(s.data == NULL);
    ASSERT_EQ(s.len, 0);
    ASSERT_EQ(s.cap, 0);
    str_free(&s);
    ASSERT_TRUE(s.data == NULL);
    ASSERT_EQ(s.cap, 0);
}

TEST(free_then_init_reuses_the_struct) {
    Str s;
    str_init(&s);
    str_append(&s, "first");
    str_free(&s);
    str_init(&s);
    ASSERT_EQ(s.len, 0);
    ASSERT_STR_EQ(s.data, "");
    str_append(&s, "second");
    ASSERT_STR_EQ(s.data, "second");
    str_free(&s);
}

TEST(mixed_push_and_append_agree_on_length) {
    Str s;
    str_init(&s);
    for (size_t i = 0; i < 500; i++) {
        str_push(&s, '<');
        str_append(&s, "mid");
        str_append_n(&s, ">>ignored", 2);
    }
    ASSERT_EQ(s.len, 500 * 6);
    ASSERT_EQ(strlen(s.data), 500 * 6);
    ASSERT_EQ(memcmp(s.data, "<mid>><mid>>", 12), 0);
    str_free(&s);
}

TEST_MAIN()
