// Tests for the parser, driven by hand-built token lists, not by the lexer.

#include "parser.h"

#include <stdio.h>
#include <string.h>

#include "ast.h"

#include "../alloc/alloc.h"
#include "../util/vec.h"
#include "../../tests/harness.h"

// Building blocks

static Token *mk_word(const char *text) {
    Token *t = nsh_malloc(sizeof(*t));
    t->kind = TOK_WORD;
    vec_init(&t->segs);
    WordSeg *s = nsh_malloc(sizeof(*s));
    size_t n = strlen(text) + 1;
    s->text = nsh_malloc(n);
    memcpy(s->text, text, n);
    s->expand = true;
    vec_push(&t->segs, s);
    return t;
}

static Token *mk_op(TokenKind kind) {
    Token *t = nsh_malloc(sizeof(*t));
    t->kind = kind;
    vec_init(&t->segs);
    return t;
}

// Glues another quoting run onto a word, so multi-segment words are testable.
static Token *seg_add(Token *t, const char *text) {
    WordSeg *s = nsh_malloc(sizeof(*s));
    size_t n = strlen(text) + 1;
    s->text = nsh_malloc(n);
    memcpy(s->text, text, n);
    s->expand = false;
    vec_push(&t->segs, s);
    return t;
}

// A spec entry is an operator spelling, or a word. "=x" forces the word "x".
static Token *mk_spec(const char *spec) {
    static const struct {
        const char *text;
        TokenKind kind;
    } ops[] = {{"|", TOK_PIPE},        {"<", TOK_REDIR_IN},
               {">", TOK_REDIR_OUT},   {">>", TOK_REDIR_APPEND},
               {"2>", TOK_REDIR_ERR},  {"&", TOK_AMP},
               {"&&", TOK_AND_IF},     {"||", TOK_OR_IF},
               {";", TOK_SEMI},        {"\n", TOK_NEWLINE},
               {"(", TOK_LPAREN},      {")", TOK_RPAREN}};
    if (spec[0] == '=') {
        return mk_word(spec + 1);
    }
    for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); i++) {
        if (strcmp(spec, ops[i].text) == 0) {
            return mk_op(ops[i].kind);
        }
    }
    return mk_word(spec);
}

static TokenList toks_n(const char *const *spec, size_t n) {
    TokenList tl;
    vec_init(&tl.tokens);
    for (size_t i = 0; i < n; i++) {
        vec_push(&tl.tokens, mk_spec(spec[i]));
    }
    return tl;
}

static size_t spec_len(const char *const *spec) {
    size_t n = 0;
    while (spec[n] != NULL) {
        n++;
    }
    return n;
}

static TokenList toks_all(const char *const *spec) {
    return toks_n(spec, spec_len(spec));
}

#define SPEC(...) ((const char *const[]){__VA_ARGS__, NULL})
#define TOKS(...) toks_all(SPEC(__VA_ARGS__))

// The token list is freed here, so ASan sees any break in the ownership rules.
static NshError parse(TokenList tl, Node **out) {
    *out = (Node *)0x1;
    NshError err = parser_parse_program(&tl, out);
    token_list_free(&tl);
    return err;
}

// Tree accessors

// The buffer is static and reused, so only one word at a time.
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

static Node *lst(const Node *n, size_t i) {
    return vec_get(&n->u.list.nodes, i);
}

static AndOrItem *aoi(const Node *n, size_t i) {
    return vec_get(&n->u.andor.items, i);
}

static Command *cmd_at(const Node *n, size_t i) {
    return vec_get(&n->u.pipeline.pl.cmds, i);
}

static Node *ifc(const Node *n, size_t i) {
    return vec_get(&n->u.nif.conds, i);
}

static Node *ifb(const Node *n, size_t i) {
    return vec_get(&n->u.nif.bodies, i);
}

// Argv text of word wi in stage ci of a NODE_PIPELINE.
static const char *argv_at(const Node *n, size_t ci, size_t wi) {
    if (n == NULL || n->kind != NODE_PIPELINE) {
        return "(not a pipeline)";
    }
    const Command *c = cmd_at(n, ci);
    if (c == NULL) {
        return "(no such stage)";
    }
    return word_text(vec_get(&c->words, wi));
}

// Every node reachable in one hop, for shapes asserted in a single line.
#define ASSERT_KIND(n, k)                                                     \
    do {                                                                      \
        ASSERT_TRUE((n) != NULL);                                             \
        ASSERT_EQ((n)->kind, (k));                                            \
    } while (0)

#define ASSERT_CMD1(n, txt)                                                   \
    do {                                                                      \
        ASSERT_KIND((n), NODE_PIPELINE);                                      \
        ASSERT_EQ((n)->u.pipeline.pl.cmds.len, 1);                            \
        ASSERT_STR_EQ(argv_at((n), 0, 0), (txt));                             \
    } while (0)

#define ASSERT_ERR(err, spec_list)                                            \
    do {                                                                      \
        Node *nsh_n = NULL;                                                   \
        ASSERT_EQ(parse(toks_all(spec_list), &nsh_n), (err));                 \
        ASSERT_TRUE(nsh_n == NULL);                                           \
    } while (0)

