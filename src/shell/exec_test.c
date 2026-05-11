// Tests for the executor, driven by whole lines of shell text.

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
#define READ_BUF 1024

// A scratch path is the template plus a short name, so two of them fit a line.
#define SCRATCH_BUF 128

// A fixed template, so a short buffer keeps snprintf from looking truncated.
#define TMP_BUF 64

static Shell g_sh;
static char g_tmp[TMP_BUF];
static char g_cwd[PATH_BUF];

// One scratch directory and one saved cwd for the whole binary.
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

// The buffer is static and reused, so only one path at a time.
static const char *tmp_path(const char *name) {
    static char buf[PATH_BUF];
    snprintf(buf, sizeof buf, "%s/%s", g_tmp, name);
    return buf;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
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

// The real path a line takes through the shell: scan, parse, execute.
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

// Cases that make a child complain on purpose keep a passing run quiet.
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

// Only permission failures across the whole search turn into 126.
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
    // What the kernel calls the scratch dir, even if /tmp is a symlink.
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
    // Single quotes pass $NSH_T_Q through, so only the child resolves it.
    ASSERT_EQ(run("/bin/sh -c 'test \"$NSH_T_Q\" = live'"), 0);
    ASSERT_EQ(run("unset NSH_T_Q"), 0);
}

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

// Pipes and redirection

// The first stage proves it ran by leaving a file, so cat stays silent.
TEST(a_pipeline_runs_every_stage) {
    char line[LINE_BUF];
    const char *marker = tmp_path("pipe_ran");
    snprintf(line, sizeof line, "/bin/sh -c \"echo hi > %s\" | /bin/cat",
             marker);
    ASSERT_EQ(run(line), 0);
    ASSERT_TRUE(file_exists(tmp_path("pipe_ran")));
}

TEST(a_redirect_writes_the_file) {
    char line[LINE_BUF];
    char path[SCRATCH_BUF];
    snprintf(path, sizeof path, "%s/redir_ran", g_tmp);
    snprintf(line, sizeof line, "/bin/echo hi > %s", path);
    ASSERT_EQ(run(line), 0);

    char body[READ_BUF];
    read_file(path, body, sizeof body);
    ASSERT_STR_EQ(body, "hi\n");
}

TEST(the_last_stage_owns_the_status) {
    ASSERT_EQ(run("false | true"), 0);
    ASSERT_EQ(run("true | false"), 1);
    ASSERT_EQ(run("true | false | true"), 0);
    ASSERT_EQ(run("true | true | false"), 1);
}

TEST(a_three_stage_pipeline_passes_data_through) {
    char line[LINE_BUF];
    char path[SCRATCH_BUF];
    snprintf(path, sizeof path, "%s/three.txt", g_tmp);
    snprintf(line, sizeof line, "printf 'b\\na\\n' | sort | cat > %s", path);
    ASSERT_EQ(run(line), 0);

    char body[READ_BUF];
    read_file(path, body, sizeof body);
    ASSERT_STR_EQ(body, "a\nb\n");
}

TEST(append_extends_where_truncation_replaces) {
    char line[LINE_BUF];
    char path[SCRATCH_BUF];
    char body[READ_BUF];
    snprintf(path, sizeof path, "%s/append.txt", g_tmp);

    snprintf(line, sizeof line, "/bin/echo one > %s", path);
    ASSERT_EQ(run(line), 0);
    snprintf(line, sizeof line, "/bin/echo two >> %s", path);
    ASSERT_EQ(run(line), 0);
    read_file(path, body, sizeof body);
    ASSERT_STR_EQ(body, "one\ntwo\n");

    snprintf(line, sizeof line, "/bin/echo three > %s", path);
    ASSERT_EQ(run(line), 0);
    read_file(path, body, sizeof body);
    ASSERT_STR_EQ(body, "three\n");
}

TEST(input_redirect_feeds_the_command) {
    char line[LINE_BUF];
    char src[SCRATCH_BUF];
    char dst[SCRATCH_BUF];
    snprintf(src, sizeof src, "%s/feed_in.txt", g_tmp);
    snprintf(dst, sizeof dst, "%s/feed_out.txt", g_tmp);

    snprintf(line, sizeof line, "/bin/echo fed > %s", src);
    ASSERT_EQ(run(line), 0);
    snprintf(line, sizeof line, "cat < %s > %s", src, dst);
    ASSERT_EQ(run(line), 0);

    char body[READ_BUF];
    read_file(dst, body, sizeof body);
    ASSERT_STR_EQ(body, "fed\n");
}

