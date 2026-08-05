// Tests for the evaluator, driven by trees built node by node.

#define _POSIX_C_SOURCE 200809L

#include "eval.h"

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "func.h"
#include "jobs.h"
#include "signals.h"

#include "../alloc/alloc.h"
#include "../util/vec.h"
#include "../../tests/harness.h"

// A fixed template plus a short name, so snprintf never looks truncated.
#define TMP_BUF 64
#define PATH_BUF 128
#define CMD_BUF 512
#define READ_BUF 1024

// eval.c caps function recursion here; the test pokes the counter to the edge.
#define DEPTH_CAP 64

static Shell g_sh;
static char g_tmp[TMP_BUF];
static NshError g_err;

static void setup(void) {
    history_init(&g_sh.history, 16);
    g_sh.last_status = 0;
    g_sh.want_exit = false;
    g_sh.exit_code = 0;
    g_sh.interactive = false;
    g_sh.tty_fd = -1;
    g_sh.shell_pgid = getpgrp();
    g_sh.flow = FLOW_NONE;
    g_sh.flow_status = 0;
    g_sh.loop_depth = 0;
    g_sh.func_depth = 0;
    g_sh.argc = 0;
    g_sh.argv = NULL;
    jobs_init();
    // The loop tests need SIGINT ignored by default and a handler to install.
    signals_install_shell();
    snprintf(g_tmp, sizeof g_tmp, "/tmp/nsh_eval_XXXXXX");
    if (mkdtemp(g_tmp) == NULL) {
        fprintf(stderr, "eval_test: mkdtemp failed\n");
        exit(1);
    }
}

static void teardown(void) {
    char cmd[CMD_BUF];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_tmp);
    if (system(cmd) != 0) {
        fprintf(stderr, "eval_test: could not remove %s\n", g_tmp);
    }
    history_free(&g_sh.history);
    jobs_free_all();
    func_free_all();
}

static void scratch(char *buf, size_t size, const char *name) {
    snprintf(buf, size, "%s/%s", g_tmp, name);
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void read_file(const char *path, char *buf, size_t size) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
}

// Missing counts as -1, so "never created" and "empty" stay distinguishable.
static int count_lines(const char *path) {
    if (!file_exists(path)) {
        return -1;
    }
    char body[READ_BUF];
    read_file(path, body, sizeof body);
    int n = 0;
    for (size_t i = 0; body[i] != '\0'; i++) {
        if (body[i] == '\n') {
            n++;
        }
    }
    return n;
}

static void make_empty(const char *path) {
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fclose(f);
    }
}

static int stderr_off(void) {
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }
    return saved;
}

static void stderr_on(int saved) {
    fflush(stderr);
    if (saved >= 0) {
        dup2(saved, STDERR_FILENO);
        close(saved);
    }
}

// Tree builders

static char *dup_cstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *out = nsh_malloc(n);
    memcpy(out, s, n);
    return out;
}

static Token *mk_word(const char *text, bool expand) {
    Token *t = nsh_malloc(sizeof(*t));
    t->kind = TOK_WORD;
    vec_init(&t->segs);
    WordSeg *seg = nsh_malloc(sizeof(*seg));
    seg->text = dup_cstr(text);
    seg->expand = expand;
    vec_push(&t->segs, seg);
    return t;
}

static Node *build_pipe(bool expand, const char *first, va_list ap) {
    Command *c = nsh_calloc(1, sizeof(*c));
    vec_init(&c->words);
    vec_push(&c->words, mk_word(first, expand));
    for (const char *w = va_arg(ap, const char *); w != NULL;
         w = va_arg(ap, const char *)) {
        vec_push(&c->words, mk_word(w, expand));
    }
    Node *n = ast_node_new(NODE_PIPELINE);
    vec_push(&n->u.pipeline.pl.cmds, c);
    return n;
}

// Words the expander sees, exactly like unquoted text from the lexer.
static Node *mk_pipe(const char *first, ...) {
    va_list ap;
    va_start(ap, first);
    Node *n = build_pipe(true, first, ap);
    va_end(ap);
    return n;
}

// Literal words, exactly like single quoted text: a $ survives to the child.
static Node *mk_pipe_lit(const char *first, ...) {
    va_list ap;
    va_start(ap, first);
    Node *n = build_pipe(false, first, ap);
    va_end(ap);
    return n;
}

