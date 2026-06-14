// Unit tests for the executor. Every case goes in as a line of shell text and
// comes out as a status plus whatever the command did to the filesystem, so
// the lexer, parser, expander and executor are all under test together.

#define _POSIX_C_SOURCE 200809L

#include "exec.h"

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

// The scratch directory name is a fixed template, so a short buffer is honest
// and keeps the compiler from suspecting a truncated snprintf.
#define TMP_BUF 64

static Shell g_sh;
static char g_tmp[TMP_BUF];
static char g_cwd[PATH_BUF];

// One scratch directory and one saved cwd for the whole binary, torn down in
// main after the cases run.
static void setup(void) {
    history_init(&g_sh.history, 16);
    g_sh.last_status = 0;
    g_sh.want_exit = false;
    g_sh.exit_code = 0;
    if (getcwd(g_cwd, sizeof g_cwd) == NULL) {
        fprintf(stderr, "exec_test: getcwd failed\n");
        exit(1);
    }
    snprintf(g_tmp, sizeof g_tmp, "/tmp/nsh_exec_XXXXXX");
    if (mkdtemp(g_tmp) == NULL) {
        fprintf(stderr, "exec_test: mkdtemp failed\n");
        exit(1);
    }
}

static void teardown(void) {
    char cmd[LINE_BUF];
    if (chdir(g_cwd) != 0) {
        fprintf(stderr, "exec_test: could not return to %s\n", g_cwd);
    }
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_tmp);
    if (system(cmd) != 0) {
        fprintf(stderr, "exec_test: could not remove %s\n", g_tmp);
    }
    history_free(&g_sh.history);
}

