// Tests for the redirect module: the fd moves, the flags, save and restore.

#define _POSIX_C_SOURCE 200809L

#include "redirect.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lexer.h"
#include "parser.h"

#include "../util/vec.h"
#include "../../tests/harness.h"

#define PATH_BUF 4096
#define LINE_BUF 8192
#define READ_BUF 256

// A fixed template, so a short buffer keeps snprintf from looking truncated.
#define TMP_BUF 64

static char g_tmp[TMP_BUF];

// No status and no positionals: these targets are all plain paths.
static const ExpandCtx g_ctx = {0, 0, NULL};

static void setup(void) {
    snprintf(g_tmp, sizeof g_tmp, "/tmp/nsh_redir_XXXXXX");
    if (mkdtemp(g_tmp) == NULL) {
        fprintf(stderr, "redirect_test: mkdtemp failed\n");
        exit(1);
    }
}

static void teardown(void) {
    char cmd[LINE_BUF];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_tmp);
    if (system(cmd) != 0) {
        fprintf(stderr, "redirect_test: could not remove %s\n", g_tmp);
    }
}

// The caller owns the buffer, so two paths can be live at once.
static void tmp_path(char *buf, size_t size, const char *name) {
    snprintf(buf, size, "%s/%s", g_tmp, name);
}

// One parsed line, holding the Command the tests hand to redirect_apply.
typedef struct {
    TokenList tl;
    Pipeline pl;
} Parsed;

static const Command *parse_line(Parsed *p, const char *line) {
    p->tl = (TokenList){{NULL, 0, 0}};
    p->pl = (Pipeline){{NULL, 0, 0}, false};
    if (lexer_scan(line, &p->tl) != NSH_OK) {
        return NULL;
    }
    if (parser_parse(&p->tl, &p->pl) != NSH_OK || p->pl.cmds.len != 1) {
        return NULL;
    }
    return vec_get(&p->pl.cmds, 0);
}

static void parsed_free(Parsed *p) {
    token_list_free(&p->tl);
    pipeline_free(&p->pl);
}

static bool write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return false;
    }
    fputs(text, f);
    return fclose(f) == 0;
}

// Empty on any failure, so a caller can compare without checking twice.
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

static long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

// Identity of whatever fd points at, so a restore can be proved exact.
static bool fd_ident(int fd, dev_t *dev, ino_t *ino) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return false;
    }
    *dev = st.st_dev;
    *ino = st.st_ino;
    return true;
}

TEST(redirect_in_feeds_fd_zero) {
    char path[PATH_BUF];
    tmp_path(path, sizeof path, "in.txt");
    ASSERT_TRUE(write_file(path, "hello\n"));

    char line[LINE_BUF];
    snprintf(line, sizeof line, "cat < %s", path);
    Parsed p;
    const Command *c = parse_line(&p, line);
    ASSERT_TRUE(c != NULL);

    RedirSave save = REDIR_SAVE_INIT;
    NshError err = redirect_apply(c, &g_ctx, &save);
    char buf[READ_BUF] = {0};
    ssize_t n = (err == NSH_OK) ? read(STDIN_FILENO, buf, sizeof buf - 1) : -1;
    redirect_restore(&save);
    parsed_free(&p);

    ASSERT_EQ(err, NSH_OK);
    ASSERT_EQ(n, 6);
    ASSERT_STR_EQ(buf, "hello\n");
}

TEST(redirect_out_truncates_and_append_extends) {
    char path[PATH_BUF];
    tmp_path(path, sizeof path, "out.txt");
    char line[LINE_BUF];
    char append_line[LINE_BUF];
    snprintf(line, sizeof line, "x > %s", path);
    snprintf(append_line, sizeof append_line, "x >> %s", path);

    Parsed trunc;
    Parsed app;
    const Command *ct = parse_line(&trunc, line);
    const Command *ca = parse_line(&app, append_line);
    ASSERT_TRUE(ct != NULL && ca != NULL);

    RedirSave save = REDIR_SAVE_INIT;
    NshError first = redirect_apply(ct, &g_ctx, &save);
    ssize_t w1 = (first == NSH_OK) ? write(STDOUT_FILENO, "aaaa", 4) : -1;
    redirect_restore(&save);
    long after_first = file_size(path);

    NshError second = redirect_apply(ct, &g_ctx, &save);
    ssize_t w2 = (second == NSH_OK) ? write(STDOUT_FILENO, "bb", 2) : -1;
    redirect_restore(&save);
    long after_trunc = file_size(path);

    NshError third = redirect_apply(ca, &g_ctx, &save);
    ssize_t w3 = (third == NSH_OK) ? write(STDOUT_FILENO, "cc", 2) : -1;
    redirect_restore(&save);
    long after_append = file_size(path);

    char body[READ_BUF];
    read_file(path, body, sizeof body);
    parsed_free(&trunc);
    parsed_free(&app);

    ASSERT_EQ(first, NSH_OK);
    ASSERT_EQ(second, NSH_OK);
    ASSERT_EQ(third, NSH_OK);
    ASSERT_EQ(w1, 4);
    ASSERT_EQ(w2, 2);
    ASSERT_EQ(w3, 2);
    ASSERT_EQ(after_first, 4);
    ASSERT_EQ(after_trunc, 2);
    ASSERT_EQ(after_append, 4);
    ASSERT_STR_EQ(body, "bbcc");
}