static Node *mk_neg(Node *pipeline) {
    pipeline->u.pipeline.negate = true;
    return pipeline;
}

static void andor_add(Node *n, AndOrOp op, Node *item) {
    AndOrItem *it = nsh_malloc(sizeof(*it));
    it->op = op;
    it->node = item;
    vec_push(&n->u.andor.items, it);
}

static Node *mk_andor(Node *first) {
    Node *n = ast_node_new(NODE_ANDOR);
    andor_add(n, ANDOR_FIRST, first);
    return n;
}

static Node *mk_list(Node *first, ...) {
    Node *n = ast_node_new(NODE_LIST);
    va_list ap;
    va_start(ap, first);
    for (Node *item = first; item != NULL; item = va_arg(ap, Node *)) {
        vec_push(&n->u.list.nodes, item);
    }
    va_end(ap);
    return n;
}

static void if_add(Node *n, Node *cond, Node *body) {
    vec_push(&n->u.nif.conds, cond);
    vec_push(&n->u.nif.bodies, body);
}

static Node *mk_while(Node *cond, Node *body) {
    Node *n = ast_node_new(NODE_WHILE);
    n->u.nwhile.cond = cond;
    n->u.nwhile.body = body;
    return n;
}

static Node *mk_for(const char *var, Node *body) {
    Node *n = ast_node_new(NODE_FOR);
    n->u.nfor.var = dup_cstr(var);
    n->u.nfor.body = body;
    return n;
}

static void for_add(Node *n, const char *text, bool expand) {
    vec_push(&n->u.nfor.words, mk_word(text, expand));
}

static Node *mk_funcdef(const char *name, Node *body) {
    Node *n = ast_node_new(NODE_FUNCDEF);
    n->u.funcdef.name = dup_cstr(name);
    n->u.funcdef.body = body;
    return n;
}

// One shell command as a pipeline, the way most bodies here are written.
static Node *mk_sh(const char *cmd) {
    return mk_pipe_lit("/bin/sh", "-c", cmd, NULL);
}

// Takes ownership of the tree and hands back $?.
static int run_tree(Node *n) {
    g_err = eval_run(&g_sh, n);
    ast_free(n);
    return g_sh.last_status;
}

// A condition that stays true until path holds limit lines.
static Node *mk_counter_cond(const char *path, int limit) {
    char cmd[CMD_BUF];
    snprintf(cmd, sizeof cmd, "test $(wc -l < %s) -lt %d", path, limit);
    return mk_sh(cmd);
}

static Node *mk_appender(const char *path) {
    char cmd[CMD_BUF];
    snprintf(cmd, sizeof cmd, "echo x >> %s", path);
    return mk_sh(cmd);
}

static Node *mk_marker(const char *path) {
    char cmd[CMD_BUF];
    snprintf(cmd, sizeof cmd, "echo x > %s", path);
    return mk_sh(cmd);
}

// Entry points

TEST(an_empty_program_is_a_no_op) {
    g_sh.last_status = 5;
    ASSERT_EQ(eval_run(&g_sh, NULL), NSH_OK);
    ASSERT_EQ(g_sh.last_status, 5);
    g_sh.last_status = 0;
}

TEST(a_null_shell_is_rejected) {
    ASSERT_EQ(eval_run(NULL, NULL), NSH_ERR_INVALID);
}

TEST(a_bare_pipeline_runs_and_sets_the_status) {
    ASSERT_EQ(run_tree(mk_pipe("/bin/true", NULL)), 0);
    ASSERT_EQ(g_err, NSH_OK);
    ASSERT_EQ(run_tree(mk_pipe("/bin/false", NULL)), 1);
}

// And-or

TEST(negate_inverts_the_pipeline_status) {
    ASSERT_EQ(run_tree(mk_neg(mk_pipe("/bin/true", NULL))), 1);
    ASSERT_EQ(run_tree(mk_neg(mk_pipe("/bin/false", NULL))), 0);
}

TEST(and_runs_the_right_side_when_the_left_succeeds) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "and_ran");
    Node *n = mk_andor(mk_pipe("/bin/true", NULL));
    andor_add(n, ANDOR_AND, mk_marker(marker));
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_TRUE(file_exists(marker));
}

