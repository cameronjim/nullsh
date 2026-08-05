// nullsh entry point: prompt, read, lex, parse, execute, and the history file.

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "alloc/alloc.h"
#include "shell/edit.h"
#include "shell/exec.h"
#include "shell/jobs.h"
#include "shell/lexer.h"
#include "shell/parser.h"
#include "shell/shell.h"
#include "shell/signals.h"
#include "util/line.h"
#include "util/str.h"

#define HISTORY_CAP 1000
#define HISTORY_FILE "/.nullsh_history"
#define CWD_MAX 4096

// The terminal moves to a high fd so a builtin's redirect of fd 0 cannot lose it.
#define TTY_FD_MIN 10

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

// The tilde replaces HOME only on a whole path component, never in /homework.
// The editor needs the prompt as text, so it is built rather than printed.
static void prompt_build(Str *p) {
    char cwd[CWD_MAX];
    const char *home = getenv("HOME");
    size_t hlen = (home == NULL) ? 0 : strlen(home);
    str_clear(p);
    if (getcwd(cwd, sizeof cwd) == NULL) {
        str_append(p, "nullsh$ ");
        return;
    }
    if (hlen > 0 && strncmp(cwd, home, hlen) == 0 &&
        (cwd[hlen] == '\0' || cwd[hlen] == '/')) {
        str_append(p, "nullsh:~");
        str_append(p, cwd + hlen);
    } else {
        str_append(p, "nullsh:");
        str_append(p, cwd);
    }
    str_append(p, "$ ");
}

// Dispositions are installed either way: a script should not die to a SIGINT
// aimed at its pipeline, and & needs SIGCHLD reaping even with no terminal.
static void shell_setup(Shell *sh) {
    signals_install_shell();
    sh->shell_pgid = getpgrp();
    if (!isatty(STDIN_FILENO)) {
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

// The prompt's garbage collection: drain every pending child event, then talk.
static void reap_jobs(void) {
    if (!signals_chld_take()) {
        return;
    }
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        jobs_update(pid, status);
    }
    jobs_reap_notify(stdout);
}

int main(void) {
    Shell sh = {0};
    sh.tty_fd = -1;
    jobs_init();
    shell_setup(&sh);
    history_init(&sh.history, HISTORY_CAP);
    char *hist_file = history_path();
    if (hist_file != NULL) {
        // A first run has no file yet, which is not an error.
        history_load(&sh.history, hist_file);
    }

    Str line;
    Str prompt;
    str_init(&line);
    str_init(&prompt);
    TokenList tl = {{NULL, 0, 0}};
    Pipeline pl = {{NULL, 0, 0}, false};

    for (;;) {
        reap_jobs();
        NshError err;
        if (sh.interactive) {
            prompt_build(&prompt);
            // Raw mode is entered and left inside the call, so a command that
            // runs next inherits a cooked terminal.
            err = edit_read_line(prompt.data, &sh.history, &line);
        } else {
            err = line_read(stdin, &line);
        }
        if (err == NSH_EOF) {
            if (sh.interactive) {
                fputc('\n', stdout);
            }
            break;
        }
        if (err != NSH_OK) {
            fputs("nullsh: read error on stdin\n", stderr);
            sh.last_status = 1;
            break;
        }
        if (line.len == 0) {
            continue;
        }

        // Recorded before parsing, so a bad line can still be recalled.
        history_add(&sh.history, line.data);
        if (lexer_scan(line.data, &tl) != NSH_OK ||
            parser_parse(&tl, &pl) != NSH_OK) {
            fputs("nullsh: syntax error\n", stderr);
            sh.last_status = 2;
        } else {
            exec_pipeline(&sh, &pl);
        }
        token_list_free(&tl);
        pipeline_free(&pl);
        if (sh.want_exit) {
            break;
        }
    }
    if (hist_file != NULL) {
        if (history_save(&sh.history, hist_file) != NSH_OK) {
            fprintf(stderr, "nullsh: could not save history to %s\n",
                    hist_file);
        }
        nsh_free(hist_file);
    }
    token_list_free(&tl);
    pipeline_free(&pl);
    str_free(&line);
    str_free(&prompt);
    history_free(&sh.history);
    jobs_free_all();
    // Ctrl-D leaves with the status of the last command, like bash.
    return sh.want_exit ? sh.exit_code : sh.last_status;
}