#define ASSERT_SYNTAX(...) ASSERT_ERR(NSH_ERR_SYNTAX, SPEC(__VA_ARGS__))
#define ASSERT_INCOMPLETE(...) ASSERT_ERR(NSH_ERR_INCOMPLETE, SPEC(__VA_ARGS__))

// Truncating a valid stream anywhere past from must always read as unfinished.
static void chops_incomplete(int *nsh_failed, const char *const *spec,
                             size_t from) {
    size_t n = spec_len(spec);
    for (size_t k = from; k < n; k++) {
        Node *out = NULL;
        NshError err = parse(toks_n(spec, k), &out);
        if (err != NSH_ERR_INCOMPLETE) {
            printf("    chop after %zu token(s) gave %d\n", k, (int)err);
        }
        ASSERT_EQ(err, NSH_ERR_INCOMPLETE);
        ASSERT_TRUE(out == NULL);
    }
}

// Empty programs

TEST(no_tokens_is_an_empty_program) {
    Node *n = NULL;
    TokenList tl;
    vec_init(&tl.tokens);
    ASSERT_EQ(parse(tl, &n), NSH_OK);
    ASSERT_TRUE(n == NULL);
}

TEST(separators_only_is_an_empty_program) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS(";", "\n", ";", "\n"), &n), NSH_OK);
    ASSERT_TRUE(n == NULL);
}

// Simple commands and pipelines

TEST(one_command_is_a_bare_pipeline) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("echo", "one", "two"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_PIPELINE);
    ASSERT_EQ(n->u.pipeline.negate, false);
    ASSERT_EQ(n->u.pipeline.pl.background, false);
    ASSERT_EQ(n->u.pipeline.pl.cmds.len, 1);
    ASSERT_EQ(cmd_at(n, 0)->words.len, 3);
    ASSERT_STR_EQ(argv_at(n, 0, 0), "echo");
    ASSERT_STR_EQ(argv_at(n, 0, 1), "one");
    ASSERT_STR_EQ(argv_at(n, 0, 2), "two");
    ast_free(n);
}

TEST(three_stage_pipeline) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("a", "|", "b", "|", "c"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_PIPELINE);
    ASSERT_EQ(n->u.pipeline.pl.cmds.len, 3);
    ASSERT_STR_EQ(argv_at(n, 0, 0), "a");
    ASSERT_STR_EQ(argv_at(n, 1, 0), "b");
    ASSERT_STR_EQ(argv_at(n, 2, 0), "c");
    ast_free(n);
}

TEST(newlines_after_a_pipe_are_skipped) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("a", "|", "\n", "\n", "b"), &n), NSH_OK);
    ASSERT_EQ(n->u.pipeline.pl.cmds.len, 2);
    ast_free(n);
}

TEST(multi_segment_words_keep_their_segments) {
    TokenList tl;
    vec_init(&tl.tokens);
    vec_push(&tl.tokens, mk_word("echo"));
    vec_push(&tl.tokens, seg_add(seg_add(mk_word("a"), "b"), "c"));
    Node *n = NULL;
    ASSERT_EQ(parse(tl, &n), NSH_OK);
    ASSERT_STR_EQ(argv_at(n, 0, 1), "abc");
    Token *w = vec_get(&cmd_at(n, 0)->words, 1);
    ASSERT_EQ(w->segs.len, 3);
    ASSERT_STR_EQ(((WordSeg *)vec_get(&w->segs, 1))->text, "b");
    ast_free(n);
}

// Redirects

TEST(every_redirect_slot_fills_once) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("2>", "e", "cmd", ">>", "o", "-x", "<", "i"), &n),
              NSH_OK);
    ASSERT_EQ(n->u.pipeline.pl.cmds.len, 1);
    Command *c = cmd_at(n, 0);
    ASSERT_EQ(c->words.len, 2);
    ASSERT_STR_EQ(argv_at(n, 0, 0), "cmd");
    ASSERT_STR_EQ(argv_at(n, 0, 1), "-x");
    ASSERT_STR_EQ(word_text(c->redir_in), "i");
    ASSERT_STR_EQ(word_text(c->redir_out), "o");
    ASSERT_STR_EQ(word_text(c->redir_err), "e");
    ASSERT_EQ(c->redir_append, true);
    ast_free(n);
}

TEST(truncating_redirect_leaves_append_clear) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("cmd", ">", "o"), &n), NSH_OK);
    ASSERT_EQ(cmd_at(n, 0)->redir_append, false);
    ast_free(n);
}

TEST(the_same_slot_in_two_stages_is_fine) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("a", ">", "x", "|", "b", ">", "y"), &n), NSH_OK);
    ASSERT_STR_EQ(word_text(cmd_at(n, 0)->redir_out), "x");
    ASSERT_STR_EQ(word_text(cmd_at(n, 1)->redir_out), "y");
    ast_free(n);
}

TEST(duplicate_redirect_slots_are_syntax_errors) {
    ASSERT_SYNTAX("cat", "<", "a", "<", "b");
    ASSERT_SYNTAX("cmd", ">", "a", ">>", "b");
    ASSERT_SYNTAX("cmd", ">>", "a", ">", "b");
    ASSERT_SYNTAX("cmd", ">", "a", ">", "b");
    ASSERT_SYNTAX("cmd", "2>", "a", "2>", "b");
}

