// Unit tests for the lexer. The lexer decides word boundaries, operator
// shapes, and what expansion is later allowed to touch, so every rule gets its
// own case and the segment structure is asserted, not just the joined text.

#include "lexer.h"

#include "../alloc/alloc.h"
#include "../util/vec.h"
#include "../../tests/harness.h"

static TokenList tl_zero(void) {
    TokenList tl;
    tl.tokens.items = NULL;
    tl.tokens.len = 0;
    tl.tokens.cap = 0;
    return tl;
}

static Token *tok_at(const TokenList *tl, size_t i) {
    return vec_get(&tl->tokens, i);
}

static WordSeg *seg_at(const Token *t, size_t i) {
    return vec_get(&t->segs, i);
}

#define ASSERT_KIND(tlp, ti, k)                                               \
    do {                                                                      \
        Token *nsh_tk = tok_at((tlp), (ti));                                  \
        ASSERT_TRUE(nsh_tk != NULL);                                          \
        ASSERT_EQ(nsh_tk->kind, (k));                                         \
        ASSERT_EQ(nsh_tk->segs.len, 0);                                       \
    } while (0)

#define ASSERT_NSEGS(tlp, ti, n)                                              \
    do {                                                                      \
        Token *nsh_tk = tok_at((tlp), (ti));                                  \
        ASSERT_TRUE(nsh_tk != NULL);                                          \
        ASSERT_EQ(nsh_tk->kind, TOK_WORD);                                    \
        ASSERT_EQ(nsh_tk->segs.len, (n));                                     \
    } while (0)

#define ASSERT_SEG(tlp, ti, si, txt, exp)                                     \
    do {                                                                      \
        Token *nsh_tk = tok_at((tlp), (ti));                                  \
        ASSERT_TRUE(nsh_tk != NULL);                                          \
        ASSERT_EQ(nsh_tk->kind, TOK_WORD);                                    \
        WordSeg *nsh_sg = seg_at(nsh_tk, (si));                               \
        ASSERT_TRUE(nsh_sg != NULL);                                          \
        ASSERT_STR_EQ(nsh_sg->text, (txt));                                   \
        ASSERT_EQ(nsh_sg->expand, (exp));                                     \
    } while (0)

// The common case: one word, one expandable segment.
#define ASSERT_WORD(tlp, ti, txt)                                             \
    do {                                                                      \
        ASSERT_NSEGS((tlp), (ti), 1);                                         \
        ASSERT_SEG((tlp), (ti), 0, (txt), true);                              \
    } while (0)

TEST(empty_line_yields_no_tokens) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 0);
    ASSERT_TRUE(tl.tokens.items != NULL);
    token_list_free(&tl);
}

TEST(only_blanks_yields_no_tokens) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("   \t  \t", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 0);
    token_list_free(&tl);
}

TEST(simple_words_split_on_spaces) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("echo hello world", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 3);
    ASSERT_WORD(&tl, 0, "echo");
    ASSERT_WORD(&tl, 1, "hello");
    ASSERT_WORD(&tl, 2, "world");
    token_list_free(&tl);
}

TEST(leading_trailing_and_repeated_blanks_are_discarded) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("  \t a \t\t  b  \t", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 2);
    ASSERT_WORD(&tl, 0, "a");
    ASSERT_WORD(&tl, 1, "b");
    token_list_free(&tl);
}

TEST(each_operator_stands_alone) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("| < > >> &", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 5);
    ASSERT_KIND(&tl, 0, TOK_PIPE);
    ASSERT_KIND(&tl, 1, TOK_REDIR_IN);
    ASSERT_KIND(&tl, 2, TOK_REDIR_OUT);
    ASSERT_KIND(&tl, 3, TOK_REDIR_APPEND);
    ASSERT_KIND(&tl, 4, TOK_AMP);
    token_list_free(&tl);
}

TEST(append_wins_over_two_single_redirects) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("a>>b", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 3);
    ASSERT_WORD(&tl, 0, "a");
    ASSERT_KIND(&tl, 1, TOK_REDIR_APPEND);
    ASSERT_WORD(&tl, 2, "b");
    token_list_free(&tl);
}

