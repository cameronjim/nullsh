// nullsh entry point: startup, the history file, and the choice of driver.

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "alloc/alloc.h"
#include "shell/func.h"
#include "shell/jobs.h"
#include "shell/run.h"
#include "shell/shell.h"
#include "shell/signals.h"
#include "util/str.h"

#define HISTORY_CAP 1000
#define HISTORY_FILE "/.nullsh_history"

// A script that cannot be opened is a command that was never found.
#define EXIT_NO_SCRIPT 127

// The terminal moves to a high fd so a builtin's redirect of fd 0 cannot lose it.
#define TTY_FD_MIN 10

// $0 answers even when there is no script path and no argv worth borrowing.
static char *self_argv[] = {"nullsh", NULL};

// NULL when HOME says nothing. Caller nsh_frees the result.
static char *history_path(void) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return NULL;
    }
    Str p;
    str_init(&p);
    str_append(&p, home);
    str_append(&p, HISTORY_FILE);
    char *out = str_take(&p);
    str_free(&p);
    return out;
}

// Dispositions are installed either way: a script should not die to a SIGINT
// aimed at its pipeline, and & needs SIGCHLD reaping even with no terminal.
// A script names its own input, so a tty on fd 0 does not make it interactive
// and the terminal handoff stays where the invoking shell left it.
static void shell_setup(Shell *sh, bool may_prompt) {
    signals_install_shell();
    sh->shell_pgid = getpgrp();
    if (!may_prompt || !isatty(STDIN_FILENO)) {
        return;
    }
    sh->interactive = true;
    int fd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, TTY_FD_MIN);
    sh->tty_fd = (fd >= 0) ? fd : STDIN_FILENO;
    // A session leader cannot leave its group, and then it already owns one.
    if (setpgid(0, 0) == 0) {
        sh->shell_pgid = getpid();
    }
    if (tcsetpgrp(sh->tty_fd, sh->shell_pgid) != 0) {
        fprintf(stderr, "nullsh: tcsetpgrp: %s\n", strerror(errno));
    }
}

int main(int argc, char **argv) {
    Shell sh = {0};
    sh.tty_fd = -1;
    jobs_init();

    FILE *script = NULL;
    if (argc > 1) {
        script = fopen(argv[1], "r");
        if (script == NULL) {
            fprintf(stderr, "nullsh: %s: %s\n", argv[1], strerror(errno));
            return EXIT_NO_SCRIPT;
        }
        // $0 is the script path and $1.. are what came after it.
        sh.argc = argc - 1;
        sh.argv = argv + 1;
    } else {
        sh.argc = 1;
        sh.argv = self_argv;
    }

    shell_setup(&sh, script == NULL);
    history_init(&sh.history, HISTORY_CAP);
    // A script must not disturb the history file the prompt owns.
    char *hist_file = (script == NULL) ? history_path() : NULL;
    if (hist_file != NULL) {
        // A first run has no file yet, which is not an error.
        history_load(&sh.history, hist_file);
    }

    if (script != NULL) {
        run_stream(&sh, script);
        fclose(script);
    } else if (sh.interactive) {
        run_interactive(&sh);
    } else {
        run_stream(&sh, stdin);
    }

    if (hist_file != NULL) {
        if (history_save(&sh.history, hist_file) != NSH_OK) {
            fprintf(stderr, "nullsh: could not save history to %s\n",
                    hist_file);
        }
        nsh_free(hist_file);
    }
    history_free(&sh.history);
    func_free_all();
    jobs_free_all();
    // Ctrl-D leaves with the status of the last command, like bash.
    return sh.want_exit ? sh.exit_code : sh.last_status;
}