TEST(a_redirect_target_that_is_an_operator_is_a_syntax_error) {
    ASSERT_SYNTAX("echo", ">", "|", "b");
    ASSERT_SYNTAX("echo", ">", "<", "f");
    ASSERT_SYNTAX("echo", ">", "&");
    ASSERT_SYNTAX("echo", ">", ";");
    ASSERT_SYNTAX("echo", ">", "\n");
}

TEST(a_redirect_with_no_target_at_all_is_incomplete) {
    ASSERT_INCOMPLETE("echo", ">");
    ASSERT_INCOMPLETE("echo", "<");
    ASSERT_INCOMPLETE("echo", ">>");
    ASSERT_INCOMPLETE("echo", "2>");
}

TEST(a_command_of_only_redirects_is_a_syntax_error) {
    ASSERT_SYNTAX("<", "in", ">", "out", "2>", "err");
    ASSERT_SYNTAX(">", "f", "|", "b");
    ASSERT_SYNTAX("a", "|", ">", "f");
}

// Operators with nothing before or after them

TEST(a_leading_operator_is_a_syntax_error) {
    ASSERT_SYNTAX("|", "a");
    ASSERT_SYNTAX("&&", "a");
    ASSERT_SYNTAX("||", "a");
    ASSERT_SYNTAX("&", "a");
    ASSERT_SYNTAX("&");
}

TEST(a_trailing_operator_is_incomplete) {
    ASSERT_INCOMPLETE("a", "|");
    ASSERT_INCOMPLETE("a", "&&");
    ASSERT_INCOMPLETE("a", "||");
    ASSERT_INCOMPLETE("a", "&&", "\n");
}

TEST(adjacent_operators_are_syntax_errors) {
    ASSERT_SYNTAX("a", "|", "|", "b");
    ASSERT_SYNTAX("a", "&&", "&&", "b");
    ASSERT_SYNTAX("a", "&", "&");
    ASSERT_SYNTAX("a", "&", "|", "b");
}

// && and ||

TEST(and_if_builds_a_two_item_andor) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("a", "&&", "b"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_ANDOR);
    ASSERT_EQ(n->u.andor.items.len, 2);
    ASSERT_EQ(aoi(n, 0)->op, ANDOR_FIRST);
    ASSERT_EQ(aoi(n, 1)->op, ANDOR_AND);
    ASSERT_CMD1(aoi(n, 0)->node, "a");
    ASSERT_CMD1(aoi(n, 1)->node, "b");
    ast_free(n);
}

TEST(or_if_builds_a_two_item_andor) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("a", "||", "b"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_ANDOR);
    ASSERT_EQ(aoi(n, 1)->op, ANDOR_OR);
    ast_free(n);
}

// The chain is flat: no precedence between && and ||, left to right.
TEST(mixed_and_or_chains_stay_flat) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("a", "&&", "b", "||", "c", "&&", "d"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_ANDOR);
    ASSERT_EQ(n->u.andor.items.len, 4);
    ASSERT_EQ(aoi(n, 0)->op, ANDOR_FIRST);
    ASSERT_EQ(aoi(n, 1)->op, ANDOR_AND);
    ASSERT_EQ(aoi(n, 2)->op, ANDOR_OR);
    ASSERT_EQ(aoi(n, 3)->op, ANDOR_AND);
    ASSERT_CMD1(aoi(n, 3)->node, "d");
    ast_free(n);
}

TEST(newlines_after_and_if_are_skipped) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("a", "&&", "\n", "\n", "b"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_ANDOR);
    ASSERT_EQ(n->u.andor.items.len, 2);
    ast_free(n);
}

TEST(andor_items_hold_whole_pipelines) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("a", "|", "b", "&&", "c"), &n), NSH_OK);
    Node *first = aoi(n, 0)->node;
    ASSERT_KIND(first, NODE_PIPELINE);
    ASSERT_EQ(first->u.pipeline.pl.cmds.len, 2);
    ast_free(n);
}

// Negation

TEST(bang_sets_negate_on_the_pipeline) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("!", "a", "|", "b"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_PIPELINE);
    ASSERT_EQ(n->u.pipeline.negate, true);
    ASSERT_EQ(n->u.pipeline.pl.cmds.len, 2);
    ast_free(n);
}

TEST(bang_applies_per_andor_item) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("!", "a", "&&", "b", "||", "!", "c"), &n), NSH_OK);
    ASSERT_EQ(aoi(n, 0)->node->u.pipeline.negate, true);
    ASSERT_EQ(aoi(n, 1)->node->u.pipeline.negate, false);
    ASSERT_EQ(aoi(n, 2)->node->u.pipeline.negate, true);
    ast_free(n);
}

TEST(double_bang_is_a_syntax_error) {
    ASSERT_SYNTAX("!", "!", "a");
}

TEST(bang_alone_is_incomplete) {
    ASSERT_INCOMPLETE("!");
    ASSERT_INCOMPLETE("a", "&&", "!");
}

// A bang in argument position, or as a later stage's name, is just a word.
TEST(bang_away_from_the_front_is_a_word) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("echo", "!"), &n), NSH_OK);
    ASSERT_EQ(cmd_at(n, 0)->words.len, 2);
    ASSERT_STR_EQ(argv_at(n, 0, 1), "!");
    ast_free(n);
}

// Lists

