// Unit tests for the parser. Every case goes through the real lexer, so the
// token stream under test is the one the shell actually produces. Each case
// frees its pipeline, which is what lets ASan prove the ownership rules.

#include "parser.h"

#include <string.h>

#include "lexer.h"

#include "../util/vec.h"
#include "../../tests/harness.h"

static TokenList tl_zero(void) {
    TokenList tl;
    tl.tokens.items = NULL;
    tl.tokens.len = 0;
    tl.tokens.cap = 0;
    return tl;
}

static Pipeline pl_zero(void) {
    Pipeline p;
    p.cmds.items = NULL;
    p.cmds.len = 0;
    p.cmds.cap = 0;
    p.background = false;
    return p;
}

// Lex then parse. The token list is freed here, so a leak in the parser's
// consume-everything contract shows up as a double free or a leak under ASan.
static NshError run(const char *line, Pipeline *out) {
    TokenList tl = tl_zero();
    NshError lex = lexer_scan(line, &tl);
    if (lex != NSH_OK) {
        token_list_free(&tl);
        return lex;
    }
    NshError err = parser_parse(&tl, out);
    token_list_free(&tl);
    return err;
}

// Flattens a word's segments into one comparable string. The buffer is static
// and reused, so only one word can be inspected at a time.
static const char *word_text(const Token *t) {
    static char buf[128];
    size_t n = 0;
    buf[0] = '\0';
    if (t == NULL || t->kind != TOK_WORD) {
        return "(not a word)";
    }
    for (size_t i = 0; i < t->segs.len; i++) {
        const WordSeg *seg = vec_get(&t->segs, i);
        size_t len = strlen(seg->text);
        if (n + len >= sizeof(buf)) {
            break;
        }
        memcpy(buf + n, seg->text, len);
        n += len;
        buf[n] = '\0';
    }
    return buf;
}

static Command *cmd_at(const Pipeline *p, size_t i) {
    return vec_get(&p->cmds, i);
}

#define ASSERT_NCMDS(plp, n) ASSERT_EQ((plp)->cmds.len, (n))

#define ASSERT_NARGV(plp, ci, n)                                              \
    do {                                                                      \
        Command *nsh_c = cmd_at((plp), (ci));                                 \
        ASSERT_TRUE(nsh_c != NULL);                                           \
        ASSERT_EQ(nsh_c->words.len, (n));                                     \
    } while (0)

#define ASSERT_ARGV(plp, ci, wi, txt)                                         \
    do {                                                                      \
        Command *nsh_c = cmd_at((plp), (ci));                                 \
        ASSERT_TRUE(nsh_c != NULL);                                           \
        Token *nsh_w = vec_get(&nsh_c->words, (wi));                          \
        ASSERT_TRUE(nsh_w != NULL);                                           \
        ASSERT_STR_EQ(word_text(nsh_w), (txt));                               \
    } while (0)

// slot names a Command member: redir_in, redir_out or redir_err.
#define ASSERT_REDIR(plp, ci, slot, txt)                                      \
    do {                                                                      \
        Command *nsh_c = cmd_at((plp), (ci));                                 \
        ASSERT_TRUE(nsh_c != NULL);                                           \
        ASSERT_TRUE(nsh_c->slot != NULL);                                     \
        ASSERT_STR_EQ(word_text(nsh_c->slot), (txt));                         \
    } while (0)

#define ASSERT_NO_REDIRS(plp, ci)                                             \
    do {                                                                      \
        Command *nsh_c = cmd_at((plp), (ci));                                 \
        ASSERT_TRUE(nsh_c != NULL);                                           \
        ASSERT_TRUE(nsh_c->redir_in == NULL);                                 \
        ASSERT_TRUE(nsh_c->redir_out == NULL);                                \
        ASSERT_TRUE(nsh_c->redir_err == NULL);                                \
        ASSERT_TRUE(nsh_c->redir_append == false);                            \
    } while (0)

