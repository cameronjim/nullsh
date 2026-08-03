// Unit tests for the line reader. Input comes from tmpfile(), which is plain
// ISO C, so nothing here needs a feature test macro.

#include "line.h"

#include <stdio.h>
#include <string.h>

#include "../alloc/alloc.h"
#include "../../tests/harness.h"

// Writes content into an anonymous temp file and rewinds it. Returns NULL only
// if the platform refuses to give us a temp file at all.
static FILE *stream_of(const char *content, size_t n) {
    FILE *f = tmpfile();
    if (f == NULL) {
        return NULL;
    }
    if (n > 0 && fwrite(content, 1, n, f) != n) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    return f;
}

static FILE *stream_of_cstr(const char *content) {
    return stream_of(content, strlen(content));
}

TEST(reads_a_single_line_and_strips_the_newline) {
    FILE *f = stream_of_cstr("echo hi\n");
    ASSERT_TRUE(f != NULL);
    Str s;
    str_init(&s);
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_STR_EQ(s.data, "echo hi");
    ASSERT_EQ(s.len, 7);
    ASSERT_EQ(line_read(f, &s), NSH_EOF);
    str_free(&s);
    fclose(f);
}

TEST(reads_several_lines_in_order) {
    FILE *f = stream_of_cstr("one\ntwo\nthree\n");
    ASSERT_TRUE(f != NULL);
    Str s;
    str_init(&s);
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_STR_EQ(s.data, "one");
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_STR_EQ(s.data, "two");
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_STR_EQ(s.data, "three");
    ASSERT_EQ(line_read(f, &s), NSH_EOF);
    str_free(&s);
    fclose(f);
}

// A bare newline is a real line, not end of input. The REPL relies on this to
// tell "user pressed enter" from "user pressed Ctrl-D".
TEST(an_empty_line_reads_as_ok_with_an_empty_string) {
    FILE *f = stream_of_cstr("\n");
    ASSERT_TRUE(f != NULL);
    Str s;
    str_init(&s);
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_EQ(s.len, 0);
    ASSERT_TRUE(s.data != NULL);
    ASSERT_STR_EQ(s.data, "");
    ASSERT_EQ(line_read(f, &s), NSH_EOF);
    str_free(&s);
    fclose(f);
}

TEST(blank_lines_between_content_are_preserved) {
    FILE *f = stream_of_cstr("a\n\n\nb\n");
    ASSERT_TRUE(f != NULL);
    Str s;
    str_init(&s);
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_STR_EQ(s.data, "a");
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_STR_EQ(s.data, "");
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_STR_EQ(s.data, "");
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_STR_EQ(s.data, "b");
    ASSERT_EQ(line_read(f, &s), NSH_EOF);
    str_free(&s);
    fclose(f);
}

TEST(a_last_line_without_a_newline_still_reads_ok) {
    FILE *f = stream_of_cstr("first\nlast");
    ASSERT_TRUE(f != NULL);
    Str s;
    str_init(&s);
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_STR_EQ(s.data, "first");
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_STR_EQ(s.data, "last");
    ASSERT_EQ(line_read(f, &s), NSH_EOF);
    str_free(&s);
    fclose(f);
}

TEST(an_empty_file_is_eof_immediately) {
    FILE *f = stream_of("", 0);
    ASSERT_TRUE(f != NULL);
    Str s;
    str_init(&s);
    ASSERT_EQ(line_read(f, &s), NSH_EOF);
    ASSERT_EQ(s.len, 0);
    ASSERT_STR_EQ(s.data, "");
    str_free(&s);
    fclose(f);
}

TEST(eof_keeps_returning_eof) {
    FILE *f = stream_of_cstr("only\n");
    ASSERT_TRUE(f != NULL);
    Str s;
    str_init(&s);
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(line_read(f, &s), NSH_EOF);
        ASSERT_EQ(s.len, 0);
    }
    str_free(&s);
    fclose(f);
}

// out is cleared before anything is read, so leftovers from the previous line
// can never leak into the next one, not even on EOF.
TEST(out_is_cleared_before_reading) {
    FILE *f = stream_of_cstr("fresh\n");
    ASSERT_TRUE(f != NULL);
    Str s;
    str_init(&s);
    str_append(&s, "stale content that must disappear");
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_STR_EQ(s.data, "fresh");
    str_append(&s, "stale again");
    ASSERT_EQ(line_read(f, &s), NSH_EOF);
    ASSERT_EQ(s.len, 0);
    ASSERT_STR_EQ(s.data, "");
    str_free(&s);
    fclose(f);
}

TEST(a_long_line_arrives_intact) {
    const size_t n = 10000;
    char *buf = nsh_malloc(n + 2);
    for (size_t i = 0; i < n; i++) {
        buf[i] = (char)('A' + (int)(i % 26));
    }
    buf[n] = '\n';
    buf[n + 1] = '\0';

    FILE *f = stream_of(buf, n + 1);
    ASSERT_TRUE(f != NULL);
    Str s;
    str_init(&s);
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_EQ(s.len, n);
    ASSERT_EQ(strlen(s.data), n);
    ASSERT_EQ(memcmp(s.data, buf, n), 0);
    ASSERT_EQ(line_read(f, &s), NSH_EOF);
    str_free(&s);
    fclose(f);
    nsh_free(buf);
}

TEST(a_long_line_without_a_trailing_newline_arrives_intact) {
    const size_t n = 10000;
    char *buf = nsh_malloc(n + 1);
    memset(buf, 'z', n);
    buf[n] = '\0';

    FILE *f = stream_of(buf, n);
    ASSERT_TRUE(f != NULL);
    Str s;
    str_init(&s);
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_EQ(s.len, n);
    ASSERT_EQ(memcmp(s.data, buf, n), 0);
    ASSERT_EQ(line_read(f, &s), NSH_EOF);
    str_free(&s);
    fclose(f);
    nsh_free(buf);
}

// Reading byte by byte means a NUL inside a line is data, not a terminator.
TEST(a_nul_byte_inside_a_line_is_kept) {
    const char raw[] = {'a', '\0', 'b', '\n'};
    FILE *f = stream_of(raw, sizeof(raw));
    ASSERT_TRUE(f != NULL);
    Str s;
    str_init(&s);
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_EQ(s.len, 3);
    ASSERT_EQ(memcmp(s.data, raw, 3), 0);
    ASSERT_EQ(s.data[3], '\0');
    ASSERT_EQ(line_read(f, &s), NSH_EOF);
    str_free(&s);
    fclose(f);
}

TEST(high_bytes_survive_unchanged) {
    const char raw[] = {(char)0x80, (char)0xFF, (char)0xC3, (char)0xA9, '\n'};
    FILE *f = stream_of(raw, sizeof(raw));
    ASSERT_TRUE(f != NULL);
    Str s;
    str_init(&s);
    ASSERT_EQ(line_read(f, &s), NSH_OK);
    ASSERT_EQ(s.len, 4);
    ASSERT_EQ(memcmp(s.data, raw, 4), 0);
    str_free(&s);
    fclose(f);
}

// A directory opens fine on Linux but every read fails, which is the simplest
// way to reach the ferror path without mocking stdio.
TEST(a_read_error_reports_io) {
    FILE *f = fopen(".", "r");
    if (f == NULL) {
        // Some platforms refuse the open outright. Nothing to check here.
        ASSERT_TRUE(1);
        return;
    }
    Str s;
    str_init(&s);
    NshError e = line_read(f, &s);
    ASSERT_EQ(e, NSH_ERR_IO);
    str_free(&s);
    fclose(f);
}

TEST_MAIN()