TEST(and_skips_the_right_side_when_the_left_fails) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "and_skipped");
    Node *n = mk_andor(mk_pipe("/bin/false", NULL));
    andor_add(n, ANDOR_AND, mk_marker(marker));
    ASSERT_EQ(run_tree(n), 1);
    ASSERT_TRUE(!file_exists(marker));
}

TEST(or_skips_the_right_side_when_the_left_succeeds) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "or_skipped");
    Node *n = mk_andor(mk_pipe("/bin/true", NULL));
    andor_add(n, ANDOR_OR, mk_marker(marker));
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_TRUE(!file_exists(marker));
}

TEST(or_runs_the_right_side_when_the_left_fails) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "or_ran");
    Node *n = mk_andor(mk_pipe("/bin/false", NULL));
    andor_add(n, ANDOR_OR, mk_marker(marker));
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_TRUE(file_exists(marker));
}

// A skipped item never touches $?, so the last real command owns it.
TEST(the_status_comes_from_the_last_command_actually_run) {
    Node *a = mk_andor(mk_pipe("/bin/true", NULL));
    andor_add(a, ANDOR_AND, mk_pipe("/bin/false", NULL));
    ASSERT_EQ(run_tree(a), 1);

    Node *b = mk_andor(mk_pipe("/bin/false", NULL));
    andor_add(b, ANDOR_AND, mk_pipe("/bin/true", NULL));
    ASSERT_EQ(run_tree(b), 1);

    Node *c = mk_andor(mk_pipe("/bin/false", NULL));
    andor_add(c, ANDOR_AND, mk_pipe("/bin/true", NULL));
    andor_add(c, ANDOR_OR, mk_pipe("/bin/true", NULL));
    ASSERT_EQ(run_tree(c), 0);
}

TEST(a_negated_item_chains_on_its_inverted_status) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "neg_chain");
    Node *n = mk_andor(mk_neg(mk_pipe("/bin/false", NULL)));
    andor_add(n, ANDOR_AND, mk_marker(marker));
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_TRUE(file_exists(marker));
}

// Lists

TEST(a_list_runs_its_items_in_order) {
    char out[PATH_BUF];
    char one[CMD_BUF];
    char two[CMD_BUF];
    scratch(out, sizeof out, "list_order");
    snprintf(one, sizeof one, "echo a >> %s", out);
    snprintf(two, sizeof two, "echo b >> %s", out);
    ASSERT_EQ(run_tree(mk_list(mk_sh(one), mk_sh(two), NULL)), 0);

    char body[READ_BUF];
    read_file(out, body, sizeof body);
    ASSERT_STR_EQ(body, "a\nb\n");
}

TEST(a_list_ends_with_the_last_items_status) {
    ASSERT_EQ(run_tree(mk_list(mk_pipe("/bin/true", NULL),
                               mk_pipe("/bin/false", NULL), NULL)), 1);
    ASSERT_EQ(run_tree(mk_list(mk_pipe("/bin/false", NULL),
                               mk_pipe("/bin/true", NULL), NULL)), 0);
}

TEST(a_list_stops_at_want_exit) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "after_exit");
    Node *n = mk_list(mk_pipe("exit", "3", NULL), mk_marker(marker), NULL);
    ASSERT_EQ(run_tree(n), 3);
    ASSERT_TRUE(g_sh.want_exit);
    ASSERT_EQ(g_sh.exit_code, 3);
    ASSERT_TRUE(!file_exists(marker));
    g_sh.want_exit = false;
    g_sh.exit_code = 0;
}

// A pending flow flag is the loop's business, so the list only stops.
TEST(a_list_stops_at_a_pending_flow_flag) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "after_flow");
    g_sh.flow = FLOW_BREAK;
    run_tree(mk_list(mk_marker(marker), mk_pipe("/bin/true", NULL), NULL));
    ASSERT_TRUE(!file_exists(marker));
    ASSERT_EQ(g_sh.flow, FLOW_BREAK);
    g_sh.flow = FLOW_NONE;
}

// If

TEST(if_takes_the_first_true_branch) {
    char taken[PATH_BUF];
    char other[PATH_BUF];
    scratch(taken, sizeof taken, "if_taken");
    scratch(other, sizeof other, "if_other");
    Node *n = ast_node_new(NODE_IF);
    if_add(n, mk_pipe("/bin/true", NULL), mk_marker(taken));
    if_add(n, mk_pipe("/bin/true", NULL), mk_marker(other));
    n->u.nif.else_body = mk_marker(other);
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_TRUE(file_exists(taken));
    ASSERT_TRUE(!file_exists(other));
}