// A rejected line must leave out usable, not merely non-crashing.
#define ASSERT_EMPTY_PIPELINE(plp)                                            \
    do {                                                                      \
        ASSERT_EQ((plp)->cmds.len, 0);                                        \
        ASSERT_TRUE((plp)->cmds.items != NULL);                               \
        ASSERT_TRUE((plp)->background == false);                              \
    } while (0)

// Shorthand for the many one-line syntax error cases.
#define ASSERT_SYNTAX(line)                                                   \
    do {                                                                      \
        Pipeline nsh_pl = pl_zero();                                          \
        ASSERT_EQ(run((line), &nsh_pl), NSH_ERR_SYNTAX);                      \
        ASSERT_EMPTY_PIPELINE(&nsh_pl);                                       \
        pipeline_free(&nsh_pl);                                               \
    } while (0)

// Words and commands

TEST(empty_line_is_an_empty_pipeline) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("", &pl), NSH_OK);
    ASSERT_EMPTY_PIPELINE(&pl);
    pipeline_free(&pl);
}

TEST(blank_only_line_is_an_empty_pipeline) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("   \t ", &pl), NSH_OK);
    ASSERT_EMPTY_PIPELINE(&pl);
    pipeline_free(&pl);
}

TEST(single_command_keeps_argv_order) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("echo one two three", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 1);
    ASSERT_NARGV(&pl, 0, 4);
    ASSERT_ARGV(&pl, 0, 0, "echo");
    ASSERT_ARGV(&pl, 0, 1, "one");
    ASSERT_ARGV(&pl, 0, 2, "two");
    ASSERT_ARGV(&pl, 0, 3, "three");
    ASSERT_NO_REDIRS(&pl, 0);
    ASSERT_TRUE(pl.background == false);
    pipeline_free(&pl);
}

TEST(one_word_command) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("ls", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 1);
    ASSERT_NARGV(&pl, 0, 1);
    ASSERT_ARGV(&pl, 0, 0, "ls");
    pipeline_free(&pl);
}

TEST(quoted_word_stays_one_argv_entry) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("echo \"a b\"", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 1);
    ASSERT_NARGV(&pl, 0, 2);
    ASSERT_ARGV(&pl, 0, 0, "echo");
    ASSERT_ARGV(&pl, 0, 1, "a b");
    pipeline_free(&pl);
}

// The parser must not flatten or reorder segments; expansion needs them intact.
TEST(multi_segment_word_survives_with_its_segments) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("echo \"a\"'b'c", &pl), NSH_OK);
    ASSERT_NARGV(&pl, 0, 2);
    ASSERT_ARGV(&pl, 0, 1, "abc");
    Command *c = cmd_at(&pl, 0);
    ASSERT_TRUE(c != NULL);
    Token *w = vec_get(&c->words, 1);
    ASSERT_TRUE(w != NULL);
    ASSERT_EQ(w->segs.len, 3);
    WordSeg *s1 = vec_get(&w->segs, 1);
    ASSERT_TRUE(s1 != NULL);
    ASSERT_STR_EQ(s1->text, "b");
    ASSERT_TRUE(s1->expand == false);
    pipeline_free(&pl);
}

TEST(empty_quoted_word_is_a_real_argv_entry) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("echo \"\"", &pl), NSH_OK);
    ASSERT_NARGV(&pl, 0, 2);
    ASSERT_ARGV(&pl, 0, 1, "");
    pipeline_free(&pl);
}

// Pipes

TEST(two_stage_pipeline) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("a | b", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 2);
    ASSERT_NARGV(&pl, 0, 1);
    ASSERT_ARGV(&pl, 0, 0, "a");
    ASSERT_NARGV(&pl, 1, 1);
    ASSERT_ARGV(&pl, 1, 0, "b");
    pipeline_free(&pl);
}

