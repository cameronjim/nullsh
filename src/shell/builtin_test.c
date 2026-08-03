// Unit tests for the builtin table: dispatch by name, the exit status each
// builtin reports for every argument shape, and the side effects that matter,
// the process cwd, the environment, and the Shell struct itself. Builtins that
// only print are checked for status, since capturing stdout needs a fork.

#define _POSIX_C_SOURCE 200809L

#include "builtin.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../tests/harness.h"

#define PATH_BUF 4096

static void shell_start(Shell *sh) {
    (void)history_init(&sh->history, 100);
    sh->last_status = 0;
    sh->want_exit = false;
    sh->exit_code = 0;
}

static void shell_stop(Shell *sh) {
    history_free(&sh->history);
}

// Copies the current value of name into out, or leaves out empty. Returns
// false when the variable is unset, so a test can restore it exactly.
static bool env_save(const char *name, char *out, size_t out_size) {
    const char *value = getenv(name);
    out[0] = '\0';
    if (value == NULL) {
        return false;
    }
    snprintf(out, out_size, "%s", value);
    return true;
}

static void env_restore(const char *name, const char *saved, bool had) {
    if (had) {
        setenv(name, saved, 1);
    } else {
        unsetenv(name);
    }
}

TEST(lookup_finds_every_builtin) {
    ASSERT_TRUE(builtin_lookup("cd") != NULL);
    ASSERT_TRUE(builtin_lookup("exit") != NULL);
    ASSERT_TRUE(builtin_lookup("help") != NULL);
    ASSERT_TRUE(builtin_lookup("export") != NULL);
    ASSERT_TRUE(builtin_lookup("unset") != NULL);
    ASSERT_TRUE(builtin_lookup("history") != NULL);
}

TEST(lookup_rejects_non_builtins) {
    ASSERT_TRUE(builtin_lookup("ls") == NULL);
    ASSERT_TRUE(builtin_lookup("") == NULL);
    ASSERT_TRUE(builtin_lookup(NULL) == NULL);
    ASSERT_TRUE(builtin_lookup("CD") == NULL);
    ASSERT_TRUE(builtin_lookup("cdx") == NULL);
}

TEST(lookup_maps_names_to_distinct_functions) {
    ASSERT_TRUE(builtin_lookup("cd") != builtin_lookup("exit"));
    ASSERT_TRUE(builtin_lookup("export") != builtin_lookup("unset"));
    ASSERT_TRUE(builtin_lookup("help") != builtin_lookup("history"));
}

TEST(cd_changes_directory_and_sets_pwd_and_oldpwd) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn cd = builtin_lookup("cd");
    ASSERT_TRUE(cd != NULL);

    char start[PATH_BUF];
    ASSERT_TRUE(getcwd(start, sizeof start) != NULL);
    setenv("PWD", start, 1);

    char *argv[] = {"cd", "/tmp", NULL};
    int rc = cd(&sh, 2, argv);

    char now[PATH_BUF];
    bool got_cwd = getcwd(now, sizeof now) != NULL;
    char pwd[PATH_BUF];
    char oldpwd[PATH_BUF];
    bool had_pwd = env_save("PWD", pwd, sizeof pwd);
    bool had_oldpwd = env_save("OLDPWD", oldpwd, sizeof oldpwd);
    ASSERT_EQ(chdir(start), 0);
    setenv("PWD", start, 1);
    shell_stop(&sh);

    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(got_cwd);
    ASSERT_STR_EQ(now, "/tmp");
    ASSERT_TRUE(had_pwd);
    ASSERT_STR_EQ(pwd, "/tmp");
    ASSERT_TRUE(had_oldpwd);
    ASSERT_STR_EQ(oldpwd, start);
}

TEST(cd_dash_returns_to_the_previous_directory) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn cd = builtin_lookup("cd");
    ASSERT_TRUE(cd != NULL);

    char start[PATH_BUF];
    ASSERT_TRUE(getcwd(start, sizeof start) != NULL);
    setenv("PWD", start, 1);

    char *to_tmp[] = {"cd", "/tmp", NULL};
    char *back[] = {"cd", "-", NULL};
    int first = cd(&sh, 2, to_tmp);
    int second = cd(&sh, 2, back);

    char now[PATH_BUF];
    bool got_cwd = getcwd(now, sizeof now) != NULL;
    char pwd[PATH_BUF];
    char oldpwd[PATH_BUF];
    (void)env_save("PWD", pwd, sizeof pwd);
    (void)env_save("OLDPWD", oldpwd, sizeof oldpwd);
    ASSERT_EQ(chdir(start), 0);
    setenv("PWD", start, 1);
    shell_stop(&sh);

    ASSERT_EQ(first, 0);
    ASSERT_EQ(second, 0);
    ASSERT_TRUE(got_cwd);
    ASSERT_STR_EQ(now, start);
    ASSERT_STR_EQ(pwd, start);
    ASSERT_STR_EQ(oldpwd, "/tmp");
}