// Longest match runs left to right, so three > are an append plus a single.
TEST(three_redirect_chars_are_append_then_single) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan(">>>", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 2);
    ASSERT_KIND(&tl, 0, TOK_REDIR_APPEND);
    ASSERT_KIND(&tl, 1, TOK_REDIR_OUT);
    token_list_free(&tl);
}

TEST(operators_glued_to_words_still_split) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("a|b<c>d&e", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 9);
    ASSERT_WORD(&tl, 0, "a");
    ASSERT_KIND(&tl, 1, TOK_PIPE);
    ASSERT_WORD(&tl, 2, "b");
    ASSERT_KIND(&tl, 3, TOK_REDIR_IN);
    ASSERT_WORD(&tl, 4, "c");
    ASSERT_KIND(&tl, 5, TOK_REDIR_OUT);
    ASSERT_WORD(&tl, 6, "d");
    ASSERT_KIND(&tl, 7, TOK_AMP);
    ASSERT_WORD(&tl, 8, "e");
    token_list_free(&tl);
}

TEST(redir_err_at_line_start) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("2>f", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 2);
    ASSERT_KIND(&tl, 0, TOK_REDIR_ERR);
    ASSERT_WORD(&tl, 1, "f");
    token_list_free(&tl);
}

TEST(redir_err_after_blank) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("echo 2> f", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 3);
    ASSERT_WORD(&tl, 0, "echo");
    ASSERT_KIND(&tl, 1, TOK_REDIR_ERR);
    ASSERT_WORD(&tl, 2, "f");
    token_list_free(&tl);
}

TEST(redir_err_after_an_operator) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("a|2>b", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 4);
    ASSERT_WORD(&tl, 0, "a");
    ASSERT_KIND(&tl, 1, TOK_PIPE);
    ASSERT_KIND(&tl, 2, TOK_REDIR_ERR);
    ASSERT_WORD(&tl, 3, "b");
    token_list_free(&tl);
}

// The 2 is glued to a word, so it is ordinary text and the > is a plain
// stdout redirect.
TEST(digit_inside_a_word_is_not_a_redirect) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("a2>b", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 3);
    ASSERT_WORD(&tl, 0, "a2");
    ASSERT_KIND(&tl, 1, TOK_REDIR_OUT);
    ASSERT_WORD(&tl, 2, "b");
    token_list_free(&tl);
}

TEST(two_digit_prefix_is_not_a_redirect) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("12>f", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 3);
    ASSERT_WORD(&tl, 0, "12");
    ASSERT_KIND(&tl, 1, TOK_REDIR_OUT);
    ASSERT_WORD(&tl, 2, "f");
    token_list_free(&tl);
}

// The quote opens the word, so the 2 no longer starts one.
TEST(quoted_two_is_a_word_not_a_redirect) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("echo \"2\">f", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 4);
    ASSERT_WORD(&tl, 0, "echo");
    ASSERT_WORD(&tl, 1, "2");
    ASSERT_KIND(&tl, 2, TOK_REDIR_OUT);
    ASSERT_WORD(&tl, 3, "f");
    token_list_free(&tl);
}

TEST(bare_two_without_a_redirect_is_a_word) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("2 x", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 2);
    ASSERT_WORD(&tl, 0, "2");
    ASSERT_WORD(&tl, 1, "x");
    token_list_free(&tl);
}

// There is no append-to-stderr token, so 2>> is the stderr redirect followed
// by a leftover stdout redirect. The parser rejects the dangling one.
TEST(redir_err_does_not_absorb_a_second_gt) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("2>>f", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 3);
    ASSERT_KIND(&tl, 0, TOK_REDIR_ERR);
    ASSERT_KIND(&tl, 1, TOK_REDIR_OUT);
    ASSERT_WORD(&tl, 2, "f");
    token_list_free(&tl);
}

TEST(single_quotes_keep_dollars_and_spaces_literal) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("echo 'a $HOME b'", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 2);
    ASSERT_WORD(&tl, 0, "echo");
    ASSERT_NSEGS(&tl, 1, 1);
    ASSERT_SEG(&tl, 1, 0, "a $HOME b", false);
    token_list_free(&tl);
}