TEST(three_stage_pipeline_with_arguments) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("ls -l | grep x | wc -l", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 3);
    ASSERT_NARGV(&pl, 0, 2);
    ASSERT_ARGV(&pl, 0, 0, "ls");
    ASSERT_ARGV(&pl, 0, 1, "-l");
    ASSERT_NARGV(&pl, 1, 2);
    ASSERT_ARGV(&pl, 1, 0, "grep");
    ASSERT_ARGV(&pl, 1, 1, "x");
    ASSERT_NARGV(&pl, 2, 2);
    ASSERT_ARGV(&pl, 2, 0, "wc");
    ASSERT_ARGV(&pl, 2, 1, "-l");
    pipeline_free(&pl);
}

TEST(pipes_glued_to_words_still_split_commands) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("a|b|c", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 3);
    ASSERT_ARGV(&pl, 0, 0, "a");
    ASSERT_ARGV(&pl, 1, 0, "b");
    ASSERT_ARGV(&pl, 2, 0, "c");
    pipeline_free(&pl);
}

TEST(each_stage_keeps_its_own_redirects) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("a < in | b > out", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 2);
    ASSERT_REDIR(&pl, 0, redir_in, "in");
    ASSERT_TRUE(cmd_at(&pl, 0)->redir_out == NULL);
    ASSERT_REDIR(&pl, 1, redir_out, "out");
    ASSERT_TRUE(cmd_at(&pl, 1)->redir_in == NULL);
    pipeline_free(&pl);
}

// Redirect placement

TEST(redirect_after_argv) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("echo hi > f", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 1);
    ASSERT_NARGV(&pl, 0, 2);
    ASSERT_ARGV(&pl, 0, 0, "echo");
    ASSERT_ARGV(&pl, 0, 1, "hi");
    ASSERT_REDIR(&pl, 0, redir_out, "f");
    ASSERT_TRUE(cmd_at(&pl, 0)->redir_append == false);
    pipeline_free(&pl);
}

TEST(redirect_between_argv_words) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("echo > f hi", &pl), NSH_OK);
    ASSERT_NARGV(&pl, 0, 2);
    ASSERT_ARGV(&pl, 0, 0, "echo");
    ASSERT_ARGV(&pl, 0, 1, "hi");
    ASSERT_REDIR(&pl, 0, redir_out, "f");
    pipeline_free(&pl);
}

// Bash accepts a leading redirect, and it means the same thing.
TEST(redirect_before_argv) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("> f echo hi", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 1);
    ASSERT_NARGV(&pl, 0, 2);
    ASSERT_ARGV(&pl, 0, 0, "echo");
    ASSERT_ARGV(&pl, 0, 1, "hi");
    ASSERT_REDIR(&pl, 0, redir_out, "f");
    pipeline_free(&pl);
}

TEST(redirect_before_argv_in_a_later_stage) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("a | < f b", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 2);
    ASSERT_NARGV(&pl, 1, 1);
    ASSERT_ARGV(&pl, 1, 0, "b");
    ASSERT_REDIR(&pl, 1, redir_in, "f");
    pipeline_free(&pl);
}

// Redirect slots

TEST(input_redirect_fills_redir_in) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("cat < f", &pl), NSH_OK);
    ASSERT_REDIR(&pl, 0, redir_in, "f");
    ASSERT_TRUE(cmd_at(&pl, 0)->redir_out == NULL);
    ASSERT_TRUE(cmd_at(&pl, 0)->redir_err == NULL);
    pipeline_free(&pl);
}

TEST(append_redirect_sets_the_append_flag) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("echo hi >> f", &pl), NSH_OK);
    ASSERT_REDIR(&pl, 0, redir_out, "f");
    ASSERT_TRUE(cmd_at(&pl, 0)->redir_append == true);
    pipeline_free(&pl);
}

TEST(truncate_redirect_leaves_the_append_flag_clear) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("echo hi > f", &pl), NSH_OK);
    ASSERT_TRUE(cmd_at(&pl, 0)->redir_append == false);
    pipeline_free(&pl);
}