TEST(if_falls_through_to_the_elif) {
    char taken[PATH_BUF];
    char other[PATH_BUF];
    scratch(taken, sizeof taken, "elif_taken");
    scratch(other, sizeof other, "elif_other");
    Node *n = ast_node_new(NODE_IF);
    if_add(n, mk_pipe("/bin/false", NULL), mk_marker(other));
    if_add(n, mk_pipe("/bin/true", NULL), mk_marker(taken));
    n->u.nif.else_body = mk_marker(other);
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_TRUE(file_exists(taken));
    ASSERT_TRUE(!file_exists(other));
}

TEST(if_falls_through_to_the_else) {
    char taken[PATH_BUF];
    scratch(taken, sizeof taken, "else_taken");
    Node *n = ast_node_new(NODE_IF);
    if_add(n, mk_pipe("/bin/false", NULL), mk_pipe("/bin/true", NULL));
    n->u.nif.else_body = mk_marker(taken);
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_TRUE(file_exists(taken));
}

TEST(the_chosen_body_owns_the_status) {
    Node *n = ast_node_new(NODE_IF);
    if_add(n, mk_pipe("/bin/true", NULL), mk_pipe("/bin/false", NULL));
    ASSERT_EQ(run_tree(n), 1);
}

TEST(an_if_with_no_branch_taken_is_zero) {
    ASSERT_EQ(run_tree(mk_pipe("/bin/false", NULL)), 1);
    Node *n = ast_node_new(NODE_IF);
    if_add(n, mk_pipe("/bin/false", NULL), mk_pipe("/bin/false", NULL));
    ASSERT_EQ(run_tree(n), 0);
}

// While

TEST(while_runs_until_the_condition_fails) {
    char count[PATH_BUF];
    scratch(count, sizeof count, "while_count");
    make_empty(count);
    Node *n = mk_while(mk_counter_cond(count, 3), mk_appender(count));
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_EQ(count_lines(count), 3);
    ASSERT_EQ(g_sh.loop_depth, 0);
}

TEST(a_while_whose_body_never_runs_is_zero) {
    ASSERT_EQ(run_tree(mk_pipe("/bin/false", NULL)), 1);
    Node *n = mk_while(mk_pipe("/bin/false", NULL), mk_pipe("/bin/false", NULL));
    ASSERT_EQ(run_tree(n), 0);
}

TEST(the_last_body_iteration_owns_the_while_status) {
    char count[PATH_BUF];
    char cmd[CMD_BUF];
    scratch(count, sizeof count, "while_status");
    make_empty(count);
    snprintf(cmd, sizeof cmd, "echo x >> %s; exit 4", count);
    Node *n = mk_while(mk_counter_cond(count, 2), mk_sh(cmd));
    ASSERT_EQ(run_tree(n), 4);
    ASSERT_EQ(count_lines(count), 2);
}

// The real break builtin is the exec agent's; here the flag stands in for it.
TEST(a_while_consumes_break_and_leaves_with_zero) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "while_break");
    g_sh.flow = FLOW_BREAK;
    Node *n = mk_while(mk_pipe("/bin/true", NULL), mk_marker(marker));
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_EQ(g_sh.flow, FLOW_NONE);
    ASSERT_TRUE(!file_exists(marker));
    ASSERT_EQ(g_sh.loop_depth, 0);
}

// Continue is consumed and the loop keeps going, so only one pass is skipped.
TEST(a_while_consumes_continue_and_keeps_looping) {
    char count[PATH_BUF];
    scratch(count, sizeof count, "while_continue");
    make_empty(count);
    g_sh.flow = FLOW_CONTINUE;
    Node *n = mk_while(mk_counter_cond(count, 2), mk_appender(count));
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_EQ(g_sh.flow, FLOW_NONE);
    ASSERT_EQ(count_lines(count), 2);
}