TEST(stderr_redirect_leaves_stdout_clean) {
    char line[LINE_BUF];
    char out[SCRATCH_BUF];
    char err[SCRATCH_BUF];
    snprintf(out, sizeof out, "%s/split_out.txt", g_tmp);
    snprintf(err, sizeof err, "%s/split_err.txt", g_tmp);
    snprintf(line, sizeof line,
             "/bin/sh -c 'echo to_out; echo to_err >&2' > %s 2> %s", out, err);
    ASSERT_EQ(run(line), 0);

    char body[READ_BUF];
    read_file(out, body, sizeof body);
    ASSERT_STR_EQ(body, "to_out\n");
    read_file(err, body, sizeof body);
    ASSERT_STR_EQ(body, "to_err\n");
}

TEST(a_redirect_target_can_come_from_a_variable) {
    char line[LINE_BUF];
    char path[SCRATCH_BUF];
    snprintf(path, sizeof path, "%s/from_var.txt", g_tmp);

    snprintf(line, sizeof line, "export NSH_T_TARGET=%s", path);
    ASSERT_EQ(run(line), 0);
    ASSERT_EQ(run("/bin/echo via_var > $NSH_T_TARGET"), 0);

    char body[READ_BUF];
    read_file(path, body, sizeof body);
    int cleared = run("unset NSH_T_TARGET");
    ASSERT_STR_EQ(body, "via_var\n");
    ASSERT_EQ(cleared, 0);
}

// A target that cannot be opened is a command that never ran.
TEST(an_unopenable_target_leaves_status_one_and_runs_nothing) {
    char line[LINE_BUF];
    const char *marker = tmp_path("never_ran");
    snprintf(line, sizeof line,
             "/bin/sh -c \"echo hi > %s\" > /nsh/no/such/dir/target", marker);
    int saved = stderr_off();
    int status = run(line);
    stderr_on(saved);
    ASSERT_EQ(status, 1);
    ASSERT_TRUE(!file_exists(tmp_path("never_ran")));
}

// A bad target word is a usage error, exactly like a bad argv word.
TEST(an_unterminated_target_is_a_bad_substitution) {
    int saved = stderr_off();
    int status = run("/bin/true > ${BROKEN");
    stderr_on(saved);
    ASSERT_EQ(status, 2);
}

// The redirect moves the shell's own fds, so the chdir must still land here.
TEST(a_builtin_with_a_redirect_still_changes_the_shell) {
    char want[PATH_BUF];
    ASSERT_EQ(chdir(g_tmp), 0);
    bool got_want = getcwd(want, sizeof want) != NULL;
    ASSERT_EQ(chdir(g_cwd), 0);
    ASSERT_TRUE(got_want);

    char line[LINE_BUF];
    char log[SCRATCH_BUF];
    snprintf(log, sizeof log, "%s/cd_log.txt", g_tmp);
    snprintf(line, sizeof line, "cd %s > %s", g_tmp, log);
    int status = run(line);

    char here[PATH_BUF];
    bool got_here = getcwd(here, sizeof here) != NULL;
    int back = chdir(g_cwd);

    ASSERT_EQ(status, 0);
    ASSERT_TRUE(got_here);
    ASSERT_STR_EQ(here, want);
    ASSERT_EQ(back, 0);
    ASSERT_TRUE(file_exists(log));
}

TEST(a_builtin_can_be_a_pipeline_stage) {
    char line[LINE_BUF];
    char path[SCRATCH_BUF];
    snprintf(path, sizeof path, "%s/help.txt", g_tmp);
    snprintf(line, sizeof line, "help | /bin/cat > %s", path);
    ASSERT_EQ(run(line), 0);

    char body[READ_BUF];
    read_file(path, body, sizeof body);
    ASSERT_TRUE(strstr(body, "nullsh builtins") != NULL);
}

// An early reader exit must not wedge the writer or the shell.
TEST(an_early_reader_exit_does_not_hang) {
    char line[LINE_BUF];
    char path[SCRATCH_BUF];
    snprintf(path, sizeof path, "%s/head.txt", g_tmp);
    snprintf(line, sizeof line, "seq 100000 | head -1 > %s", path);
    ASSERT_EQ(run(line), 0);

    char body[READ_BUF];
    read_file(path, body, sizeof body);
    ASSERT_STR_EQ(body, "1\n");
}

// Six stages is where a single leaked write end shows up as a hang.
TEST(a_six_stage_pipeline_closes_every_fd) {
    char line[LINE_BUF];
    char path[SCRATCH_BUF];
    snprintf(path, sizeof path, "%s/six.txt", g_tmp);
    snprintf(line, sizeof line,
             "/bin/echo deep | cat | cat | cat | cat | cat > %s", path);
    ASSERT_EQ(run(line), 0);

    char body[READ_BUF];
    read_file(path, body, sizeof body);
    ASSERT_STR_EQ(body, "deep\n");
}

// Phase 4 limits

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

// An empty pipeline is not a command, so the status survives it.
TEST(an_empty_line_leaves_the_status_alone) {
    ASSERT_EQ(run("/bin/false"), 1);
    ASSERT_EQ(run("   "), 1);
    ASSERT_EQ(run(""), 1);
}

// These cases need a scratch directory around the whole run, so main is here.
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