TEST(stderr_redirect_fills_redir_err) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("cmd 2> e", &pl), NSH_OK);
    ASSERT_REDIR(&pl, 0, redir_err, "e");
    ASSERT_TRUE(cmd_at(&pl, 0)->redir_in == NULL);
    ASSERT_TRUE(cmd_at(&pl, 0)->redir_out == NULL);
    pipeline_free(&pl);
}

TEST(all_three_slots_filled_at_once) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("cmd < in > out 2> err", &pl), NSH_OK);
    ASSERT_NARGV(&pl, 0, 1);
    ASSERT_ARGV(&pl, 0, 0, "cmd");
    ASSERT_REDIR(&pl, 0, redir_in, "in");
    ASSERT_REDIR(&pl, 0, redir_out, "out");
    ASSERT_REDIR(&pl, 0, redir_err, "err");
    ASSERT_TRUE(cmd_at(&pl, 0)->redir_append == false);
    pipeline_free(&pl);
}

// Same three slots, this time with >> for stdout and the operators scattered
// through the argv words.
TEST(all_three_slots_filled_with_append_and_scattered_words) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("2> err cmd >> out -x < in -y", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 1);
    ASSERT_NARGV(&pl, 0, 3);
    ASSERT_ARGV(&pl, 0, 0, "cmd");
    ASSERT_ARGV(&pl, 0, 1, "-x");
    ASSERT_ARGV(&pl, 0, 2, "-y");
    ASSERT_REDIR(&pl, 0, redir_in, "in");
    ASSERT_REDIR(&pl, 0, redir_out, "out");
    ASSERT_REDIR(&pl, 0, redir_err, "err");
    ASSERT_TRUE(cmd_at(&pl, 0)->redir_append == true);
    pipeline_free(&pl);
}

TEST(quoted_redirect_target_keeps_its_spaces) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("echo hi > \"a b\"", &pl), NSH_OK);
    ASSERT_NARGV(&pl, 0, 2);
    ASSERT_REDIR(&pl, 0, redir_out, "a b");
    pipeline_free(&pl);
}

// Duplicate slots

TEST(two_input_redirects_are_a_syntax_error) {
    ASSERT_SYNTAX("cat < a < b");
}

TEST(truncate_then_append_are_a_syntax_error) {
    ASSERT_SYNTAX("cmd > a >> b");
}

TEST(append_then_truncate_are_a_syntax_error) {
    ASSERT_SYNTAX("cmd >> a > b");
}

TEST(two_truncate_redirects_are_a_syntax_error) {
    ASSERT_SYNTAX("cmd > a > b");
}

TEST(two_stderr_redirects_are_a_syntax_error) {
    ASSERT_SYNTAX("cmd 2> a 2> b");
}

// The duplicate check is per command, not per line.
TEST(the_same_slot_in_two_stages_is_fine) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("a > x | b > y", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 2);
    ASSERT_REDIR(&pl, 0, redir_out, "x");
    ASSERT_REDIR(&pl, 1, redir_out, "y");
    pipeline_free(&pl);
}

// Missing or bad redirect targets

TEST(redirect_at_end_of_line_has_no_target) {
    ASSERT_SYNTAX("echo >");
}

TEST(input_redirect_at_end_of_line_has_no_target) {
    ASSERT_SYNTAX("echo <");
}

TEST(append_at_end_of_line_has_no_target) {
    ASSERT_SYNTAX("echo >>");
}

TEST(stderr_redirect_at_end_of_line_has_no_target) {
    ASSERT_SYNTAX("echo 2>");
}

TEST(redirect_followed_by_a_pipe_has_no_target) {
    ASSERT_SYNTAX("echo > | b");
}

TEST(redirect_followed_by_another_redirect_has_no_target) {
    ASSERT_SYNTAX("echo > < f");
}

TEST(redirect_followed_by_an_amp_has_no_target) {
    ASSERT_SYNTAX("echo > &");
}