TEST(cd_dash_without_oldpwd_fails) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn cd = builtin_lookup("cd");
    ASSERT_TRUE(cd != NULL);

    char saved[PATH_BUF];
    bool had = env_save("OLDPWD", saved, sizeof saved);
    unsetenv("OLDPWD");

    char *argv[] = {"cd", "-", NULL};
    int rc = cd(&sh, 2, argv);

    env_restore("OLDPWD", saved, had);
    shell_stop(&sh);
    ASSERT_EQ(rc, 1);
}

TEST(cd_without_args_uses_home) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn cd = builtin_lookup("cd");
    ASSERT_TRUE(cd != NULL);

    char start[PATH_BUF];
    ASSERT_TRUE(getcwd(start, sizeof start) != NULL);
    setenv("PWD", start, 1);
    char saved_home[PATH_BUF];
    bool had_home = env_save("HOME", saved_home, sizeof saved_home);
    setenv("HOME", "/tmp", 1);

    char *argv[] = {"cd", NULL};
    int rc = cd(&sh, 1, argv);

    char now[PATH_BUF];
    bool got_cwd = getcwd(now, sizeof now) != NULL;
    env_restore("HOME", saved_home, had_home);
    ASSERT_EQ(chdir(start), 0);
    setenv("PWD", start, 1);
    shell_stop(&sh);

    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(got_cwd);
    ASSERT_STR_EQ(now, "/tmp");
}

TEST(cd_without_args_and_without_home_fails) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn cd = builtin_lookup("cd");
    ASSERT_TRUE(cd != NULL);

    char start[PATH_BUF];
    ASSERT_TRUE(getcwd(start, sizeof start) != NULL);
    char saved_home[PATH_BUF];
    bool had_home = env_save("HOME", saved_home, sizeof saved_home);
    unsetenv("HOME");

    char *argv[] = {"cd", NULL};
    int rc = cd(&sh, 1, argv);

    char now[PATH_BUF];
    bool got_cwd = getcwd(now, sizeof now) != NULL;
    env_restore("HOME", saved_home, had_home);
    shell_stop(&sh);

    ASSERT_EQ(rc, 1);
    ASSERT_TRUE(got_cwd);
    // A failed cd leaves the shell exactly where it was.
    ASSERT_STR_EQ(now, start);
}

TEST(cd_rejects_more_than_one_argument) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn cd = builtin_lookup("cd");
    ASSERT_TRUE(cd != NULL);

    char start[PATH_BUF];
    ASSERT_TRUE(getcwd(start, sizeof start) != NULL);

    char *argv[] = {"cd", "/tmp", "/", NULL};
    int rc = cd(&sh, 3, argv);

    char now[PATH_BUF];
    bool got_cwd = getcwd(now, sizeof now) != NULL;
    shell_stop(&sh);

    ASSERT_EQ(rc, 1);
    ASSERT_TRUE(got_cwd);
    ASSERT_STR_EQ(now, start);
}

TEST(cd_to_missing_directory_fails) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn cd = builtin_lookup("cd");
    ASSERT_TRUE(cd != NULL);

    char start[PATH_BUF];
    ASSERT_TRUE(getcwd(start, sizeof start) != NULL);

    char *argv[] = {"cd", "/nullsh/no/such/directory", NULL};
    int rc = cd(&sh, 2, argv);

    char now[PATH_BUF];
    bool got_cwd = getcwd(now, sizeof now) != NULL;
    shell_stop(&sh);

    ASSERT_EQ(rc, 1);
    ASSERT_TRUE(got_cwd);
    ASSERT_STR_EQ(now, start);
}

TEST(exit_without_args_reuses_the_last_status) {
    Shell sh;
    shell_start(&sh);
    sh.last_status = 7;
    BuiltinFn fn = builtin_lookup("exit");
    ASSERT_TRUE(fn != NULL);

    char *argv[] = {"exit", NULL};
    int rc = fn(&sh, 1, argv);

    ASSERT_EQ(rc, 7);
    ASSERT_TRUE(sh.want_exit);
    ASSERT_EQ(sh.exit_code, 7);
    shell_stop(&sh);
}