TEST(single_quotes_have_no_escapes) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("'a\\b'", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_NSEGS(&tl, 0, 1);
    ASSERT_SEG(&tl, 0, 0, "a\\b", false);
    token_list_free(&tl);
}

// A backslash cannot protect the closing quote: 'a\' is the word a\ .
TEST(backslash_does_not_escape_the_closing_single_quote) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("'a\\'", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_NSEGS(&tl, 0, 1);
    ASSERT_SEG(&tl, 0, 0, "a\\", false);
    token_list_free(&tl);
}

TEST(operators_are_literal_inside_single_quotes) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("'a|b>c'", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_NSEGS(&tl, 0, 1);
    ASSERT_SEG(&tl, 0, 0, "a|b>c", false);
    token_list_free(&tl);
}

TEST(double_quotes_keep_spaces_but_stay_expandable) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("echo \"a $HOME b\"", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 2);
    ASSERT_WORD(&tl, 0, "echo");
    ASSERT_NSEGS(&tl, 1, 1);
    ASSERT_SEG(&tl, 1, 0, "a $HOME b", true);
    token_list_free(&tl);
}

TEST(operators_are_literal_inside_double_quotes) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("\"a|b>>c\"", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_NSEGS(&tl, 0, 1);
    ASSERT_SEG(&tl, 0, 0, "a|b>>c", true);
    token_list_free(&tl);
}

// Inside double quotes only $ " \ and backquote are escapable.
TEST(double_quote_escapes_the_four_special_characters) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("\"a\\\"b\\\\c\\$d\\`e\"", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_NSEGS(&tl, 0, 1);
    ASSERT_SEG(&tl, 0, 0, "a\"b\\c$d`e", true);
    token_list_free(&tl);
}

TEST(double_quote_keeps_other_backslashes_literal) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("\"a\\nb\\'c\"", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_NSEGS(&tl, 0, 1);
    ASSERT_SEG(&tl, 0, 0, "a\\nb\\'c", true);
    token_list_free(&tl);
}

TEST(escaped_space_joins_one_word) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("a\\ b c", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 2);
    ASSERT_WORD(&tl, 0, "a b");
    ASSERT_WORD(&tl, 1, "c");
    token_list_free(&tl);
}

TEST(backslash_outside_quotes_escapes_anything) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("\\'a\\\"b\\$c\\|d\\\\e", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_WORD(&tl, 0, "'a\"b$c|d\\e");
    token_list_free(&tl);
}

TEST(trailing_backslash_is_literal) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("a\\", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_WORD(&tl, 0, "a\\");
    token_list_free(&tl);
}

TEST(lone_trailing_backslash_is_a_word) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("\\", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_WORD(&tl, 0, "\\");
    token_list_free(&tl);
}

TEST(adjacent_quoting_runs_form_one_word) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("\"a\"'b'c", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_NSEGS(&tl, 0, 3);
    ASSERT_SEG(&tl, 0, 0, "a", true);
    ASSERT_SEG(&tl, 0, 1, "b", false);
    ASSERT_SEG(&tl, 0, 2, "c", true);
    token_list_free(&tl);
}

TEST(bare_text_before_a_quote_keeps_its_place) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("x'y'z\"w\"", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_NSEGS(&tl, 0, 4);
    ASSERT_SEG(&tl, 0, 0, "x", true);
    ASSERT_SEG(&tl, 0, 1, "y", false);
    ASSERT_SEG(&tl, 0, 2, "z", true);
    ASSERT_SEG(&tl, 0, 3, "w", true);
    token_list_free(&tl);
}

TEST(empty_double_quotes_are_a_real_empty_word) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("\"\"", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_NSEGS(&tl, 0, 1);
    ASSERT_SEG(&tl, 0, 0, "", true);
    token_list_free(&tl);
}

TEST(empty_single_quotes_are_a_real_empty_word) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("''", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_NSEGS(&tl, 0, 1);
    ASSERT_SEG(&tl, 0, 0, "", false);
    token_list_free(&tl);
}

