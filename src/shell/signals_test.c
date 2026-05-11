// Tests for the signals module: the SIGCHLD flag, the ignores, reset in a forked child.

#define _POSIX_C_SOURCE 200809L

#include "signals.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../tests/harness.h"

// Tests share the one flag, so each starts from installed dispositions and a clear flag.
static void fresh(void) {
    signals_install_shell();
    signals_chld_take();
}

// The child never returns into the harness; every path ends in _exit or a signal.
static pid_t spawn_raising_sigint(bool reset) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        if (reset) {
            signals_reset_child();
        }
        raise(SIGINT);
        _exit(0);
    }
    return pid;
}

TEST(sigchld_sets_the_flag_once) {
    fresh();
    ASSERT_EQ(raise(SIGCHLD), 0);
    ASSERT_TRUE(signals_chld_take() != 0);
    ASSERT_EQ(signals_chld_take(), 0);
}

// The flag is a flag, not a counter: two arrivals still take as one.
TEST(two_sigchlds_collapse_into_one_take) {
    fresh();
    ASSERT_EQ(raise(SIGCHLD), 0);
    ASSERT_EQ(raise(SIGCHLD), 0);
    ASSERT_TRUE(signals_chld_take() != 0);
    ASSERT_EQ(signals_chld_take(), 0);
}

// Surviving all four is the assertion: a missed ignore kills or stops this process.
TEST(terminal_signals_are_ignored) {
    fresh();
    ASSERT_EQ(kill(getpid(), SIGINT), 0);
    ASSERT_EQ(kill(getpid(), SIGQUIT), 0);
    ASSERT_EQ(kill(getpid(), SIGTSTP), 0);
    ASSERT_EQ(kill(getpid(), SIGTTOU), 0);
    ASSERT_EQ(signals_chld_take(), 0);
}

TEST(reset_child_undoes_the_sigint_ignore) {
    fresh();
    pid_t pid = spawn_raising_sigint(true);
    ASSERT_TRUE(pid > 0);
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGINT);
}

TEST(child_without_reset_inherits_the_ignore) {
    fresh();
    pid_t pid = spawn_raising_sigint(false);
    ASSERT_TRUE(pid > 0);
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
}

TEST(install_is_idempotent) {
    signals_install_shell();
    signals_install_shell();
    signals_chld_take();
    ASSERT_EQ(kill(getpid(), SIGINT), 0);
    ASSERT_EQ(raise(SIGCHLD), 0);
    ASSERT_TRUE(signals_chld_take() != 0);
    ASSERT_EQ(signals_chld_take(), 0);
}

TEST_MAIN()
