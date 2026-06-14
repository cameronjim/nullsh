// Tests for the pure half of line editing: the buffer and the key decoder.

#include "edit.h"

#include <string.h>

#include "../../tests/harness.h"

// The buffer after a fresh set, which is how every buffer case starts.
static void seed(EditLine *e, const char *text, size_t cursor) {
    edit_set(e, text);
    e->cursor = cursor;
}

static void feed_str(EditKeys *k, const char *bytes, size_t n,
                     EditKey *keys, char *chars) {
    for (size_t i = 0; i < n; i++) {
        char ch = 0;
        keys[i] = editkeys_feed(k, (unsigned char)bytes[i], &ch);
        chars[i] = ch;
    }
}

// Feeds a whole sequence and returns the action of its final byte, checking
// that every earlier byte produced EK_NONE.
static EditKey feed_seq(const char *bytes, int *nsh_failed) {
    EditKeys k;
    editkeys_init(&k);
    size_t n = strlen(bytes);
    EditKey last = EK_NONE;
    for (size_t i = 0; i < n; i++) {
        char ch = 0;
        last = editkeys_feed(&k, (unsigned char)bytes[i], &ch);
        if (i + 1 < n && last != EK_NONE) {
            printf("    byte %zu of a sequence acted early\n", i);
            *nsh_failed = 1;
            return EK_NONE;
        }
    }
    return last;
}