TEST(empty_quotes_survive_between_arguments) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("echo \"\" x", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 3);
    ASSERT_WORD(&tl, 0, "echo");
    ASSERT_NSEGS(&tl, 1, 1);
    ASSERT_SEG(&tl, 1, 0, "", true);
    ASSERT_WORD(&tl, 2, "x");
    token_list_free(&tl);
}

TEST(empty_quotes_glued_inside_a_word) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("a\"\"b", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_NSEGS(&tl, 0, 3);
    ASSERT_SEG(&tl, 0, 0, "a", true);
    ASSERT_SEG(&tl, 0, 1, "", true);
    ASSERT_SEG(&tl, 0, 2, "b", true);
    token_list_free(&tl);
}

TEST(hash_and_dollar_are_ordinary_word_text) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("echo # $HOME", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 3);
    ASSERT_WORD(&tl, 0, "echo");
    ASSERT_WORD(&tl, 1, "#");
    ASSERT_WORD(&tl, 2, "$HOME");
    token_list_free(&tl);
}

TEST(unterminated_single_quote_is_a_syntax_error) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("echo 'abc", &tl), NSH_ERR_SYNTAX);
    ASSERT_EQ(tl.tokens.len, 0);
    ASSERT_TRUE(tl.tokens.items != NULL);
    token_list_free(&tl);
}

TEST(unterminated_double_quote_is_a_syntax_error) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("echo \"abc", &tl), NSH_ERR_SYNTAX);
    ASSERT_EQ(tl.tokens.len, 0);
    token_list_free(&tl);
}

// The backslash consumes the last quote, so the run never closes.
TEST(escaped_closing_double_quote_leaves_it_unterminated) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("\"a\\\"", &tl), NSH_ERR_SYNTAX);
    ASSERT_EQ(tl.tokens.len, 0);
    token_list_free(&tl);
}

// Everything already tokenized has to be released, not handed back.
TEST(syntax_error_discards_earlier_tokens) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("a b c | d 'oops", &tl), NSH_ERR_SYNTAX);
    ASSERT_EQ(tl.tokens.len, 0);
    token_list_free(&tl);
}

TEST(scanning_again_after_an_error_works) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("'oops", &tl), NSH_ERR_SYNTAX);
    ASSERT_EQ(lexer_scan("ok", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_WORD(&tl, 0, "ok");
    token_list_free(&tl);
}

TEST(rescanning_overwrites_instead_of_appending) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("one two three", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 3);
    ASSERT_EQ(lexer_scan("four", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 1);
    ASSERT_WORD(&tl, 0, "four");
    ASSERT_EQ(lexer_scan("", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 0);
    token_list_free(&tl);
}

TEST(token_list_free_is_safe_twice) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("a b", &tl), NSH_OK);
    token_list_free(&tl);
    token_list_free(&tl);
    ASSERT_EQ(tl.tokens.len, 0);
}

TEST(a_long_line_grows_the_token_list) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("a a a a a a a a a a a a a a a a a a a a a", &tl),
              NSH_OK);
    ASSERT_EQ(tl.tokens.len, 21);
    ASSERT_WORD(&tl, 20, "a");
    token_list_free(&tl);
}

TEST(torture_line_mixes_every_rule) {
    TokenList tl = tl_zero();
    ASSERT_EQ(lexer_scan("ls -l \"a b\"'c'd\\ e >out 2>err |wc&", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 10);
    ASSERT_WORD(&tl, 0, "ls");
    ASSERT_WORD(&tl, 1, "-l");
    ASSERT_NSEGS(&tl, 2, 3);
    ASSERT_SEG(&tl, 2, 0, "a b", true);
    ASSERT_SEG(&tl, 2, 1, "c", false);
    ASSERT_SEG(&tl, 2, 2, "d e", true);
    ASSERT_KIND(&tl, 3, TOK_REDIR_OUT);
    ASSERT_WORD(&tl, 4, "out");
    ASSERT_KIND(&tl, 5, TOK_REDIR_ERR);
    ASSERT_WORD(&tl, 6, "err");
    ASSERT_KIND(&tl, 7, TOK_PIPE);
    ASSERT_WORD(&tl, 8, "wc");
    ASSERT_KIND(&tl, 9, TOK_AMP);
    token_list_free(&tl);
}

TEST_MAIN()