TEST(a_while_leaves_return_for_the_caller) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "while_return");
    g_sh.flow = FLOW_RETURN;
    g_sh.flow_status = 7;
    run_tree(mk_while(mk_pipe("/bin/true", NULL), mk_marker(marker)));
    ASSERT_EQ(g_sh.flow, FLOW_RETURN);
    ASSERT_EQ(g_sh.flow_status, 7);
    ASSERT_TRUE(!file_exists(marker));
    ASSERT_EQ(g_sh.loop_depth, 0);
    g_sh.flow = FLOW_NONE;
    g_sh.flow_status = 0;
}

TEST(a_while_stops_at_want_exit) {
    char count[PATH_BUF];
    char cmd[CMD_BUF];
    scratch(count, sizeof count, "while_exit");
    make_empty(count);
    snprintf(cmd, sizeof cmd, "echo x >> %s", count);
    Node *body = mk_list(mk_sh(cmd), mk_pipe("exit", "6", NULL), NULL);
    ASSERT_EQ(run_tree(mk_while(mk_counter_cond(count, 5), body)), 6);
    ASSERT_TRUE(g_sh.want_exit);
    ASSERT_EQ(count_lines(count), 1);
    ASSERT_EQ(g_sh.loop_depth, 0);
    g_sh.want_exit = false;
    g_sh.exit_code = 0;
}

TEST(loops_nest) {
    char count[PATH_BUF];
    scratch(count, sizeof count, "nest_count");
    make_empty(count);
    Node *inner = mk_while(mk_counter_cond(count, 4), mk_appender(count));
    Node *outer = mk_while(mk_counter_cond(count, 4), inner);
    ASSERT_EQ(run_tree(outer), 0);
    ASSERT_EQ(count_lines(count), 4);
    ASSERT_EQ(g_sh.loop_depth, 0);
}

// For

TEST(for_iterates_over_its_words) {
    char out[PATH_BUF];
    char cmd[CMD_BUF];
    scratch(out, sizeof out, "for_out");
    snprintf(cmd, sizeof cmd, "echo $NSH_T_FV >> %s", out);
    Node *n = mk_for("NSH_T_FV", mk_sh(cmd));
    for_add(n, "a", true);
    for_add(n, "b", true);
    for_add(n, "c", true);
    ASSERT_EQ(run_tree(n), 0);

    char body[READ_BUF];
    read_file(out, body, sizeof body);
    ASSERT_STR_EQ(body, "a\nb\nc\n");
}

// setenv means the variable is visible in the shell process straight away.
TEST(for_leaves_the_variable_set_in_the_environment) {
    Node *n = mk_for("NSH_T_FV2", mk_pipe("/bin/true", NULL));
    for_add(n, "first", true);
    for_add(n, "last", true);
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_STR_EQ(getenv("NSH_T_FV2"), "last");
    unsetenv("NSH_T_FV2");
}

TEST(for_expands_its_words_before_assigning) {
    ASSERT_EQ(setenv("NSH_T_SRC", "zed", 1), 0);
    Node *n = mk_for("NSH_T_FV3", mk_pipe("/bin/true", NULL));
    for_add(n, "$NSH_T_SRC", true);
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_STR_EQ(getenv("NSH_T_FV3"), "zed");
    unsetenv("NSH_T_SRC");
    unsetenv("NSH_T_FV3");
}

TEST(a_for_with_no_words_is_zero_and_runs_nothing) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "for_empty");
    ASSERT_EQ(run_tree(mk_pipe("/bin/false", NULL)), 1);
    ASSERT_EQ(run_tree(mk_for("NSH_T_FV4", mk_marker(marker))), 0);
    ASSERT_TRUE(!file_exists(marker));
    ASSERT_TRUE(getenv("NSH_T_FV4") == NULL);
    ASSERT_EQ(g_sh.loop_depth, 0);
}

TEST(the_last_body_iteration_owns_the_for_status) {
    Node *n = mk_for("NSH_T_FV5", mk_sh("test $NSH_T_FV5 = ok"));
    for_add(n, "ok", true);
    for_add(n, "no", true);
    ASSERT_EQ(run_tree(n), 1);
    unsetenv("NSH_T_FV5");
}

TEST(a_word_that_cannot_expand_ends_the_for_loop) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "for_badword");
    Node *n = mk_for("NSH_T_FV6", mk_marker(marker));
    for_add(n, "${BROKEN", true);
    for_add(n, "fine", true);

    int saved = stderr_off();
    int status = run_tree(n);
    stderr_on(saved);

    ASSERT_EQ(status, 1);
    ASSERT_TRUE(!file_exists(marker));
    ASSERT_EQ(g_sh.loop_depth, 0);
}