// Builds <scratch>/name. The buffer is static and reused, so only one path can
// be held at a time.
static const char *tmp_path(const char *name) {
    static char buf[PATH_BUF];
    snprintf(buf, sizeof buf, "%s/%s", g_tmp, name);
    return buf;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// The real path a line takes through the shell: scan, parse, execute. Returns
// the status the shell would report, so a case reads like a transcript.
static int run(const char *line) {
    TokenList tl = {{NULL, 0, 0}};
    Pipeline pl = {{NULL, 0, 0}, false};
    if (lexer_scan(line, &tl) == NSH_OK && parser_parse(&tl, &pl) == NSH_OK) {
        exec_pipeline(&g_sh, &pl);
    } else {
        g_sh.last_status = 2;
    }
    token_list_free(&tl);
    pipeline_free(&pl);
    return g_sh.last_status;
}

// Cases that make a child complain on purpose route stderr to /dev/null so a
// passing run stays quiet. Returns the descriptor to hand back later.
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

// Externals

TEST(absolute_path_true_succeeds) {
    ASSERT_EQ(run("/bin/true"), 0);
}

TEST(absolute_path_false_fails) {
    ASSERT_EQ(run("/bin/false"), 1);
}

TEST(bare_name_is_found_on_path) {
    ASSERT_EQ(run("true"), 0);
    ASSERT_EQ(run("false"), 1);
}

// Proof that a child really ran: it leaves a file behind.
TEST(arguments_reach_the_program) {
    char line[LINE_BUF];
    const char *marker = tmp_path("ran_with_args");
    snprintf(line, sizeof line, "/bin/sh -c \"echo hi > %s\"", marker);
    ASSERT_EQ(run(line), 0);
    ASSERT_TRUE(file_exists(marker));
}

TEST(quoted_argument_stays_one_word) {
    char line[LINE_BUF];
    const char *marker = tmp_path("a b");
    snprintf(line, sizeof line, "/bin/sh -c \"echo hi > '%s'\"", marker);
    ASSERT_EQ(run(line), 0);
    ASSERT_TRUE(file_exists(marker));
}

TEST(unknown_command_is_127) {
    int saved = stderr_off();
    int status = run("nsh_definitely_not_a_command");
    stderr_on(saved);
    ASSERT_EQ(status, 127);
}

TEST(missing_absolute_path_is_127) {
    int saved = stderr_off();
    int status = run("/nsh/no/such/program");
    stderr_on(saved);
    ASSERT_EQ(status, 127);
}

TEST(non_executable_file_is_126) {
    const char *victim = tmp_path("noexec");
    FILE *f = fopen(victim, "w");
    ASSERT_TRUE(f != NULL);
    fputs("not a program\n", f);
    fclose(f);
    ASSERT_EQ(chmod(victim, 0644), 0);
    ASSERT_EQ(chdir(g_tmp), 0);

    int saved = stderr_off();
    int status = run("./noexec");
    stderr_on(saved);
    int back = chdir(g_cwd);

    ASSERT_EQ(status, 126);
    ASSERT_EQ(back, 0);
}

// Only permission failures across the whole search turn into 126. A PATH
// directory holding a matching but unrunnable file is the everyday shape.
TEST(path_hit_without_execute_permission_is_126) {
    static char old_path[LINE_BUF];
    char bindir[PATH_BUF];
    char victim[PATH_BUF];
    snprintf(bindir, sizeof bindir, "%s/bin", g_tmp);
    snprintf(victim, sizeof victim, "%s/bin/nsh_locked", g_tmp);
    ASSERT_EQ(mkdir(bindir, 0755), 0);

    FILE *f = fopen(victim, "w");
    ASSERT_TRUE(f != NULL);
    fputs("#!/bin/sh\n", f);
    fclose(f);
    ASSERT_EQ(chmod(victim, 0644), 0);

    const char *cur = getenv("PATH");
    bool had_path = (cur != NULL);
    if (had_path) {
        snprintf(old_path, sizeof old_path, "%s", cur);
    }
    ASSERT_EQ(setenv("PATH", bindir, 1), 0);

    int saved = stderr_off();
    int status = run("nsh_locked");
    stderr_on(saved);

    // Restore before asserting, so a failure here does not break later cases.
    if (had_path) {
        setenv("PATH", old_path, 1);
    } else {
        unsetenv("PATH");
    }
    ASSERT_EQ(status, 126);
}

// Builtins

TEST(cd_builtin_changes_the_shell_cwd) {
    // What the kernel calls the scratch directory, which is what getcwd will
    // report after the builtin runs even if /tmp is reached through a symlink.
    char want[PATH_BUF];
    ASSERT_EQ(chdir(g_tmp), 0);
    bool got_want = getcwd(want, sizeof want) != NULL;
    ASSERT_EQ(chdir(g_cwd), 0);
    ASSERT_TRUE(got_want);

    char line[LINE_BUF];
    snprintf(line, sizeof line, "cd %s", g_tmp);
    ASSERT_EQ(run(line), 0);

    char here[PATH_BUF];
    bool got_here = getcwd(here, sizeof here) != NULL;
    int back = chdir(g_cwd);

    ASSERT_TRUE(got_here);
    ASSERT_STR_EQ(here, want);
    ASSERT_EQ(back, 0);
}

TEST(export_builtin_reaches_the_environment) {
    ASSERT_EQ(run("export NSH_T_EXPORT=yes"), 0);
    ASSERT_STR_EQ(getenv("NSH_T_EXPORT"), "yes");
    ASSERT_EQ(run("unset NSH_T_EXPORT"), 0);
    ASSERT_TRUE(getenv("NSH_T_EXPORT") == NULL);
}

TEST(exit_builtin_sets_want_exit_without_leaving) {
    ASSERT_EQ(run("exit 7"), 7);
    ASSERT_TRUE(g_sh.want_exit == true);
    ASSERT_EQ(g_sh.exit_code, 7);
    g_sh.want_exit = false;
    g_sh.exit_code = 0;
}

// Expansion

TEST(variables_expand_before_the_program_sees_them) {
    char line[LINE_BUF];
    const char *marker = tmp_path("expanded");
    snprintf(line, sizeof line, "export NSH_T_MARK=%s", marker);
    ASSERT_EQ(run(line), 0);
    ASSERT_EQ(run("/bin/sh -c \"echo hi > $NSH_T_MARK\""), 0);
    bool made = file_exists(tmp_path("expanded"));
    ASSERT_EQ(run("unset NSH_T_MARK"), 0);
    ASSERT_TRUE(made);
}

TEST(single_quotes_keep_the_dollar_literal) {
    ASSERT_EQ(run("export NSH_T_Q=live"), 0);
    // Single quotes hand the child the four characters $NSH, so only the
    // child's own shell can resolve them, and it agrees on the value.
    ASSERT_EQ(run("/bin/sh -c 'test \"$NSH_T_Q\" = live'"), 0);
    ASSERT_EQ(run("unset NSH_T_Q"), 0);
}

// $? is the previous command's status, visible to the next line.
TEST(dollar_question_carries_the_last_status) {
    ASSERT_EQ(run("/bin/false"), 1);
    ASSERT_EQ(run("export NSH_T_STATUS=$?"), 0);
    ASSERT_STR_EQ(getenv("NSH_T_STATUS"), "1");
    ASSERT_EQ(run("/bin/true"), 0);
    ASSERT_EQ(run("export NSH_T_STATUS=$?"), 0);
    ASSERT_STR_EQ(getenv("NSH_T_STATUS"), "0");
    ASSERT_EQ(run("unset NSH_T_STATUS"), 0);
}

// An unclosed ${ is caught in the expander, after a clean lex and parse.
TEST(unterminated_brace_expansion_is_a_bad_substitution) {
    int saved = stderr_off();
    int status = run("/bin/true ${BROKEN");
    stderr_on(saved);
    ASSERT_EQ(status, 2);
}

// Phase 1 limits

TEST(a_pipeline_is_refused_without_running_anything) {
    char line[LINE_BUF];
    const char *marker = tmp_path("pipe_ran");
    snprintf(line, sizeof line, "/bin/sh -c \"echo hi > %s\" | /bin/cat",
             marker);
    int saved = stderr_off();
    int status = run(line);
    stderr_on(saved);
    ASSERT_EQ(status, 1);
    ASSERT_TRUE(!file_exists(tmp_path("pipe_ran")));
}

TEST(a_redirect_is_refused_without_running_anything) {
    char line[LINE_BUF];
    const char *marker = tmp_path("redir_ran");
    snprintf(line, sizeof line, "/bin/true > %s", marker);
    int saved = stderr_off();
    int status = run(line);
    stderr_on(saved);
    ASSERT_EQ(status, 1);
    ASSERT_TRUE(!file_exists(tmp_path("redir_ran")));
}

TEST(background_is_refused_without_running_anything) {
    char line[LINE_BUF];
    const char *marker = tmp_path("bg_ran");
    snprintf(line, sizeof line, "/bin/sh -c \"echo hi > %s\" &", marker);
    int saved = stderr_off();
    int status = run(line);
    stderr_on(saved);
    ASSERT_EQ(status, 1);
    ASSERT_TRUE(!file_exists(tmp_path("bg_ran")));
}

// An empty pipeline is not a command, so the status must survive it.
TEST(an_empty_line_leaves_the_status_alone) {
    ASSERT_EQ(run("/bin/false"), 1);
    ASSERT_EQ(run("   "), 1);
    ASSERT_EQ(run(""), 1);
}

// TEST_MAIN owns main, and these cases need a scratch directory around the
// whole run, so the loop is spelled out here instead.
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