TEST(semicolons_build_a_list) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("a", ";", "b", ";", "c"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_LIST);
    ASSERT_EQ(n->u.list.nodes.len, 3);
    ASSERT_CMD1(lst(n, 0), "a");
    ASSERT_CMD1(lst(n, 2), "c");
    ast_free(n);
}

TEST(newlines_separate_list_items_like_semicolons) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("a", "\n", "b"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_LIST);
    ASSERT_EQ(n->u.list.nodes.len, 2);
    ast_free(n);
}

// One item needs no NODE_LIST wrapper, however many separators surround it.
TEST(a_single_item_is_not_wrapped_in_a_list) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("\n", ";", "a", ";", "\n"), &n), NSH_OK);
    ASSERT_CMD1(n, "a");
    ast_free(n);
}

TEST(a_list_can_hold_andors_and_compounds) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("a", "&&", "b", ";", "while", "c", ";", "do", "d",
                         ";", "done", ";", "e"),
                    &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_LIST);
    ASSERT_EQ(n->u.list.nodes.len, 3);
    ASSERT_KIND(lst(n, 0), NODE_ANDOR);
    ASSERT_KIND(lst(n, 1), NODE_WHILE);
    ASSERT_CMD1(lst(n, 2), "e");
    ast_free(n);
}

// Background

TEST(amp_backgrounds_the_pipeline_it_follows) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("sleep", "5", "&"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_PIPELINE);
    ASSERT_EQ(n->u.pipeline.pl.background, true);
    ast_free(n);
}

TEST(amp_also_separates_list_items) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("a", "&", "b"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_LIST);
    ASSERT_EQ(n->u.list.nodes.len, 2);
    ASSERT_EQ(lst(n, 0)->u.pipeline.pl.background, true);
    ASSERT_EQ(lst(n, 1)->u.pipeline.pl.background, false);
    ast_free(n);
}

TEST(a_negated_pipeline_can_be_backgrounded) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("!", "a", "&"), &n), NSH_OK);
    ASSERT_EQ(n->u.pipeline.negate, true);
    ASSERT_EQ(n->u.pipeline.pl.background, true);
    ast_free(n);
}

TEST(backgrounding_an_andor_chain_is_a_syntax_error) {
    ASSERT_SYNTAX("a", "&&", "b", "&");
    ASSERT_SYNTAX("a", "||", "b", "&");
}

TEST(backgrounding_a_compound_is_a_syntax_error) {
    ASSERT_SYNTAX("if", "a", ";", "then", "b", ";", "fi", "&");
    ASSERT_SYNTAX("while", "a", ";", "do", "b", ";", "done", "&");
    ASSERT_SYNTAX("for", "i", "in", "x", ";", "do", "b", ";", "done", "&");
    ASSERT_SYNTAX("f", "(", ")", "{", "b", ";", "}", "&");
}

// if

TEST(if_then_fi_shape) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("if", "c", ";", "then", "b", ";", "fi"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_IF);
    ASSERT_EQ(n->u.nif.conds.len, 1);
    ASSERT_EQ(n->u.nif.bodies.len, 1);
    ASSERT_TRUE(n->u.nif.else_body == NULL);
    ASSERT_CMD1(ifc(n, 0), "c");
    ASSERT_CMD1(ifb(n, 0), "b");
    ast_free(n);
}

TEST(if_then_else_fi_shape) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("if", "c", ";", "then", "b", ";", "else", "e", ";",
                         "fi"),
                    &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_IF);
    ASSERT_EQ(n->u.nif.conds.len, 1);
    ASSERT_CMD1(n->u.nif.else_body, "e");
    ast_free(n);
}

TEST(elif_chain_without_else) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("if", "c1", ";", "then", "b1", ";", "elif", "c2", ";",
                         "then", "b2", ";", "elif", "c3", ";", "then", "b3",
                         ";", "fi"),
                    &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_IF);
    ASSERT_EQ(n->u.nif.conds.len, 3);
    ASSERT_EQ(n->u.nif.bodies.len, 3);
    ASSERT_TRUE(n->u.nif.else_body == NULL);
    ASSERT_CMD1(ifc(n, 2), "c3");
    ASSERT_CMD1(ifb(n, 2), "b3");
    ast_free(n);
}

TEST(elif_chain_with_else) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("if", "c1", ";", "then", "b1", ";", "elif", "c2", ";",
                         "then", "b2", ";", "else", "e", ";", "fi"),
                    &n),
              NSH_OK);
    ASSERT_EQ(n->u.nif.conds.len, 2);
    ASSERT_CMD1(n->u.nif.else_body, "e");
    ast_free(n);
}

// Newlines stand in for the semicolons everywhere in a compound.
TEST(if_written_over_several_lines) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("if", "c", "\n", "then", "\n", "b", "\n", "else",
                         "\n", "e", "\n", "fi"),
                    &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_IF);
    ASSERT_CMD1(ifb(n, 0), "b");
    ASSERT_CMD1(n->u.nif.else_body, "e");
    ast_free(n);
}

TEST(if_bodies_may_be_lists) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("if", "c", ";", "then", "b1", ";", "b2", ";", "fi"),
                    &n),
              NSH_OK);
    Node *body = vec_get(&n->u.nif.bodies, 0);
    ASSERT_KIND(body, NODE_LIST);
    ASSERT_EQ(body->u.list.nodes.len, 2);
    ast_free(n);
}