// The lexer has no append-to-stderr token, so 2>> is 2> followed by a bare >.
// The 2> then has no word target and the line is rejected.
TEST(stderr_append_is_not_supported_and_is_a_syntax_error) {
    ASSERT_SYNTAX("cmd 2>> f");
}

TEST(stderr_append_glued_is_a_syntax_error) {
    ASSERT_SYNTAX("cmd 2>>f");
}

// Empty commands

TEST(leading_pipe_is_a_syntax_error) {
    ASSERT_SYNTAX("| a");
}

TEST(trailing_pipe_is_a_syntax_error) {
    ASSERT_SYNTAX("a |");
}

TEST(pipe_alone_is_a_syntax_error) {
    ASSERT_SYNTAX("|");
}

TEST(adjacent_pipes_are_a_syntax_error) {
    ASSERT_SYNTAX("a || b");
}

TEST(pipes_with_only_a_redirect_between_them_are_a_syntax_error) {
    ASSERT_SYNTAX("a | > f | b");
}

TEST(a_command_of_only_redirects_is_a_syntax_error) {
    ASSERT_SYNTAX("> f");
}

TEST(a_first_stage_of_only_redirects_is_a_syntax_error) {
    ASSERT_SYNTAX("> f | b");
}

TEST(a_last_stage_of_only_redirects_is_a_syntax_error) {
    ASSERT_SYNTAX("a | > f");
}

TEST(only_redirects_on_the_whole_line_is_a_syntax_error) {
    ASSERT_SYNTAX("< in > out 2> err");
}

// Background

TEST(trailing_amp_sets_background) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("sleep 5 &", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 1);
    ASSERT_NARGV(&pl, 0, 2);
    ASSERT_ARGV(&pl, 0, 0, "sleep");
    ASSERT_ARGV(&pl, 0, 1, "5");
    ASSERT_TRUE(pl.background == true);
    pipeline_free(&pl);
}

TEST(amp_glued_to_the_last_word_still_sets_background) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("sleep 5&", &pl), NSH_OK);
    ASSERT_NARGV(&pl, 0, 2);
    ASSERT_TRUE(pl.background == true);
    pipeline_free(&pl);
}

TEST(amp_backgrounds_a_whole_pipeline) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("a | b &", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 2);
    ASSERT_TRUE(pl.background == true);
    pipeline_free(&pl);
}

TEST(amp_after_a_redirect_target_sets_background) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("cmd > f &", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 1);
    ASSERT_REDIR(&pl, 0, redir_out, "f");
    ASSERT_TRUE(pl.background == true);
    pipeline_free(&pl);
}

TEST(no_amp_leaves_background_clear) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("a | b", &pl), NSH_OK);
    ASSERT_TRUE(pl.background == false);
    pipeline_free(&pl);
}

TEST(amp_alone_is_a_syntax_error) {
    ASSERT_SYNTAX("&");
}

TEST(amp_before_a_command_is_a_syntax_error) {
    ASSERT_SYNTAX("& a");
}

TEST(amp_between_commands_is_a_syntax_error) {
    ASSERT_SYNTAX("a & b");
}

TEST(amp_before_a_pipe_is_a_syntax_error) {
    ASSERT_SYNTAX("a & | b");
}

TEST(two_amps_are_a_syntax_error) {
    ASSERT_SYNTAX("a & &");
}

TEST(amp_after_an_empty_stage_is_a_syntax_error) {
    ASSERT_SYNTAX("a | &");
}

// Ownership and reuse

