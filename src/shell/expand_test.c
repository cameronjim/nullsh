// Unit tests for variable expansion. Tokens are built by hand here because the
// lexer is a separate module; each test owns its segments and its environment
// variables and cleans both up. Every variable used is NSH_TEST_ prefixed so a
// stray one can never collide with the real environment.

#define _POSIX_C_SOURCE 200809L

#include "expand.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../alloc/alloc.h"
#include "../util/vec.h"
#include "../../tests/harness.h"

static char *test_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = nsh_malloc(n);
    memcpy(p, s, n);
    return p;
}

static void word_init(Token *t) {
    t->kind = TOK_WORD;
    vec_init(&t->segs);
}

static void word_push(Token *t, const char *text, bool expand) {
    WordSeg *seg = nsh_malloc(sizeof(*seg));
    seg->text = test_strdup(text);
    seg->expand = expand;
    vec_push(&t->segs, seg);
}

static void word_free(Token *t) {
    for (size_t i = 0; i < t->segs.len; i++) {
        WordSeg *seg = t->segs.items[i];
        nsh_free(seg->text);
        nsh_free(seg);
    }
    vec_free(&t->segs);
}

// The common shape: one segment, expand it, hand back both code and result.
static NshError expand_one(const char *text, bool expand, int last_status,
                           char **out) {
    Token t;
    word_init(&t);
    word_push(&t, text, expand);
    NshError err = expand_word(&t, last_status, out);
    word_free(&t);
    return err;
}