TEST(exit_takes_a_numeric_argument) {
    Shell sh;
    shell_start(&sh);
    sh.last_status = 3;
    BuiltinFn fn = builtin_lookup("exit");
    ASSERT_TRUE(fn != NULL);

    char *argv[] = {"exit", "42", NULL};
    int rc = fn(&sh, 2, argv);

    ASSERT_EQ(rc, 42);
    ASSERT_TRUE(sh.want_exit);
    ASSERT_EQ(sh.exit_code, 42);
    shell_stop(&sh);
}

TEST(exit_masks_the_status_to_one_byte) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("exit");
    ASSERT_TRUE(fn != NULL);

    char *big[] = {"exit", "300", NULL};
    ASSERT_EQ(fn(&sh, 2, big), 44);
    ASSERT_EQ(sh.exit_code, 44);

    char *negative[] = {"exit", "-1", NULL};
    ASSERT_EQ(fn(&sh, 2, negative), 255);
    ASSERT_EQ(sh.exit_code, 255);
    ASSERT_TRUE(sh.want_exit);
    shell_stop(&sh);
}

TEST(exit_still_leaves_on_a_non_numeric_argument) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("exit");
    ASSERT_TRUE(fn != NULL);

    char *argv[] = {"exit", "abc", NULL};
    int rc = fn(&sh, 2, argv);

    ASSERT_EQ(rc, 2);
    ASSERT_TRUE(sh.want_exit);
    ASSERT_EQ(sh.exit_code, 2);
    shell_stop(&sh);
}

TEST(exit_rejects_a_partly_numeric_argument) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("exit");
    ASSERT_TRUE(fn != NULL);

    char *trailing[] = {"exit", "12x", NULL};
    ASSERT_EQ(fn(&sh, 2, trailing), 2);
    ASSERT_TRUE(sh.want_exit);

    Shell empty_arg;
    shell_start(&empty_arg);
    char *blank[] = {"exit", "", NULL};
    ASSERT_EQ(fn(&empty_arg, 2, blank), 2);
    ASSERT_TRUE(empty_arg.want_exit);
    ASSERT_EQ(empty_arg.exit_code, 2);

    shell_stop(&empty_arg);
    shell_stop(&sh);
}

TEST(exit_with_too_many_arguments_stays_in_the_shell) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("exit");
    ASSERT_TRUE(fn != NULL);

    char *argv[] = {"exit", "1", "2", NULL};
    int rc = fn(&sh, 3, argv);

    ASSERT_EQ(rc, 1);
    ASSERT_TRUE(!sh.want_exit);
    ASSERT_EQ(sh.exit_code, 0);
    shell_stop(&sh);
}

TEST(export_sets_a_variable) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("export");
    ASSERT_TRUE(fn != NULL);

    char *argv[] = {"export", "NULLSH_T1=hello", NULL};
    int rc = fn(&sh, 2, argv);
    const char *value = getenv("NULLSH_T1");
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(value, "hello");

    char *overwrite[] = {"export", "NULLSH_T1=again", NULL};
    ASSERT_EQ(fn(&sh, 2, overwrite), 0);
    ASSERT_STR_EQ(getenv("NULLSH_T1"), "again");

    char *empty[] = {"export", "NULLSH_T1=", NULL};
    ASSERT_EQ(fn(&sh, 2, empty), 0);
    ASSERT_STR_EQ(getenv("NULLSH_T1"), "");

    unsetenv("NULLSH_T1");
    shell_stop(&sh);
}

TEST(export_without_args_does_nothing) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("export");
    ASSERT_TRUE(fn != NULL);
    char *argv[] = {"export", NULL};
    ASSERT_EQ(fn(&sh, 1, argv), 0);
    shell_stop(&sh);
}

TEST(export_rejects_invalid_identifiers) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("export");
    ASSERT_TRUE(fn != NULL);

    char *digit_first[] = {"export", "1BAD=x", NULL};
    ASSERT_EQ(fn(&sh, 2, digit_first), 1);
    ASSERT_TRUE(getenv("1BAD") == NULL);

    char *dash[] = {"export", "BAD-NAME=x", NULL};
    ASSERT_EQ(fn(&sh, 2, dash), 1);

    char *no_name[] = {"export", "=x", NULL};
    ASSERT_EQ(fn(&sh, 2, no_name), 1);

    char *empty[] = {"export", "", NULL};
    ASSERT_EQ(fn(&sh, 2, empty), 1);

    shell_stop(&sh);
}