// An empty name is the one setenv rejects without needing a broken system.
TEST(a_setenv_failure_ends_the_for_loop) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "for_badvar");
    Node *n = mk_for("", mk_marker(marker));
    for_add(n, "value", true);

    int saved = stderr_off();
    int status = run_tree(n);
    stderr_on(saved);

    ASSERT_EQ(status, 1);
    ASSERT_TRUE(!file_exists(marker));
    ASSERT_EQ(g_sh.loop_depth, 0);
}

TEST(a_for_consumes_break) {
    char count[PATH_BUF];
    scratch(count, sizeof count, "for_break");
    make_empty(count);
    g_sh.flow = FLOW_BREAK;
    Node *n = mk_for("NSH_T_FV7", mk_appender(count));
    for_add(n, "a", true);
    for_add(n, "b", true);
    for_add(n, "c", true);
    ASSERT_EQ(run_tree(n), 0);
    ASSERT_EQ(g_sh.flow, FLOW_NONE);
    ASSERT_EQ(count_lines(count), 1);
    unsetenv("NSH_T_FV7");
}

// Loops and Ctrl-C

// A child that died on SIGINT reports 130, which the loop treats as a stop.
TEST(a_child_killed_by_sigint_ends_the_loop) {
    char count[PATH_BUF];
    char cmd[CMD_BUF];
    scratch(count, sizeof count, "int_child");
    make_empty(count);
    snprintf(cmd, sizeof cmd, "echo x >> %s; kill -INT $$", count);
    Node *n = mk_while(mk_counter_cond(count, 5), mk_sh(cmd));

    int saved = stderr_off();
    int status = run_tree(n);
    stderr_on(saved);

    ASSERT_EQ(status, 130);
    ASSERT_EQ(count_lines(count), 1);
    ASSERT_EQ(g_sh.loop_depth, 0);
}

// The watch is on inside a loop, so a SIGINT aimed at the shell is recorded.
TEST(a_sigint_at_the_shell_ends_the_loop) {
    char count[PATH_BUF];
    char cmd[CMD_BUF];
    scratch(count, sizeof count, "int_shell");
    make_empty(count);
    snprintf(cmd, sizeof cmd, "echo x >> %s; kill -INT %ld", count,
             (long)getpid());
    Node *n = mk_while(mk_counter_cond(count, 5), mk_sh(cmd));

    int saved = stderr_off();
    int status = run_tree(n);
    stderr_on(saved);

    ASSERT_EQ(status, 130);
    ASSERT_EQ(count_lines(count), 1);
    ASSERT_EQ(g_sh.loop_depth, 0);
}

// Leaving the loop puts SIGINT back on SIG_IGN, so a later one is swallowed.
TEST(the_interrupt_watch_is_off_once_the_loop_ends) {
    ASSERT_EQ(run_tree(mk_while(mk_pipe("/bin/false", NULL),
                                mk_pipe("/bin/true", NULL))), 0);
    raise(SIGINT);
    ASSERT_EQ(signals_int_take(), 0);
}

// Only the outermost loop toggles the watch: an inner loop ending must not
// disarm it, or the SIGINT after it would be ignored and the outer loop
// would run all four passes instead of stopping at one.
TEST(an_inner_loop_does_not_disarm_the_interrupt_watch) {
    char count[PATH_BUF];
    char cmd[CMD_BUF];
    scratch(count, sizeof count, "int_nested");
    make_empty(count);
    snprintf(cmd, sizeof cmd, "echo x >> %s; kill -INT %ld", count,
             (long)getpid());
    Node *inner = mk_while(mk_pipe("/bin/false", NULL),
                           mk_pipe("/bin/true", NULL));
    Node *body = mk_list(inner, mk_sh(cmd), NULL);
    Node *outer = mk_while(mk_counter_cond(count, 4), body);

    int saved = stderr_off();
    int status = run_tree(outer);
    stderr_on(saved);

    ASSERT_EQ(status, 130);
    ASSERT_EQ(count_lines(count), 1);
    ASSERT_EQ(g_sh.loop_depth, 0);
}