TEST(if_truncated_anywhere_is_incomplete) {
    chops_incomplete(nsh_failed,
                     SPEC("if", "c", ";", "then", "b", ";", "elif", "c2", ";",
                          "then", "b2", ";", "else", "e", ";", "fi"),
                     1);
}

TEST(empty_if_bodies_are_syntax_errors) {
    ASSERT_SYNTAX("if", ";", "then", "b", ";", "fi");
    ASSERT_SYNTAX("if", "c", ";", "then", ";", "fi");
    ASSERT_SYNTAX("if", "c", ";", "then", "b", ";", "else", ";", "fi");
    ASSERT_SYNTAX("if", "c", ";", "then", "b", ";", "elif", ";", "then", "b2",
                  ";", "fi");
}

TEST(mismatched_if_keywords_are_syntax_errors) {
    ASSERT_SYNTAX("if", "c", ";", "do", "b", ";", "fi");
    ASSERT_SYNTAX("if", "c", ";", "then", "b", ";", "done");
    ASSERT_SYNTAX("fi");
    ASSERT_SYNTAX("then", "a");
    ASSERT_SYNTAX("else", "a");
    ASSERT_SYNTAX("elif", "a");
    ASSERT_SYNTAX("a", ";", "fi");
}

// while

TEST(while_do_done_shape) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("while", "c", ";", "do", "b", ";", "done"), &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_WHILE);
    ASSERT_CMD1(n->u.nwhile.cond, "c");
    ASSERT_CMD1(n->u.nwhile.body, "b");
    ast_free(n);
}

TEST(while_truncated_anywhere_is_incomplete) {
    chops_incomplete(nsh_failed,
                     SPEC("while", "c", ";", "do", "b", ";", "done"), 1);
}

TEST(while_errors) {
    ASSERT_SYNTAX("while", ";", "do", "b", ";", "done");
    ASSERT_SYNTAX("while", "c", ";", "do", ";", "done");
    ASSERT_SYNTAX("while", "c", ";", "then", "b", ";", "done");
    ASSERT_SYNTAX("while", "c", ";", "do", "b", ";", "fi");
    ASSERT_SYNTAX("done");
    ASSERT_SYNTAX("do", "a");
}

// for

TEST(for_with_words) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("for", "i", "in", "x", "y", ";", "do", "b", ";",
                         "done"),
                    &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_FOR);
    ASSERT_STR_EQ(n->u.nfor.var, "i");
    ASSERT_EQ(n->u.nfor.words.len, 2);
    ASSERT_STR_EQ(word_text(vec_get(&n->u.nfor.words, 0)), "x");
    ASSERT_STR_EQ(word_text(vec_get(&n->u.nfor.words, 1)), "y");
    ASSERT_CMD1(n->u.nfor.body, "b");
    ast_free(n);
}

TEST(for_with_zero_words) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("for", "i", "in", ";", "do", "b", ";", "done"), &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_FOR);
    ASSERT_EQ(n->u.nfor.words.len, 0);
    ast_free(n);
}

TEST(for_with_newlines_instead_of_semicolons) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("for", "i", "\n", "in", "x", "\n", "do", "b", "\n",
                         "done"),
                    &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_FOR);
    ASSERT_EQ(n->u.nfor.words.len, 1);
    ast_free(n);
}

// Keyword words are ordinary words in the for word list.
TEST(for_words_may_look_like_keywords) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("for", "i", "in", "if", "fi", ";", "do", "b", ";",
                         "done"),
                    &n),
              NSH_OK);
    ASSERT_EQ(n->u.nfor.words.len, 2);
    ASSERT_STR_EQ(word_text(vec_get(&n->u.nfor.words, 0)), "if");
    ast_free(n);
}

TEST(for_truncated_anywhere_is_incomplete) {
    chops_incomplete(
        nsh_failed,
        SPEC("for", "i", "in", "x", ";", "do", "b", ";", "done"), 1);
}

TEST(for_needs_a_valid_name) {
    ASSERT_SYNTAX("for", "1i", "in", "x", ";", "do", "b", ";", "done");
    ASSERT_SYNTAX("for", "a-b", "in", "x", ";", "do", "b", ";", "done");
    ASSERT_SYNTAX("for", "a.b", "in", "x", ";", "do", "b", ";", "done");
    ASSERT_SYNTAX("for", ";", "do", "b", ";", "done");
}

TEST(for_accepts_underscores_and_digits_after_the_first_char) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("for", "_a9Z", "in", "x", ";", "do", "b", ";",
                         "done"),
                    &n),
              NSH_OK);
    ASSERT_STR_EQ(n->u.nfor.var, "_a9Z");
    ast_free(n);
}

TEST(for_errors) {
    ASSERT_SYNTAX("for", "i", "x", ";", "do", "b", ";", "done");
    ASSERT_SYNTAX("for", "i", "in", "x", ";", "then", "b", ";", "done");
    ASSERT_SYNTAX("for", "i", "in", "x", ";", "do", ";", "done");
    ASSERT_SYNTAX("for", "i", "in", "x", "|", "do", "b", ";", "done");
}

// Function definitions

TEST(funcdef_shape) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("f", "(", ")", "{", "b", ";", "}"), &n), NSH_OK);
    ASSERT_KIND(n, NODE_FUNCDEF);
    ASSERT_STR_EQ(n->u.funcdef.name, "f");
    ASSERT_CMD1(n->u.funcdef.body, "b");
    ast_free(n);
}