TEST(redirect_err_moves_only_fd_two) {
    char path[PATH_BUF];
    tmp_path(path, sizeof path, "err.txt");
    char line[LINE_BUF];
    snprintf(line, sizeof line, "x 2> %s", path);
    Parsed p;
    const Command *c = parse_line(&p, line);
    ASSERT_TRUE(c != NULL);

    dev_t dev = 0;
    ino_t ino = 0;
    dev_t after_dev = 0;
    ino_t after_ino = 0;
    bool before = fd_ident(STDOUT_FILENO, &dev, &ino);

    RedirSave save = REDIR_SAVE_INIT;
    NshError err = redirect_apply(c, &g_ctx, &save);
    ssize_t n = (err == NSH_OK) ? write(STDERR_FILENO, "boom", 4) : -1;
    bool during = fd_ident(STDOUT_FILENO, &after_dev, &after_ino);
    redirect_restore(&save);
    parsed_free(&p);

    ASSERT_EQ(err, NSH_OK);
    ASSERT_EQ(n, 4);
    ASSERT_TRUE(before && during);
    ASSERT_TRUE(dev == after_dev && ino == after_ino);
    ASSERT_EQ(file_size(path), 4);
}

// The save must survive an unrelated redirect landing on the same fd.
TEST(save_and_restore_put_the_originals_back) {
    char first[PATH_BUF];
    char second[PATH_BUF];
    tmp_path(first, sizeof first, "first.txt");
    tmp_path(second, sizeof second, "second.txt");
    char line[LINE_BUF];
    snprintf(line, sizeof line, "x > %s", second);
    Parsed p;
    const Command *c = parse_line(&p, line);
    ASSERT_TRUE(c != NULL);

    fflush(stdout);
    int real_out = dup(STDOUT_FILENO);
    int fd = open(first, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool staged = (real_out >= 0 && fd >= 0 && dup2(fd, STDOUT_FILENO) >= 0);
    if (fd >= 0) {
        close(fd);
    }

    RedirSave save = REDIR_SAVE_INIT;
    NshError err = staged ? redirect_apply(c, &g_ctx, &save) : NSH_ERR_IO;
    ssize_t w1 = (err == NSH_OK) ? write(STDOUT_FILENO, "in_second", 9) : -1;
    redirect_restore(&save);
    ssize_t w2 = staged ? write(STDOUT_FILENO, "in_first", 8) : -1;

    if (real_out >= 0) {
        dup2(real_out, STDOUT_FILENO);
        close(real_out);
    }
    char a[READ_BUF];
    char b[READ_BUF];
    read_file(first, a, sizeof a);
    read_file(second, b, sizeof b);
    parsed_free(&p);

    ASSERT_TRUE(staged);
    ASSERT_EQ(err, NSH_OK);
    ASSERT_EQ(w1, 9);
    ASSERT_EQ(w2, 8);
    ASSERT_STR_EQ(a, "in_first");
    ASSERT_STR_EQ(b, "in_second");
}

TEST(open_failure_reports_io_and_leaves_the_fds_alone) {
    Parsed p;
    const Command *c = parse_line(&p, "x > /nsh/no/such/dir/target");
    ASSERT_TRUE(c != NULL);

    // The failure prints, and a passing run stays quiet.
    fflush(stderr);
    int real_err = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    dev_t dev[3] = {0};
    ino_t ino[3] = {0};
    dev_t after_dev[3] = {0};
    ino_t after_ino[3] = {0};
    bool before = true;
    for (int fd = 0; fd < 3; fd++) {
        before = fd_ident(fd, &dev[fd], &ino[fd]) && before;
    }

    RedirSave save = REDIR_SAVE_INIT;
    NshError err = redirect_apply(c, &g_ctx, &save);
    redirect_restore(&save);

    bool after = true;
    for (int fd = 0; fd < 3; fd++) {
        after = fd_ident(fd, &after_dev[fd], &after_ino[fd]) && after;
    }

    fflush(stderr);
    if (real_err >= 0) {
        dup2(real_err, STDERR_FILENO);
        close(real_err);
    }
    parsed_free(&p);

    ASSERT_EQ(err, NSH_ERR_IO);
    ASSERT_TRUE(before && after);
    for (int fd = 0; fd < 3; fd++) {
        ASSERT_TRUE(dev[fd] == after_dev[fd] && ino[fd] == after_ino[fd]);
    }
}

TEST(a_bad_target_word_is_not_an_io_error) {
    Parsed p;
    const Command *c = parse_line(&p, "x > ${BROKEN");
    ASSERT_TRUE(c != NULL);

    RedirSave save = REDIR_SAVE_INIT;
    NshError err = redirect_apply(c, &g_ctx, &save);
    redirect_restore(&save);
    parsed_free(&p);

    ASSERT_EQ(err, NSH_ERR_SYNTAX);
}

TEST(the_target_word_is_expanded) {
    char path[PATH_BUF];
    tmp_path(path, sizeof path, "expanded.txt");
    ASSERT_EQ(setenv("NSH_T_TARGET", path, 1), 0);

    Parsed p;
    const Command *c = parse_line(&p, "x > $NSH_T_TARGET");
    ASSERT_TRUE(c != NULL);

    RedirSave save = REDIR_SAVE_INIT;
    NshError err = redirect_apply(c, &g_ctx, &save);
    ssize_t n = (err == NSH_OK) ? write(STDOUT_FILENO, "z", 1) : -1;
    redirect_restore(&save);
    parsed_free(&p);
    unsetenv("NSH_T_TARGET");

    ASSERT_EQ(err, NSH_OK);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(file_size(path), 1);
}

// Every case needs the scratch directory around it, so main is spelled out.
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