TEST(plain_word_without_dollars_is_copied) {
    char *out = NULL;
    ASSERT_EQ(expand_one("hello", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "hello");
    nsh_free(out);
}

TEST(empty_segment_gives_empty_string) {
    char *out = NULL;
    ASSERT_EQ(expand_one("", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "");
    nsh_free(out);
}

TEST(word_with_no_segments_gives_empty_string) {
    Token t;
    word_init(&t);
    char *out = NULL;
    ASSERT_EQ(expand_word(&t, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "");
    nsh_free(out);
    word_free(&t);
}

TEST(set_variable_expands_to_its_value) {
    setenv("NSH_TEST_A", "value", 1);
    char *out = NULL;
    ASSERT_EQ(expand_one("$NSH_TEST_A", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "value");
    nsh_free(out);
    unsetenv("NSH_TEST_A");
}

TEST(unset_variable_expands_to_nothing) {
    unsetenv("NSH_TEST_MISSING");
    char *out = NULL;
    ASSERT_EQ(expand_one("$NSH_TEST_MISSING", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "");
    nsh_free(out);
    ASSERT_EQ(expand_one("[$NSH_TEST_MISSING]", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "[]");
    nsh_free(out);
}

TEST(variable_set_to_the_empty_string_expands_to_nothing) {
    setenv("NSH_TEST_EMPTY", "", 1);
    char *out = NULL;
    ASSERT_EQ(expand_one("<$NSH_TEST_EMPTY>", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "<>");
    nsh_free(out);
    unsetenv("NSH_TEST_EMPTY");
}

TEST(brace_form_expands_like_the_bare_form) {
    setenv("NSH_TEST_A", "value", 1);
    char *out = NULL;
    ASSERT_EQ(expand_one("${NSH_TEST_A}", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "value");
    nsh_free(out);
    unsetenv("NSH_TEST_A");
}

TEST(brace_form_of_an_unset_variable_expands_to_nothing) {
    unsetenv("NSH_TEST_MISSING");
    char *out = NULL;
    ASSERT_EQ(expand_one("a${NSH_TEST_MISSING}b", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "ab");
    nsh_free(out);
}

// The braces exist so the name can end where a bare name would keep running.
TEST(braces_splice_a_value_between_name_characters) {
    setenv("NSH_TEST_A", "mid", 1);
    char *out = NULL;
    ASSERT_EQ(expand_one("x${NSH_TEST_A}y", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "xmidy");
    nsh_free(out);
    ASSERT_EQ(expand_one("${NSH_TEST_A}${NSH_TEST_A}", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "midmid");
    nsh_free(out);
    unsetenv("NSH_TEST_A");
}

TEST(status_expands_as_decimal) {
    char *out = NULL;
    ASSERT_EQ(expand_one("$?", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "0");
    nsh_free(out);
    ASSERT_EQ(expand_one("$?", true, 1, &out), NSH_OK);
    ASSERT_STR_EQ(out, "1");
    nsh_free(out);
    ASSERT_EQ(expand_one("$?", true, 127, &out), NSH_OK);
    ASSERT_STR_EQ(out, "127");
    nsh_free(out);
    ASSERT_EQ(expand_one("status=$? ok", true, 130, &out), NSH_OK);
    ASSERT_STR_EQ(out, "status=130 ok");
    nsh_free(out);
}

TEST(status_can_repeat_in_one_segment) {
    char *out = NULL;
    ASSERT_EQ(expand_one("$?$?$?", true, 42, &out), NSH_OK);
    ASSERT_STR_EQ(out, "424242");
    nsh_free(out);
}

// $?x is the status followed by a literal x; ? never absorbs what follows.
TEST(status_does_not_swallow_following_text) {
    char *out = NULL;
    ASSERT_EQ(expand_one("$?abc", true, 7, &out), NSH_OK);
    ASSERT_STR_EQ(out, "7abc");
    nsh_free(out);
}

TEST(adjacent_variables_concatenate) {
    setenv("NSH_TEST_A", "one", 1);
    setenv("NSH_TEST_B", "two", 1);
    char *out = NULL;
    ASSERT_EQ(expand_one("$NSH_TEST_A$NSH_TEST_B", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "onetwo");
    nsh_free(out);
    ASSERT_EQ(expand_one("$NSH_TEST_A${NSH_TEST_B}$?", true, 3, &out), NSH_OK);
    ASSERT_STR_EQ(out, "onetwo3");
    nsh_free(out);
    unsetenv("NSH_TEST_A");
    unsetenv("NSH_TEST_B");
}

TEST(variable_expands_at_start_middle_and_end) {
    setenv("NSH_TEST_A", "V", 1);
    char *out = NULL;
    ASSERT_EQ(expand_one("$NSH_TEST_A tail", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "V tail");
    nsh_free(out);
    ASSERT_EQ(expand_one("head $NSH_TEST_A tail", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "head V tail");
    nsh_free(out);
    ASSERT_EQ(expand_one("head $NSH_TEST_A", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "head V");
    nsh_free(out);
    unsetenv("NSH_TEST_A");
}

// A bare name runs as far as the name charset allows and stops there.
TEST(bare_name_stops_at_the_first_non_name_character) {
    setenv("NSH_TEST_A", "V", 1);
    setenv("NSH_TEST_A2", "W", 1);
    char *out = NULL;
    ASSERT_EQ(expand_one("$NSH_TEST_A-$NSH_TEST_A.", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "V-V.");
    nsh_free(out);
    // Longest match wins: NSH_TEST_A2, not NSH_TEST_A followed by "2".
    ASSERT_EQ(expand_one("$NSH_TEST_A2", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "W");
    nsh_free(out);
    unsetenv("NSH_TEST_A");
    unsetenv("NSH_TEST_A2");
}

TEST(underscore_only_name_is_a_valid_name) {
    setenv("NSH_TEST_A", "V", 1);
    char *out = NULL;
    // A leading underscore is a name start, so _NSH_TEST_A is one unset name.
    ASSERT_EQ(expand_one("$_NSH_TEST_A", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "");
    nsh_free(out);
    unsetenv("NSH_TEST_A");
}

TEST(lone_dollar_is_literal) {
    char *out = NULL;
    ASSERT_EQ(expand_one("$", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "$");
    nsh_free(out);
    ASSERT_EQ(expand_one("a$", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "a$");
    nsh_free(out);
    ASSERT_EQ(expand_one("$$", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "$$");
    nsh_free(out);
}

// Positional parameters are not supported, so $5 is four characters of text.
TEST(dollar_digit_is_literal) {
    char *out = NULL;
    ASSERT_EQ(expand_one("$5", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "$5");
    nsh_free(out);
    ASSERT_EQ(expand_one("$0 $1 $9", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "$0 $1 $9");
    nsh_free(out);
}

TEST(dollar_punctuation_is_literal) {
    char *out = NULL;
    ASSERT_EQ(expand_one("$-", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "$-");
    nsh_free(out);
    ASSERT_EQ(expand_one("$ x", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "$ x");
    nsh_free(out);
    ASSERT_EQ(expand_one("cost: $9.99, $@ and $!", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "cost: $9.99, $@ and $!");
    nsh_free(out);
}

TEST(non_expandable_segment_keeps_dollars_verbatim) {
    setenv("NSH_TEST_A", "value", 1);
    char *out = NULL;
    ASSERT_EQ(expand_one("$NSH_TEST_A", false, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "$NSH_TEST_A");
    nsh_free(out);
    ASSERT_EQ(expand_one("${NSH_TEST_A} $? ${", false, 5, &out), NSH_OK);
    ASSERT_STR_EQ(out, "${NSH_TEST_A} $? ${");
    nsh_free(out);
    unsetenv("NSH_TEST_A");
}

// nullsh does no word splitting: a value with spaces stays one argv word.
TEST(value_with_spaces_stays_one_string) {
    setenv("NSH_TEST_A", "a b   c", 1);
    char *out = NULL;
    ASSERT_EQ(expand_one("$NSH_TEST_A", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "a b   c");
    nsh_free(out);
    setenv("NSH_TEST_A", " \t leading and trailing \n", 1);
    ASSERT_EQ(expand_one("[$NSH_TEST_A]", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "[ \t leading and trailing \n]");
    nsh_free(out);
    unsetenv("NSH_TEST_A");
}

// Substituted text is data, not source: it is never scanned for more dollars.
TEST(expanded_value_is_not_rescanned) {
    setenv("NSH_TEST_A", "$NSH_TEST_B", 1);
    setenv("NSH_TEST_B", "inner", 1);
    char *out = NULL;
    ASSERT_EQ(expand_one("$NSH_TEST_A", true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "$NSH_TEST_B");
    nsh_free(out);
    setenv("NSH_TEST_A", "${NSH_TEST_B} $? ${ unterminated", 1);
    ASSERT_EQ(expand_one("$NSH_TEST_A", true, 9, &out), NSH_OK);
    ASSERT_STR_EQ(out, "${NSH_TEST_B} $? ${ unterminated");
    nsh_free(out);
    unsetenv("NSH_TEST_A");
    unsetenv("NSH_TEST_B");
}

TEST(unterminated_brace_is_a_syntax_error) {
    setenv("NSH_TEST_A", "value", 1);
    char *out = NULL;
    ASSERT_EQ(expand_one("${NSH_TEST_A", true, 0, &out), NSH_ERR_SYNTAX);
    ASSERT_TRUE(out == NULL);
    ASSERT_EQ(expand_one("${", true, 0, &out), NSH_ERR_SYNTAX);
    ASSERT_TRUE(out == NULL);
    ASSERT_EQ(expand_one("head ${NSH_TEST_A tail", true, 0, &out),
              NSH_ERR_SYNTAX);
    ASSERT_TRUE(out == NULL);
    unsetenv("NSH_TEST_A");
}

TEST(empty_braces_are_a_syntax_error) {
    char *out = NULL;
    ASSERT_EQ(expand_one("${}", true, 0, &out), NSH_ERR_SYNTAX);
    ASSERT_TRUE(out == NULL);
    ASSERT_EQ(expand_one("a${}b", true, 0, &out), NSH_ERR_SYNTAX);
    ASSERT_TRUE(out == NULL);
}

// A brace opened in one segment cannot be closed by the next one.
TEST(brace_form_may_not_span_segments) {
    setenv("NSH_TEST_A", "value", 1);
    Token t;
    word_init(&t);
    word_push(&t, "${NSH_TEST", true);
    word_push(&t, "_A}", true);
    char *out = NULL;
    ASSERT_EQ(expand_word(&t, 0, &out), NSH_ERR_SYNTAX);
    ASSERT_TRUE(out == NULL);
    word_free(&t);
    unsetenv("NSH_TEST_A");
}

TEST(non_word_token_is_invalid) {
    char *out = NULL;
    Token t;
    t.kind = TOK_PIPE;
    vec_init(&t.segs);
    ASSERT_EQ(expand_word(&t, 0, &out), NSH_ERR_INVALID);
    ASSERT_TRUE(out == NULL);
    vec_free(&t.segs);

    t.kind = TOK_REDIR_OUT;
    vec_init(&t.segs);
    ASSERT_EQ(expand_word(&t, 0, &out), NSH_ERR_INVALID);
    ASSERT_TRUE(out == NULL);
    vec_free(&t.segs);

    ASSERT_EQ(expand_word(NULL, 0, &out), NSH_ERR_INVALID);
    ASSERT_TRUE(out == NULL);
}

// "$NSH_TEST_A"'$NSH_TEST_A'$NSH_TEST_A, the shape the lexer produces for
// mixed quoting, plus a brace form and a literal dollar in one word.
TEST(multi_segment_word_mixes_every_rule) {
    setenv("NSH_TEST_A", "one two", 1);
    setenv("NSH_TEST_B", "B", 1);
    unsetenv("NSH_TEST_MISSING");
    Token t;
    word_init(&t);
    word_push(&t, "$NSH_TEST_A|", true);
    word_push(&t, "$NSH_TEST_A|", false);
    word_push(&t, "${NSH_TEST_B}|", true);
    word_push(&t, "$NSH_TEST_MISSING|", true);
    word_push(&t, "$? $5 $|", true);
    word_push(&t, "", true);
    word_push(&t, "end", false);
    char *out = NULL;
    ASSERT_EQ(expand_word(&t, 12, &out), NSH_OK);
    ASSERT_STR_EQ(out, "one two|$NSH_TEST_A|B||12 $5 $|end");
    nsh_free(out);
    word_free(&t);
    unsetenv("NSH_TEST_A");
    unsetenv("NSH_TEST_B");
}

TEST(long_value_survives_intact) {
    const size_t n = 5000;
    char *big = nsh_malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        big[i] = (char)('a' + (int)(i % 26));
    }
    big[n] = '\0';
    setenv("NSH_TEST_BIG", big, 1);

    char *out = NULL;
    ASSERT_EQ(expand_one("<$NSH_TEST_BIG>", true, 0, &out), NSH_OK);
    ASSERT_EQ(strlen(out), n + 2);
    ASSERT_EQ(out[0], '<');
    ASSERT_EQ(memcmp(out + 1, big, n), 0);
    ASSERT_EQ(out[n + 1], '>');
    nsh_free(out);

    // Twice in one segment, to exercise growth after the first copy.
    ASSERT_EQ(expand_one("$NSH_TEST_BIG$NSH_TEST_BIG", true, 0, &out), NSH_OK);
    ASSERT_EQ(strlen(out), n * 2);
    ASSERT_EQ(memcmp(out, big, n), 0);
    ASSERT_EQ(memcmp(out + n, big, n), 0);
    nsh_free(out);

    nsh_free(big);
    unsetenv("NSH_TEST_BIG");
}

// A name far longer than any stack buffer would hold, to prove the key copy
// is heap sized rather than fixed.
TEST(very_long_name_is_handled) {
    const size_t n = 4000;
    char *text = nsh_malloc(n + 2);
    text[0] = '$';
    for (size_t i = 0; i < n; i++) {
        text[i + 1] = 'N';
    }
    text[n + 1] = '\0';
    char *out = NULL;
    ASSERT_EQ(expand_one(text, true, 0, &out), NSH_OK);
    ASSERT_STR_EQ(out, "");
    nsh_free(out);
    nsh_free(text);
}

TEST(null_out_pointer_is_rejected) {
    Token t;
    word_init(&t);
    word_push(&t, "x", true);
    ASSERT_EQ(expand_word(&t, 0, NULL), NSH_ERR_INVALID);
    word_free(&t);
}

TEST_MAIN()