TEST(funcdef_allows_newlines_before_the_brace) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("f", "(", ")", "\n", "{", "\n", "b", "\n", "}"), &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_FUNCDEF);
    ASSERT_CMD1(n->u.funcdef.body, "b");
    ast_free(n);
}

TEST(funcdef_body_may_be_a_list) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("f", "(", ")", "{", "b1", ";", "b2", ";", "}"), &n),
              NSH_OK);
    Node *body = n->u.funcdef.body;
    ASSERT_KIND(body, NODE_LIST);
    ASSERT_EQ(body->u.list.nodes.len, 2);
    ast_free(n);
}

// The first token alone is a plain command, so the sweep starts at two.
TEST(funcdef_truncated_anywhere_is_incomplete) {
    chops_incomplete(nsh_failed, SPEC("f", "(", ")", "{", "b", ";", "}"), 2);
}

TEST(funcdef_needs_a_valid_name) {
    ASSERT_SYNTAX("1f", "(", ")", "{", "b", ";", "}");
    ASSERT_SYNTAX("a-b", "(", ")", "{", "b", ";", "}");
    ASSERT_SYNTAX("f/g", "(", ")", "{", "b", ";", "}");
}

TEST(funcdef_errors) {
    ASSERT_SYNTAX("f", "(", "x", ")", "{", "b", ";", "}");
    ASSERT_SYNTAX("f", "(", ")", "b", ";");
    ASSERT_SYNTAX("f", "(", ")", "{", ";", "}");
    ASSERT_SYNTAX("}");
    ASSERT_SYNTAX("{", "b", ";", "}");
}

// A closing brace needs command position, so it must follow a separator.
TEST(a_brace_glued_to_the_body_is_swallowed_as_a_word) {
    ASSERT_INCOMPLETE("f", "(", ")", "{", "b", "}");
}

// Keywords in argument position

TEST(keywords_are_ordinary_words_after_the_command_name) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("echo", "if", "then", "fi", "while", "done", "for",
                         "in", "{", "}"),
                    &n),
              NSH_OK);
    ASSERT_EQ(cmd_at(n, 0)->words.len, 10);
    ASSERT_STR_EQ(argv_at(n, 0, 1), "if");
    ASSERT_STR_EQ(argv_at(n, 0, 9), "}");
    ast_free(n);
}

TEST(keywords_are_words_as_redirect_targets_and_for_words) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("echo", ">", "fi"), &n), NSH_OK);
    ASSERT_STR_EQ(word_text(cmd_at(n, 0)->redir_out), "fi");
    ast_free(n);
}

// A keyword must be exactly one segment, so a split word is a plain word.
TEST(a_two_segment_keyword_is_not_a_keyword) {
    TokenList tl;
    vec_init(&tl.tokens);
    vec_push(&tl.tokens, seg_add(mk_word("i"), "f"));
    vec_push(&tl.tokens, mk_word("x"));
    Node *n = NULL;
    ASSERT_EQ(parse(tl, &n), NSH_OK);
    ASSERT_KIND(n, NODE_PIPELINE);
    ASSERT_STR_EQ(argv_at(n, 0, 0), "if");
    ASSERT_STR_EQ(argv_at(n, 0, 1), "x");
    ast_free(n);
}

TEST(a_reserved_word_cannot_open_a_simple_command) {
    ASSERT_SYNTAX("a", "|", "fi");
    ASSERT_SYNTAX("a", "&&", "done");
    ASSERT_SYNTAX("!", "fi");
}

// Compound isolation

TEST(a_compound_cannot_be_piped_into) {
    ASSERT_SYNTAX("echo", "|", "if", "c", ";", "then", "b", ";", "fi");
    ASSERT_SYNTAX("echo", "|", "while", "c", ";", "do", "b", ";", "done");
    ASSERT_SYNTAX("echo", "|", "for", "i", "in", "x", ";", "do", "b", ";",
                  "done");
}

TEST(a_compound_cannot_be_piped_out_of) {
    ASSERT_SYNTAX("if", "c", ";", "then", "b", ";", "fi", "|", "grep", "x");
    ASSERT_SYNTAX("while", "c", ";", "do", "b", ";", "done", "|", "grep");
}

TEST(a_compound_takes_no_redirections) {
    ASSERT_SYNTAX("if", "c", ";", "then", "b", ";", "fi", ">", "out");
    ASSERT_SYNTAX("if", "c", ";", "then", "b", ";", "fi", "<", "in");
    ASSERT_SYNTAX("for", "i", "in", "x", ";", "do", "b", ";", "done", "2>",
                  "e");
}

TEST(a_compound_does_not_chain_with_and_or) {
    ASSERT_SYNTAX("if", "c", ";", "then", "b", ";", "fi", "&&", "echo");
    ASSERT_SYNTAX("if", "c", ";", "then", "b", ";", "fi", "||", "echo");
    ASSERT_SYNTAX("a", "&&", "if", "c", ";", "then", "b", ";", "fi");
}