// Function definitions

TEST(a_funcdef_registers_a_body_and_leaves_zero) {
    ASSERT_EQ(run_tree(mk_pipe("/bin/false", NULL)), 1);
    ASSERT_EQ(run_tree(mk_funcdef("nsh_t_defined", mk_pipe("/bin/true", NULL))),
              0);
    ASSERT_TRUE(func_lookup("nsh_t_defined") != NULL);
    ASSERT_TRUE(func_lookup("nsh_t_undefined") == NULL);
    ASSERT_TRUE(func_lookup(NULL) == NULL);
}

// The table holds a clone, so freeing the parse tree cannot take it away.
TEST(the_table_keeps_its_own_copy_of_the_body) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "clone_ran");
    ASSERT_EQ(run_tree(mk_funcdef("nsh_t_clone", mk_marker(marker))), 0);

    char name[] = "nsh_t_clone";
    char *argv[] = {name, NULL};
    int status = -1;
    ASSERT_TRUE(eval_maybe_call_function(&g_sh, 1, argv, &status));
    ASSERT_EQ(status, 0);
    ASSERT_TRUE(file_exists(marker));
}

TEST(a_second_definition_replaces_the_first) {
    char first[PATH_BUF];
    char second[PATH_BUF];
    scratch(first, sizeof first, "redef_first");
    scratch(second, sizeof second, "redef_second");
    ASSERT_EQ(run_tree(mk_funcdef("nsh_t_redef", mk_marker(first))), 0);
    ASSERT_EQ(run_tree(mk_funcdef("nsh_t_redef", mk_marker(second))), 0);

    char name[] = "nsh_t_redef";
    char *argv[] = {name, NULL};
    int status = -1;
    ASSERT_TRUE(eval_maybe_call_function(&g_sh, 1, argv, &status));
    ASSERT_EQ(status, 0);
    ASSERT_TRUE(file_exists(second));
    ASSERT_TRUE(!file_exists(first));
}

// A funcdef inside a loop runs once per pass and must not corrupt the table.
TEST(a_funcdef_inside_a_loop_survives_every_pass) {
    char count[PATH_BUF];
    char marker[PATH_BUF];
    scratch(count, sizeof count, "def_loop");
    scratch(marker, sizeof marker, "def_loop_ran");
    make_empty(count);
    Node *body = mk_list(mk_appender(count),
                         mk_funcdef("nsh_t_looped", mk_marker(marker)), NULL);
    ASSERT_EQ(run_tree(mk_while(mk_counter_cond(count, 3), body)), 0);
    ASSERT_EQ(count_lines(count), 3);

    char name[] = "nsh_t_looped";
    char *argv[] = {name, NULL};
    int status = -1;
    ASSERT_TRUE(eval_maybe_call_function(&g_sh, 1, argv, &status));
    ASSERT_TRUE(file_exists(marker));
}

// Function calls

TEST(an_unknown_name_is_not_a_function_call) {
    char name[] = "nsh_t_no_such_function";
    char *argv[] = {name, NULL};
    int status = -1;
    ASSERT_TRUE(!eval_maybe_call_function(&g_sh, 1, argv, &status));
    ASSERT_EQ(status, -1);
}

TEST(a_call_with_no_argv_is_refused) {
    int status = -1;
    ASSERT_TRUE(!eval_maybe_call_function(&g_sh, 0, NULL, &status));
    ASSERT_TRUE(!eval_maybe_call_function(NULL, 1, NULL, &status));
}

TEST(the_body_status_becomes_the_call_status) {
    ASSERT_EQ(run_tree(mk_funcdef("nsh_t_fail", mk_pipe("/bin/false", NULL))),
              0);
    char name[] = "nsh_t_fail";
    char *argv[] = {name, NULL};
    int status = -1;
    ASSERT_TRUE(eval_maybe_call_function(&g_sh, 1, argv, &status));
    ASSERT_EQ(status, 1);
}