TEST(export_of_a_bare_name_is_a_no_op) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("export");
    ASSERT_TRUE(fn != NULL);

    unsetenv("NULLSH_T2");
    char *bare[] = {"export", "NULLSH_T2", NULL};
    ASSERT_EQ(fn(&sh, 2, bare), 0);
    ASSERT_TRUE(getenv("NULLSH_T2") == NULL);

    setenv("NULLSH_T2", "kept", 1);
    ASSERT_EQ(fn(&sh, 2, bare), 0);
    ASSERT_STR_EQ(getenv("NULLSH_T2"), "kept");

    unsetenv("NULLSH_T2");
    shell_stop(&sh);
}

TEST(export_keeps_going_past_a_bad_argument) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("export");
    ASSERT_TRUE(fn != NULL);

    char *argv[] = {"export", "NULLSH_T3=a", "9BAD=b", "NULLSH_T4=c", NULL};
    int rc = fn(&sh, 4, argv);

    ASSERT_EQ(rc, 1);
    ASSERT_STR_EQ(getenv("NULLSH_T3"), "a");
    ASSERT_STR_EQ(getenv("NULLSH_T4"), "c");

    unsetenv("NULLSH_T3");
    unsetenv("NULLSH_T4");
    shell_stop(&sh);
}

TEST(export_accepts_an_equals_sign_inside_the_value) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("export");
    ASSERT_TRUE(fn != NULL);

    char *argv[] = {"export", "NULLSH_T5=a=b=c", NULL};
    ASSERT_EQ(fn(&sh, 2, argv), 0);
    ASSERT_STR_EQ(getenv("NULLSH_T5"), "a=b=c");

    unsetenv("NULLSH_T5");
    shell_stop(&sh);
}

TEST(unset_removes_variables) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("unset");
    ASSERT_TRUE(fn != NULL);

    setenv("NULLSH_T6", "x", 1);
    setenv("NULLSH_T7", "y", 1);
    char *argv[] = {"unset", "NULLSH_T6", "NULLSH_T7", NULL};
    int rc = fn(&sh, 3, argv);

    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(getenv("NULLSH_T6") == NULL);
    ASSERT_TRUE(getenv("NULLSH_T7") == NULL);
    shell_stop(&sh);
}

TEST(unset_of_an_unknown_name_succeeds) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("unset");
    ASSERT_TRUE(fn != NULL);

    unsetenv("NULLSH_T8");
    char *argv[] = {"unset", "NULLSH_T8", NULL};
    ASSERT_EQ(fn(&sh, 2, argv), 0);

    char *none[] = {"unset", NULL};
    ASSERT_EQ(fn(&sh, 1, none), 0);
    shell_stop(&sh);
}

TEST(unset_rejects_invalid_identifiers_and_continues) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("unset");
    ASSERT_TRUE(fn != NULL);

    setenv("NULLSH_T9", "z", 1);
    char *argv[] = {"unset", "2BAD", "NULLSH_T9", NULL};
    int rc = fn(&sh, 3, argv);

    ASSERT_EQ(rc, 1);
    ASSERT_TRUE(getenv("NULLSH_T9") == NULL);

    char *with_equals[] = {"unset", "NAME=VALUE", NULL};
    ASSERT_EQ(fn(&sh, 2, with_equals), 1);
    shell_stop(&sh);
}

TEST(history_prints_and_returns_zero) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("history");
    ASSERT_TRUE(fn != NULL);

    char *argv[] = {"history", NULL};
    // Empty history is still success.
    ASSERT_EQ(fn(&sh, 1, argv), 0);

    history_add(&sh.history, "echo one");
    history_add(&sh.history, "echo two");
    ASSERT_EQ(history_count(&sh.history), 2);
    ASSERT_EQ(fn(&sh, 1, argv), 0);
    // Printing must not disturb the ring.
    ASSERT_EQ(history_count(&sh.history), 2);
    ASSERT_STR_EQ(history_get(&sh.history, 0), "echo one");
    shell_stop(&sh);
}

TEST(history_rejects_arguments) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("history");
    ASSERT_TRUE(fn != NULL);

    history_add(&sh.history, "echo one");
    char *argv[] = {"history", "10", NULL};
    ASSERT_EQ(fn(&sh, 2, argv), 1);
    ASSERT_EQ(history_count(&sh.history), 1);
    shell_stop(&sh);
}

TEST(help_returns_zero) {
    Shell sh;
    shell_start(&sh);
    BuiltinFn fn = builtin_lookup("help");
    ASSERT_TRUE(fn != NULL);

    char *argv[] = {"help", NULL};
    ASSERT_EQ(fn(&sh, 1, argv), 0);
    ASSERT_TRUE(!sh.want_exit);
    shell_stop(&sh);
}

TEST_MAIN()