TEST(parse_leaves_the_token_list_empty_on_success) {
    TokenList tl = tl_zero();
    Pipeline pl = pl_zero();
    ASSERT_EQ(lexer_scan("a b | c > d &", &tl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 7);
    ASSERT_EQ(parser_parse(&tl, &pl), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 0);
    ASSERT_TRUE(tl.tokens.items != NULL);
    ASSERT_NCMDS(&pl, 2);
    pipeline_free(&pl);
    token_list_free(&tl);
}

// The tokens after the failure point have to be released too, not stranded.
TEST(parse_leaves_the_token_list_empty_on_error) {
    TokenList tl = tl_zero();
    Pipeline pl = pl_zero();
    ASSERT_EQ(lexer_scan("a | | b c d > e", &tl), NSH_OK);
    ASSERT_EQ(parser_parse(&tl, &pl), NSH_ERR_SYNTAX);
    ASSERT_EQ(tl.tokens.len, 0);
    ASSERT_TRUE(tl.tokens.items != NULL);
    ASSERT_EMPTY_PIPELINE(&pl);
    pipeline_free(&pl);
    token_list_free(&tl);
}

// A duplicate slot happens after several commands are already built, so this
// is the case where the partial pipeline has to be torn down.
TEST(a_late_error_frees_the_commands_built_so_far) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("a x | b y | c < i < j", &pl), NSH_ERR_SYNTAX);
    ASSERT_EMPTY_PIPELINE(&pl);
    pipeline_free(&pl);
}

TEST(parsing_again_overwrites_the_pipeline) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("a b c | d", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 2);
    ASSERT_EQ(run("solo &", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 1);
    ASSERT_ARGV(&pl, 0, 0, "solo");
    ASSERT_TRUE(pl.background == true);
    ASSERT_EQ(run("", &pl), NSH_OK);
    ASSERT_EMPTY_PIPELINE(&pl);
    pipeline_free(&pl);
}

TEST(parsing_after_an_error_works) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("a |", &pl), NSH_ERR_SYNTAX);
    ASSERT_EQ(run("ok", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 1);
    ASSERT_ARGV(&pl, 0, 0, "ok");
    pipeline_free(&pl);
}

TEST(pipeline_free_is_safe_twice) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("a < i > o 2> e | b", &pl), NSH_OK);
    pipeline_free(&pl);
    pipeline_free(&pl);
    ASSERT_EQ(pl.cmds.len, 0);
}

TEST(pipeline_free_is_safe_on_a_zeroed_pipeline) {
    Pipeline pl = pl_zero();
    pipeline_free(&pl);
    ASSERT_EQ(pl.cmds.len, 0);
    ASSERT_TRUE(pl.cmds.items == NULL);
}

TEST(pipeline_free_is_safe_on_null) {
    pipeline_free(NULL);
    ASSERT_TRUE(true);
}

TEST(a_long_pipeline_grows_the_command_vector) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("a|a|a|a|a|a|a|a|a|a|a|a", &pl), NSH_OK);
    ASSERT_NCMDS(&pl, 12);
    ASSERT_ARGV(&pl, 11, 0, "a");
    pipeline_free(&pl);
}

TEST(torture_line_mixes_every_rule) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(run("< in ls -l \"a b\" | 2> err grep x >> out | wc -l &", &pl),
              NSH_OK);
    ASSERT_NCMDS(&pl, 3);
    ASSERT_NARGV(&pl, 0, 3);
    ASSERT_ARGV(&pl, 0, 0, "ls");
    ASSERT_ARGV(&pl, 0, 1, "-l");
    ASSERT_ARGV(&pl, 0, 2, "a b");
    ASSERT_REDIR(&pl, 0, redir_in, "in");
    ASSERT_NARGV(&pl, 1, 2);
    ASSERT_ARGV(&pl, 1, 0, "grep");
    ASSERT_ARGV(&pl, 1, 1, "x");
    ASSERT_REDIR(&pl, 1, redir_err, "err");
    ASSERT_REDIR(&pl, 1, redir_out, "out");
    ASSERT_TRUE(cmd_at(&pl, 1)->redir_append == true);
    ASSERT_NARGV(&pl, 2, 2);
    ASSERT_ARGV(&pl, 2, 0, "wc");
    ASSERT_NO_REDIRS(&pl, 2);
    ASSERT_TRUE(pl.background == true);
    pipeline_free(&pl);
}

TEST_MAIN()