TEST(a_fresh_line_is_empty) {
    EditLine e;
    edit_init(&e);
    ASSERT_STR_EQ(e.buf.data, "");
    ASSERT_EQ(e.buf.len, 0);
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(free_is_safe_twice) {
    EditLine e;
    edit_init(&e);
    edit_free(&e);
    edit_free(&e);
    ASSERT_EQ(e.cursor, 0);
}

TEST(insert_appends_at_the_end) {
    EditLine e;
    edit_init(&e);
    edit_insert(&e, 'a');
    edit_insert(&e, 'b');
    edit_insert(&e, 'c');
    ASSERT_STR_EQ(e.buf.data, "abc");
    ASSERT_EQ(e.buf.len, 3);
    ASSERT_EQ(e.cursor, 3);
    edit_free(&e);
}

TEST(insert_in_the_middle_splices) {
    EditLine e;
    edit_init(&e);
    seed(&e, "ac", 1);
    edit_insert(&e, 'b');
    ASSERT_STR_EQ(e.buf.data, "abc");
    ASSERT_EQ(e.cursor, 2);
    edit_free(&e);
}

TEST(insert_at_position_zero_prepends) {
    EditLine e;
    edit_init(&e);
    seed(&e, "bc", 0);
    edit_insert(&e, 'a');
    ASSERT_STR_EQ(e.buf.data, "abc");
    ASSERT_EQ(e.cursor, 1);
    edit_free(&e);
}

// Enough characters to force the underlying Str past its initial capacity.
TEST(insert_grows_past_the_initial_capacity) {
    EditLine e;
    edit_init(&e);
    for (int i = 0; i < 200; i++) {
        edit_insert(&e, 'x');
    }
    ASSERT_EQ(e.buf.len, 200);
    ASSERT_EQ(e.cursor, 200);
    ASSERT_EQ(strlen(e.buf.data), 200);
    // Splicing into a grown buffer still moves the whole tail.
    e.cursor = 0;
    edit_insert(&e, 'y');
    ASSERT_EQ(e.buf.data[0], 'y');
    ASSERT_EQ(e.buf.data[1], 'x');
    ASSERT_EQ(e.buf.len, 201);
    edit_free(&e);
}

TEST(backspace_at_the_start_does_nothing) {
    EditLine e;
    edit_init(&e);
    seed(&e, "abc", 0);
    edit_backspace(&e);
    ASSERT_STR_EQ(e.buf.data, "abc");
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(backspace_on_an_empty_buffer_does_nothing) {
    EditLine e;
    edit_init(&e);
    edit_backspace(&e);
    ASSERT_STR_EQ(e.buf.data, "");
    ASSERT_EQ(e.buf.len, 0);
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(backspace_removes_the_character_left_of_the_cursor) {
    EditLine e;
    edit_init(&e);
    seed(&e, "abc", 3);
    edit_backspace(&e);
    ASSERT_STR_EQ(e.buf.data, "ab");
    ASSERT_EQ(e.cursor, 2);
    seed(&e, "abc", 1);
    edit_backspace(&e);
    ASSERT_STR_EQ(e.buf.data, "bc");
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(delete_removes_the_character_under_the_cursor) {
    EditLine e;
    edit_init(&e);
    seed(&e, "abc", 0);
    edit_delete(&e);
    ASSERT_STR_EQ(e.buf.data, "bc");
    ASSERT_EQ(e.cursor, 0);
    seed(&e, "abc", 1);
    edit_delete(&e);
    ASSERT_STR_EQ(e.buf.data, "ac");
    ASSERT_EQ(e.cursor, 1);
    edit_free(&e);
}

TEST(delete_at_the_end_does_nothing) {
    EditLine e;
    edit_init(&e);
    seed(&e, "abc", 3);
    edit_delete(&e);
    ASSERT_STR_EQ(e.buf.data, "abc");
    ASSERT_EQ(e.cursor, 3);
    edit_free(&e);
}

TEST(delete_on_an_empty_buffer_does_nothing) {
    EditLine e;
    edit_init(&e);
    edit_delete(&e);
    ASSERT_STR_EQ(e.buf.data, "");
    ASSERT_EQ(e.buf.len, 0);
    edit_free(&e);
}

TEST(cursor_motion_clamps_at_both_ends) {
    EditLine e;
    edit_init(&e);
    seed(&e, "ab", 0);
    edit_left(&e);
    ASSERT_EQ(e.cursor, 0);
    edit_right(&e);
    ASSERT_EQ(e.cursor, 1);
    edit_right(&e);
    ASSERT_EQ(e.cursor, 2);
    edit_right(&e);
    ASSERT_EQ(e.cursor, 2);
    edit_left(&e);
    ASSERT_EQ(e.cursor, 1);
    edit_free(&e);
}

TEST(cursor_motion_on_an_empty_buffer_stays_put) {
    EditLine e;
    edit_init(&e);
    edit_left(&e);
    edit_right(&e);
    edit_home(&e);
    edit_end(&e);
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(home_and_end_jump_to_the_edges) {
    EditLine e;
    edit_init(&e);
    seed(&e, "hello", 2);
    edit_home(&e);
    ASSERT_EQ(e.cursor, 0);
    edit_end(&e);
    ASSERT_EQ(e.cursor, 5);
    edit_free(&e);
}

TEST(kill_to_end_truncates_at_the_cursor) {
    EditLine e;
    edit_init(&e);
    seed(&e, "hello world", 5);
    edit_kill_to_end(&e);
    ASSERT_STR_EQ(e.buf.data, "hello");
    ASSERT_EQ(e.buf.len, 5);
    ASSERT_EQ(e.cursor, 5);
    edit_free(&e);
}

TEST(kill_to_end_at_the_start_empties_the_line) {
    EditLine e;
    edit_init(&e);
    seed(&e, "hello", 0);
    edit_kill_to_end(&e);
    ASSERT_STR_EQ(e.buf.data, "");
    ASSERT_EQ(e.buf.len, 0);
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(kill_to_end_at_the_end_is_a_no_op) {
    EditLine e;
    edit_init(&e);
    seed(&e, "hello", 5);
    edit_kill_to_end(&e);
    ASSERT_STR_EQ(e.buf.data, "hello");
    edit_kill_to_end(&e);
    ASSERT_STR_EQ(e.buf.data, "hello");
    edit_free(&e);
}

TEST(kill_to_start_drops_everything_before_the_cursor) {
    EditLine e;
    edit_init(&e);
    seed(&e, "hello world", 6);
    edit_kill_to_start(&e);
    ASSERT_STR_EQ(e.buf.data, "world");
    ASSERT_EQ(e.buf.len, 5);
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(kill_to_start_at_position_zero_is_a_no_op) {
    EditLine e;
    edit_init(&e);
    seed(&e, "hello", 0);
    edit_kill_to_start(&e);
    ASSERT_STR_EQ(e.buf.data, "hello");
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(kill_to_start_at_the_end_empties_the_line) {
    EditLine e;
    edit_init(&e);
    seed(&e, "hello", 5);
    edit_kill_to_start(&e);
    ASSERT_STR_EQ(e.buf.data, "");
    ASSERT_EQ(e.buf.len, 0);
    edit_free(&e);
}

TEST(kill_word_removes_the_word_left_of_the_cursor) {
    EditLine e;
    edit_init(&e);
    seed(&e, "echo hello", 10);
    edit_kill_word_left(&e);
    ASSERT_STR_EQ(e.buf.data, "echo ");
    ASSERT_EQ(e.cursor, 5);
    edit_kill_word_left(&e);
    ASSERT_STR_EQ(e.buf.data, "");
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(kill_word_crosses_a_run_of_spaces) {
    EditLine e;
    edit_init(&e);
    seed(&e, "one    two", 10);
    edit_kill_word_left(&e);
    ASSERT_STR_EQ(e.buf.data, "one    ");
    edit_kill_word_left(&e);
    ASSERT_STR_EQ(e.buf.data, "");
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(kill_word_with_trailing_spaces_takes_the_word_too) {
    EditLine e;
    edit_init(&e);
    seed(&e, "one two   ", 10);
    edit_kill_word_left(&e);
    ASSERT_STR_EQ(e.buf.data, "one ");
    ASSERT_EQ(e.cursor, 4);
    edit_free(&e);
}

TEST(kill_word_at_position_zero_is_a_no_op) {
    EditLine e;
    edit_init(&e);
    seed(&e, "one two", 0);
    edit_kill_word_left(&e);
    ASSERT_STR_EQ(e.buf.data, "one two");
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(kill_word_on_an_empty_buffer_is_a_no_op) {
    EditLine e;
    edit_init(&e);
    edit_kill_word_left(&e);
    ASSERT_STR_EQ(e.buf.data, "");
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(kill_word_only_touches_the_left_of_the_cursor) {
    EditLine e;
    edit_init(&e);
    seed(&e, "alpha beta gamma", 10);
    edit_kill_word_left(&e);
    ASSERT_STR_EQ(e.buf.data, "alpha  gamma");
    ASSERT_EQ(e.cursor, 6);
    edit_free(&e);
}

TEST(kill_word_of_only_spaces_clears_them) {
    EditLine e;
    edit_init(&e);
    seed(&e, "   ", 3);
    edit_kill_word_left(&e);
    ASSERT_STR_EQ(e.buf.data, "");
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

TEST(kill_word_treats_a_tab_as_a_separator) {
    EditLine e;
    edit_init(&e);
    seed(&e, "one\ttwo", 7);
    edit_kill_word_left(&e);
    ASSERT_STR_EQ(e.buf.data, "one\t");
    edit_free(&e);
}

TEST(set_replaces_a_longer_line_and_parks_the_cursor) {
    EditLine e;
    edit_init(&e);
    seed(&e, "a very long previous line", 25);
    edit_set(&e, "short");
    ASSERT_STR_EQ(e.buf.data, "short");
    ASSERT_EQ(e.buf.len, 5);
    ASSERT_EQ(e.cursor, 5);
    edit_free(&e);
}

TEST(set_to_null_or_empty_clears_the_line) {
    EditLine e;
    edit_init(&e);
    seed(&e, "text", 4);
    edit_set(&e, NULL);
    ASSERT_STR_EQ(e.buf.data, "");
    ASSERT_EQ(e.cursor, 0);
    edit_set(&e, "text");
    edit_set(&e, "");
    ASSERT_STR_EQ(e.buf.data, "");
    ASSERT_EQ(e.buf.len, 0);
    ASSERT_EQ(e.cursor, 0);
    edit_free(&e);
}

// The buffer survives an arbitrary mix of operations with its NUL intact.
TEST(mixed_operations_keep_the_string_terminated) {
    EditLine e;
    edit_init(&e);
    edit_insert(&e, 'l');
    edit_insert(&e, 's');
    edit_insert(&e, ' ');
    edit_insert(&e, '-');
    edit_insert(&e, 'l');
    edit_home(&e);
    edit_delete(&e);
    ASSERT_STR_EQ(e.buf.data, "s -l");
    edit_end(&e);
    edit_backspace(&e);
    ASSERT_STR_EQ(e.buf.data, "s -");
    edit_left(&e);
    edit_insert(&e, 'x');
    ASSERT_STR_EQ(e.buf.data, "s x-");
    ASSERT_EQ(strlen(e.buf.data), e.buf.len);
    edit_free(&e);
}

TEST(printable_bytes_decode_as_characters) {
    EditKeys k;
    editkeys_init(&k);
    char ch = 0;
    ASSERT_EQ(editkeys_feed(&k, 'a', &ch), EK_CHAR);
    ASSERT_EQ(ch, 'a');
    ASSERT_EQ(editkeys_feed(&k, ' ', &ch), EK_CHAR);
    ASSERT_EQ(ch, ' ');
    ASSERT_EQ(editkeys_feed(&k, '~', &ch), EK_CHAR);
    ASSERT_EQ(ch, '~');
    // A high byte is passed through so UTF-8 text still reaches the buffer.
    ASSERT_EQ(editkeys_feed(&k, 0xc3, &ch), EK_CHAR);
    ASSERT_EQ((unsigned char)ch, 0xc3);
}

TEST(a_null_char_pointer_is_accepted) {
    EditKeys k;
    editkeys_init(&k);
    ASSERT_EQ(editkeys_feed(&k, 'a', NULL), EK_CHAR);
}

TEST(control_bytes_map_to_their_actions) {
    EditKeys k;
    editkeys_init(&k);
    ASSERT_EQ(editkeys_feed(&k, 0x0d, NULL), EK_ENTER);
    ASSERT_EQ(editkeys_feed(&k, 0x0a, NULL), EK_ENTER);
    ASSERT_EQ(editkeys_feed(&k, 0x7f, NULL), EK_BACKSPACE);
    ASSERT_EQ(editkeys_feed(&k, 0x08, NULL), EK_BACKSPACE);
    ASSERT_EQ(editkeys_feed(&k, 0x01, NULL), EK_HOME);
    ASSERT_EQ(editkeys_feed(&k, 0x05, NULL), EK_END);
    ASSERT_EQ(editkeys_feed(&k, 0x0b, NULL), EK_KILL_END);
    ASSERT_EQ(editkeys_feed(&k, 0x15, NULL), EK_KILL_START);
    ASSERT_EQ(editkeys_feed(&k, 0x17, NULL), EK_KILL_WORD);
    ASSERT_EQ(editkeys_feed(&k, 0x04, NULL), EK_EOF);
    ASSERT_EQ(editkeys_feed(&k, 0x03, NULL), EK_INTERRUPT);
}

// Anything below 0x20 that is not bound is dropped, never inserted.
TEST(unbound_control_bytes_are_swallowed) {
    EditKeys k;
    editkeys_init(&k);
    const unsigned char unbound[] = {0x00, 0x02, 0x06, 0x07, 0x09, 0x0c,
                                     0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13,
                                     0x14, 0x16, 0x18, 0x19, 0x1a, 0x1c,
                                     0x1d, 0x1e, 0x1f};
    for (size_t i = 0; i < sizeof unbound / sizeof unbound[0]; i++) {
        char ch = 'Z';
        ASSERT_EQ(editkeys_feed(&k, unbound[i], &ch), EK_NONE);
        ASSERT_EQ(ch, 'Z');
    }
}

TEST(csi_arrow_sequences_decode) {
    ASSERT_EQ(feed_seq("\x1b[D", nsh_failed), EK_LEFT);
    ASSERT_EQ(feed_seq("\x1b[C", nsh_failed), EK_RIGHT);
    ASSERT_EQ(feed_seq("\x1b[A", nsh_failed), EK_UP);
    ASSERT_EQ(feed_seq("\x1b[B", nsh_failed), EK_DOWN);
}

TEST(csi_home_and_end_decode) {
    ASSERT_EQ(feed_seq("\x1b[H", nsh_failed), EK_HOME);
    ASSERT_EQ(feed_seq("\x1b[F", nsh_failed), EK_END);
}

TEST(csi_tilde_sequences_decode) {
    ASSERT_EQ(feed_seq("\x1b[1~", nsh_failed), EK_HOME);
    ASSERT_EQ(feed_seq("\x1b[4~", nsh_failed), EK_END);
    ASSERT_EQ(feed_seq("\x1b[3~", nsh_failed), EK_DELETE);
    ASSERT_EQ(feed_seq("\x1b[7~", nsh_failed), EK_HOME);
    ASSERT_EQ(feed_seq("\x1b[8~", nsh_failed), EK_END);
}

TEST(ss3_sequences_decode) {
    ASSERT_EQ(feed_seq("\x1bOH", nsh_failed), EK_HOME);
    ASSERT_EQ(feed_seq("\x1bOF", nsh_failed), EK_END);
    ASSERT_EQ(feed_seq("\x1bOA", nsh_failed), EK_UP);
    ASSERT_EQ(feed_seq("\x1bOD", nsh_failed), EK_LEFT);
}

// The three bytes of an arrow key can arrive in three separate reads.
TEST(a_split_escape_sequence_decodes_byte_by_byte) {
    EditKeys k;
    editkeys_init(&k);
    ASSERT_EQ(editkeys_feed(&k, 0x1b, NULL), EK_NONE);
    ASSERT_EQ(editkeys_feed(&k, '[', NULL), EK_NONE);
    ASSERT_EQ(editkeys_feed(&k, 'A', NULL), EK_UP);
    // The decoder is back in the ground state for the next key.
    char ch = 0;
    ASSERT_EQ(editkeys_feed(&k, 'q', &ch), EK_CHAR);
    ASSERT_EQ(ch, 'q');
    // And the four bytes of a tilde form split the same way.
    ASSERT_EQ(editkeys_feed(&k, 0x1b, NULL), EK_NONE);
    ASSERT_EQ(editkeys_feed(&k, '[', NULL), EK_NONE);
    ASSERT_EQ(editkeys_feed(&k, '3', NULL), EK_NONE);
    ASSERT_EQ(editkeys_feed(&k, '~', NULL), EK_DELETE);
}

TEST(an_unknown_csi_sequence_is_swallowed) {
    EditKeys k;
    editkeys_init(&k);
    char ch = 'Z';
    ASSERT_EQ(editkeys_feed(&k, 0x1b, &ch), EK_NONE);
    ASSERT_EQ(editkeys_feed(&k, '[', &ch), EK_NONE);
    ASSERT_EQ(editkeys_feed(&k, 'Z', &ch), EK_NONE);
    ASSERT_EQ(ch, 'Z');
    // Nothing leaked into the buffer and the next key still works.
    ASSERT_EQ(editkeys_feed(&k, 'x', &ch), EK_CHAR);
    ASSERT_EQ(ch, 'x');
}

TEST(unknown_tilde_and_ss3_forms_are_swallowed) {
    ASSERT_EQ(feed_seq("\x1b[5~", nsh_failed), EK_NONE);
    ASSERT_EQ(feed_seq("\x1b[6~", nsh_failed), EK_NONE);
    ASSERT_EQ(feed_seq("\x1b[15~", nsh_failed), EK_NONE);
    ASSERT_EQ(feed_seq("\x1bOP", nsh_failed), EK_NONE);
}

// Ctrl-arrow arrives as ESC [ 1 ; 5 C and must not read as a plain arrow.
TEST(a_modified_arrow_is_swallowed) {
    ASSERT_EQ(feed_seq("\x1b[1;5C", nsh_failed), EK_NONE);
    ASSERT_EQ(feed_seq("\x1b[1;2A", nsh_failed), EK_NONE);
    EditKeys k;
    editkeys_init(&k);
    const char *s = "\x1b[1;5C";
    for (size_t i = 0; s[i] != '\0'; i++) {
        ASSERT_EQ(editkeys_feed(&k, (unsigned char)s[i], NULL), EK_NONE);
    }
    char ch = 0;
    ASSERT_EQ(editkeys_feed(&k, 'z', &ch), EK_CHAR);
    ASSERT_EQ(ch, 'z');
}

// A parameter longer than the pending buffer must not overflow it.
TEST(an_overlong_parameter_is_swallowed) {
    ASSERT_EQ(feed_seq("\x1b[123456789012345~", nsh_failed), EK_NONE);
    EditKeys k;
    editkeys_init(&k);
    const char *s = "\x1b[123456789012345~";
    for (size_t i = 0; s[i] != '\0'; i++) {
        ASSERT_EQ(editkeys_feed(&k, (unsigned char)s[i], NULL), EK_NONE);
    }
    ASSERT_EQ(editkeys_feed(&k, 'a', NULL), EK_CHAR);
}

TEST(a_bare_escape_lets_the_next_byte_stand_alone) {
    EditKeys k;
    editkeys_init(&k);
    char ch = 0;
    ASSERT_EQ(editkeys_feed(&k, 0x1b, &ch), EK_NONE);
    ASSERT_EQ(editkeys_feed(&k, 'a', &ch), EK_CHAR);
    ASSERT_EQ(ch, 'a');
    // The same applies to a control byte after the escape.
    ASSERT_EQ(editkeys_feed(&k, 0x1b, NULL), EK_NONE);
    ASSERT_EQ(editkeys_feed(&k, 0x0d, NULL), EK_ENTER);
    // And a doubled escape just restarts the sequence.
    ASSERT_EQ(editkeys_feed(&k, 0x1b, NULL), EK_NONE);
    ASSERT_EQ(editkeys_feed(&k, 0x1b, NULL), EK_NONE);
    ASSERT_EQ(editkeys_feed(&k, '[', NULL), EK_NONE);
    ASSERT_EQ(editkeys_feed(&k, 'C', NULL), EK_RIGHT);
}

// A whole typed line: text, an arrow, a rubout, and the return key.
TEST(a_realistic_byte_stream_decodes_in_order) {
    EditKeys k;
    editkeys_init(&k);
    const char stream[] = "ab\x1b[D\x7f\x1b[3~\rc";
    size_t n = sizeof stream - 1;
    EditKey keys[32];
    char chars[32];
    feed_str(&k, stream, n, keys, chars);
    ASSERT_EQ(keys[0], EK_CHAR);
    ASSERT_EQ(chars[0], 'a');
    ASSERT_EQ(keys[1], EK_CHAR);
    ASSERT_EQ(chars[1], 'b');
    ASSERT_EQ(keys[2], EK_NONE);
    ASSERT_EQ(keys[3], EK_NONE);
    ASSERT_EQ(keys[4], EK_LEFT);
    ASSERT_EQ(keys[5], EK_BACKSPACE);
    ASSERT_EQ(keys[8], EK_NONE);
    ASSERT_EQ(keys[9], EK_DELETE);
    ASSERT_EQ(keys[10], EK_ENTER);
    ASSERT_EQ(keys[11], EK_CHAR);
    ASSERT_EQ(chars[11], 'c');
}

TEST_MAIN()