// $1.. are the call's, $0 is the caller's, and both are back afterwards.
TEST(a_call_swaps_the_positionals_and_restores_them) {
    char zero[] = "nullsh";
    char outer[] = "outer";
    char *saved_argv[] = {zero, outer, NULL};
    g_sh.argc = 2;
    g_sh.argv = saved_argv;

    ASSERT_EQ(run_tree(mk_funcdef("nsh_t_args", mk_pipe("/bin/true", NULL))), 0);
    char name[] = "nsh_t_args";
    char one[] = "one";
    char two[] = "two";
    char *argv[] = {name, one, two, NULL};
    int status = -1;
    ASSERT_TRUE(eval_maybe_call_function(&g_sh, 3, argv, &status));

    ASSERT_EQ(status, 0);
    ASSERT_EQ(g_sh.argc, 2);
    ASSERT_TRUE(g_sh.argv == saved_argv);
    ASSERT_EQ(g_sh.func_depth, 0);
    g_sh.argc = 0;
    g_sh.argv = NULL;
}

TEST(the_recursion_cap_refuses_a_deeper_call) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "deep_ran");
    ASSERT_EQ(run_tree(mk_funcdef("nsh_t_deep", mk_marker(marker))), 0);

    char name[] = "nsh_t_deep";
    char *argv[] = {name, NULL};
    int status = -1;
    g_sh.func_depth = DEPTH_CAP;
    int saved = stderr_off();
    bool called = eval_maybe_call_function(&g_sh, 1, argv, &status);
    stderr_on(saved);
    g_sh.func_depth = 0;

    ASSERT_TRUE(called);
    ASSERT_EQ(status, 1);
    ASSERT_TRUE(!file_exists(marker));
}

// The real return builtin belongs to exec; the flag stands in for it here.
TEST(a_call_consumes_return_and_takes_its_status) {
    char marker[PATH_BUF];
    scratch(marker, sizeof marker, "return_body");
    Node *body = mk_list(mk_marker(marker), NULL);
    ASSERT_EQ(run_tree(mk_funcdef("nsh_t_ret", body)), 0);

    char name[] = "nsh_t_ret";
    char *argv[] = {name, NULL};
    int status = -1;
    g_sh.flow = FLOW_RETURN;
    g_sh.flow_status = 9;
    ASSERT_TRUE(eval_maybe_call_function(&g_sh, 1, argv, &status));

    ASSERT_EQ(status, 9);
    ASSERT_EQ(g_sh.flow, FLOW_NONE);
    ASSERT_TRUE(!file_exists(marker));
    g_sh.flow_status = 0;
}

TEST(a_stray_break_escaping_a_function_is_consumed) {
    Node *body = mk_list(mk_pipe("/bin/true", NULL), NULL);
    ASSERT_EQ(run_tree(mk_funcdef("nsh_t_stray", body)), 0);
    ASSERT_EQ(run_tree(mk_pipe("/bin/false", NULL)), 1);

    char name[] = "nsh_t_stray";
    char *argv[] = {name, NULL};
    int status = -1;
    g_sh.flow = FLOW_CONTINUE;
    int saved = stderr_off();
    bool called = eval_maybe_call_function(&g_sh, 1, argv, &status);
    stderr_on(saved);

    ASSERT_TRUE(called);
    ASSERT_EQ(g_sh.flow, FLOW_NONE);
    // The stray does not rewrite the status the body left behind.
    ASSERT_EQ(status, 1);
}

TEST(want_exit_passes_straight_through_a_call) {
    Node *body = mk_list(mk_pipe("exit", "2", NULL), NULL);
    ASSERT_EQ(run_tree(mk_funcdef("nsh_t_exit", body)), 0);

    char name[] = "nsh_t_exit";
    char *argv[] = {name, NULL};
    int status = -1;
    ASSERT_TRUE(eval_maybe_call_function(&g_sh, 1, argv, &status));
    ASSERT_EQ(status, 2);
    ASSERT_TRUE(g_sh.want_exit);
    ASSERT_EQ(g_sh.func_depth, 0);
    g_sh.want_exit = false;
    g_sh.exit_code = 0;
}

// A scratch directory has to outlive every case, so main is spelled out here.
int main(void) {
    setup();
    int failures = 0;
    for (int i = 0; i < nsh_test_count; i++) {
        int failed = 0;
        nsh_test_cases[i].fn(&failed);
        printf("  %s %s\n", failed ? "FAIL" : "ok  ", nsh_test_cases[i].name);
        failures += failed;
    }
    printf("  %d test(s), %d failed\n", nsh_test_count, failures);
    teardown();
    return failures == 0 ? 0 : 1;
}