TEST(a_compound_sequences_with_semicolons_and_newlines) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("if", "c", ";", "then", "b", ";", "fi", ";", "echo",
                         "\n", "while", "c", ";", "do", "b", ";", "done"),
                    &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_LIST);
    ASSERT_EQ(n->u.list.nodes.len, 3);
    ASSERT_KIND(lst(n, 0), NODE_IF);
    ASSERT_CMD1(lst(n, 1), "echo");
    ASSERT_KIND(lst(n, 2), NODE_WHILE);
    ast_free(n);
}

// Nesting

TEST(compounds_nest_freely) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("f", "(", ")", "{", "while", "c", ";", "do", "if",
                         "c2", ";", "then", "for", "i", "in", "x", ";", "do",
                         "deep", ";", "done", ";", "fi", ";", "done", ";",
                         "}"),
                    &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_FUNCDEF);
    Node *w = n->u.funcdef.body;
    ASSERT_KIND(w, NODE_WHILE);
    Node *i = w->u.nwhile.body;
    ASSERT_KIND(i, NODE_IF);
    Node *f = vec_get(&i->u.nif.bodies, 0);
    ASSERT_KIND(f, NODE_FOR);
    ASSERT_STR_EQ(f->u.nfor.var, "i");
    ASSERT_CMD1(f->u.nfor.body, "deep");
    ast_free(n);
}

TEST(a_nested_compound_truncated_anywhere_is_incomplete) {
    chops_incomplete(nsh_failed,
                     SPEC("while", "c", ";", "do", "if", "c2", ";", "then",
                          "b", ";", "fi", ";", "done"),
                     1);
}

TEST(an_if_nested_in_an_if_condition) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("if", "if", "a", ";", "then", "b", ";", "fi", ";",
                         "then", "c", ";", "fi"),
                    &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_IF);
    ASSERT_KIND(ifc(n, 0), NODE_IF);
    ast_free(n);
}

// Ownership

TEST(the_token_list_is_left_valid_and_empty_on_success) {
    TokenList tl = TOKS("a", "b", "|", "c", ">", "d", "&");
    ASSERT_EQ(tl.tokens.len, 7);
    Node *n = NULL;
    ASSERT_EQ(parser_parse_program(&tl, &n), NSH_OK);
    ASSERT_EQ(tl.tokens.len, 0);
    ASSERT_TRUE(tl.tokens.items != NULL);
    ASSERT_KIND(n, NODE_PIPELINE);
    ast_free(n);
    token_list_free(&tl);
}

// The tokens after the failure point have to be released too.
TEST(the_token_list_is_left_valid_and_empty_on_error) {
    TokenList tl = TOKS("a", "|", "|", "b", "c", "d", ">", "e");
    Node *n = (Node *)0x1;
    ASSERT_EQ(parser_parse_program(&tl, &n), NSH_ERR_SYNTAX);
    ASSERT_TRUE(n == NULL);
    ASSERT_EQ(tl.tokens.len, 0);
    ASSERT_TRUE(tl.tokens.items != NULL);
    token_list_free(&tl);
}

// A failure deep inside a compound must take the half-built tree with it.
TEST(a_late_failure_frees_everything_built_so_far) {
    ASSERT_SYNTAX("a", "&&", "b", ";", "while", "c", ";", "do", "x", "|", "y",
                  ";", "if", "q", ";", "then", "z", "<", "i", "<", "j", ";",
                  "fi", ";", "done");
    ASSERT_INCOMPLETE("f", "(", ")", "{", "while", "c", ";", "do", "if", "d",
                      ";", "then", "e");
}

TEST(a_torture_program_mixes_every_rule) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("<", "in", "ls", "-l", "|", "2>", "err", "grep", "x",
                         ">>", "out", "&&", "!", "wc", "-l", ";", "for", "i",
                         "in", "a", "b", "\n", "do", "echo", "$i", "&", "done",
                         "\n", "bg", "&"),
                    &n),
              NSH_OK);
    ASSERT_KIND(n, NODE_LIST);
    ASSERT_EQ(n->u.list.nodes.len, 3);
    Node *chain = lst(n, 0);
    ASSERT_KIND(chain, NODE_ANDOR);
    ASSERT_EQ(chain->u.andor.items.len, 2);
    Node *left = aoi(chain, 0)->node;
    ASSERT_EQ(left->u.pipeline.pl.cmds.len, 2);
    ASSERT_STR_EQ(word_text(cmd_at(left, 0)->redir_in), "in");
    ASSERT_STR_EQ(word_text(cmd_at(left, 1)->redir_out), "out");
    ASSERT_EQ(cmd_at(left, 1)->redir_append, true);
    ASSERT_STR_EQ(word_text(cmd_at(left, 1)->redir_err), "err");
    ASSERT_EQ(aoi(chain, 1)->node->u.pipeline.negate, true);
    Node *loop = lst(n, 1);
    ASSERT_KIND(loop, NODE_FOR);
    ASSERT_EQ(loop->u.nfor.words.len, 2);
    ASSERT_EQ(loop->u.nfor.body->u.pipeline.pl.background, true);
    ASSERT_EQ(lst(n, 2)->u.pipeline.pl.background, true);
    ast_free(n);
}

// The legacy single-pipeline entry

static Pipeline pl_zero(void) {
    Pipeline p;
    p.cmds.items = NULL;
    p.cmds.len = 0;
    p.cmds.cap = 0;
    p.background = false;
    return p;
}

// A rejected line must leave out usable, not merely non-crashing.
#define ASSERT_EMPTY_PIPELINE(plp)                                            \
    do {                                                                      \
        ASSERT_EQ((plp)->cmds.len, 0);                                        \
        ASSERT_TRUE((plp)->cmds.items != NULL);                               \
        ASSERT_TRUE((plp)->background == false);                              \
    } while (0)

static NshError legacy(TokenList tl, Pipeline *out) {
    NshError err = parser_parse(&tl, out);
    token_list_free(&tl);
    return err;
}

TEST(legacy_accepts_a_plain_pipeline) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(legacy(TOKS("<", "i", "ls", "-l", "|", "wc", ">", "o"), &pl),
              NSH_OK);
    ASSERT_EQ(pl.cmds.len, 2);
    ASSERT_EQ(pl.background, false);
    Command *c = vec_get(&pl.cmds, 0);
    ASSERT_EQ(c->words.len, 2);
    ASSERT_STR_EQ(word_text(vec_get(&c->words, 0)), "ls");
    ASSERT_STR_EQ(word_text(c->redir_in), "i");
    ASSERT_STR_EQ(word_text(((Command *)vec_get(&pl.cmds, 1))->redir_out), "o");
    pipeline_free(&pl);
}

TEST(legacy_accepts_a_background_pipeline) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(legacy(TOKS("sleep", "5", "&"), &pl), NSH_OK);
    ASSERT_EQ(pl.cmds.len, 1);
    ASSERT_TRUE(pl.background == true);
    pipeline_free(&pl);
}

TEST(legacy_accepts_an_empty_line) {
    Pipeline pl = pl_zero();
    TokenList tl;
    vec_init(&tl.tokens);
    ASSERT_EQ(legacy(tl, &pl), NSH_OK);
    ASSERT_EMPTY_PIPELINE(&pl);
    pipeline_free(&pl);
}

TEST(legacy_rejects_anything_but_one_pipeline) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(legacy(TOKS("a", "&&", "b"), &pl), NSH_ERR_SYNTAX);
    ASSERT_EMPTY_PIPELINE(&pl);
    ASSERT_EQ(legacy(TOKS("a", ";", "b"), &pl), NSH_ERR_SYNTAX);
    ASSERT_EMPTY_PIPELINE(&pl);
    ASSERT_EQ(legacy(TOKS("!", "a"), &pl), NSH_ERR_SYNTAX);
    ASSERT_EMPTY_PIPELINE(&pl);
    ASSERT_EQ(legacy(TOKS("if", "c", ";", "then", "b", ";", "fi"), &pl),
              NSH_ERR_SYNTAX);
    ASSERT_EMPTY_PIPELINE(&pl);
    ASSERT_EQ(legacy(TOKS("f", "(", ")", "{", "b", ";", "}"), &pl),
              NSH_ERR_SYNTAX);
    ASSERT_EMPTY_PIPELINE(&pl);
    pipeline_free(&pl);
}

// Unfinished input has no continuation prompt down here, so it is an error.
TEST(legacy_turns_incomplete_into_syntax) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(legacy(TOKS("a", "|"), &pl), NSH_ERR_SYNTAX);
    ASSERT_EMPTY_PIPELINE(&pl);
    ASSERT_EQ(legacy(TOKS("echo", ">"), &pl), NSH_ERR_SYNTAX);
    ASSERT_EMPTY_PIPELINE(&pl);
    pipeline_free(&pl);
}

TEST(legacy_reuses_the_pipeline_across_calls) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(legacy(TOKS("a", "b", "|", "c"), &pl), NSH_OK);
    ASSERT_EQ(pl.cmds.len, 2);
    ASSERT_EQ(legacy(TOKS("solo", "&"), &pl), NSH_OK);
    ASSERT_EQ(pl.cmds.len, 1);
    ASSERT_TRUE(pl.background == true);
    ASSERT_EQ(legacy(TOKS("a", "|"), &pl), NSH_ERR_SYNTAX);
    ASSERT_EMPTY_PIPELINE(&pl);
    ASSERT_EQ(legacy(TOKS("ok"), &pl), NSH_OK);
    ASSERT_EQ(pl.cmds.len, 1);
    pipeline_free(&pl);
}

TEST(pipeline_free_is_safe_twice_and_on_null) {
    Pipeline pl = pl_zero();
    ASSERT_EQ(legacy(TOKS("a", "<", "i", ">", "o", "2>", "e", "|", "b"), &pl),
              NSH_OK);
    pipeline_free(&pl);
    pipeline_free(&pl);
    ASSERT_EQ(pl.cmds.len, 0);
    pipeline_free(NULL);
}

TEST(ast_clone_survives_a_parsed_tree) {
    Node *n = NULL;
    ASSERT_EQ(parse(TOKS("if", "c", ";", "then", "for", "i", "in", "x", ";",
                         "do", "a", "|", "b", ";", "done", ";", "fi"),
                    &n),
              NSH_OK);
    Node *copy = ast_clone(n);
    ast_free(n);
    ASSERT_KIND(copy, NODE_IF);
    Node *loop = vec_get(&copy->u.nif.bodies, 0);
    ASSERT_KIND(loop, NODE_FOR);
    ASSERT_STR_EQ(loop->u.nfor.var, "i");
    ASSERT_EQ(loop->u.nfor.body->u.pipeline.pl.cmds.len, 2);
    ast_free(copy);
}

TEST_MAIN()
