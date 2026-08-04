// Execution: expand the words, then run a builtin or fork, search PATH, exec.

#define _POSIX_C_SOURCE 200809L

#include "exec.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "builtin.h"
#include "expand.h"
#include "redirect.h"

#include "../alloc/alloc.h"
#include "../util/str.h"
#include "../util/vec.h"

// The environment setenv keeps in step, which is what execve must be handed.
extern char **environ;

// The statuses every shell uses for a command that never ran.
#define STATUS_NOT_FOUND 127
#define STATUS_NOT_EXEC 126

// A bad expansion is a usage error, not a failed command.
#define STATUS_BAD_SUBST 2

static void argv_free(char **argv, int argc) {
    if (argv == NULL) {
        return;
    }
    for (int i = 0; i < argc; i++) {
        nsh_free(argv[i]);
    }
    nsh_free(argv);
}

// On failure nothing is handed back and nothing is left allocated.
static NshError build_argv(const Command *c, int last_status, char ***out,
                           int *out_argc) {
    size_t n = c->words.len;
    char **argv = nsh_calloc(n + 1, sizeof(*argv));
    int argc = 0;

    for (size_t i = 0; i < n; i++) {
        const Token *w = vec_get(&c->words, i);
        char *text = NULL;
        NshError err = expand_word(w, last_status, &text);
        if (err != NSH_OK) {
            argv_free(argv, argc);
            return err;
        }
        argv[argc++] = text;
    }
    argv[argc] = NULL;

    *out = argv;
    *out_argc = argc;
    return NSH_OK;
}

// Child side only: prints one line and leaves.
static void die_child(const char *name, const char *reason, int status) {
    fprintf(stderr, "nullsh: %s: %s\n", name, reason);
    fflush(stderr);
    _exit(status);
}

// A name holding a slash is a path, so PATH is not searched.
static void exec_direct(char **argv) {
    execve(argv[0], argv, environ);
    if (errno == ENOENT) {
        die_child(argv[0], "command not found", STATUS_NOT_FOUND);
    }
    die_child(argv[0], strerror(errno), STATUS_NOT_EXEC);
}

// Hand rolled rather than execvp so only all-permission failures report 126.
static void exec_search_path(char **argv) {
    const char *path = getenv("PATH");
    if (path == NULL) {
        die_child(argv[0], "command not found", STATUS_NOT_FOUND);
    }

    bool denied = false;
    int fatal = 0;
    Str cand;
    str_init(&cand);

    for (const char *p = path;; ) {
        const char *sep = strchr(p, ':');
        size_t len = (sep == NULL) ? strlen(p) : (size_t)(sep - p);

        str_clear(&cand);
        if (len == 0) {
            // POSIX: an empty PATH element is the current directory.
            str_push(&cand, '.');
        } else {
            str_append_n(&cand, p, len);
        }
        str_push(&cand, '/');
        str_append(&cand, argv[0]);

        execve(cand.data, argv, environ);
        if (errno == EACCES) {
            denied = true;
        } else if (errno != ENOENT && errno != ENOTDIR) {
            // ENOEXEC and the like: the file is there, so stop searching.
            fatal = errno;
            break;
        }

        if (sep == NULL) {
            break;
        }
        p = sep + 1;
    }

    str_free(&cand);
    if (fatal != 0) {
        die_child(argv[0], strerror(fatal), STATUS_NOT_EXEC);
    }
    if (denied) {
        die_child(argv[0], "permission denied", STATUS_NOT_EXEC);
    }
    die_child(argv[0], "command not found", STATUS_NOT_FOUND);
}

// Child side only: the last thing a stage does.
static void exec_argv(char **argv) {
    if (strchr(argv[0], '/') != NULL) {
        exec_direct(argv);
    }
    exec_search_path(argv);
}

// waitpid is retried on EINTR so a signal never abandons the child.
static int wait_for_child(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "nullsh: waitpid: %s\n", strerror(errno));
            return 1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

static bool has_redirect(const Command *c) {
    return c->redir_in != NULL || c->redir_out != NULL || c->redir_err != NULL;
}

// An open failure is a command that never ran; the rest came from expansion.
static int redirect_status(NshError err) {
    if (err == NSH_ERR_IO) {
        return 1;
    }
    fprintf(stderr, "nullsh: bad substitution\n");
    return STATUS_BAD_SUBST;
}

// Child side only.
static void die_redirect(NshError err) {
    int status = redirect_status(err);
    fflush(stderr);
    _exit(status);
}

static int run_external(const Command *c, int last_status, char **argv) {
    // Flush first, or the child inherits the buffers and reprints them.
    fflush(NULL);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "nullsh: fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        NshError err = redirect_apply(c, last_status, NULL);
        if (err != NSH_OK) {
            die_redirect(err);
        }
        exec_argv(argv);
    }
    return wait_for_child(pid);
}

// A builtin runs in the shell, so its redirects move the shell's own fds.
static int run_builtin(Shell *sh, const Command *c, BuiltinFn fn, int argc,
                       char **argv) {
    if (!has_redirect(c)) {
        return fn(sh, argc, argv);
    }
    RedirSave save = REDIR_SAVE_INIT;
    NshError err = redirect_apply(c, sh->last_status, &save);
    int status = (err == NSH_OK) ? fn(sh, argc, argv) : redirect_status(err);
    redirect_restore(&save);
    return status;
}

static int run_simple(Shell *sh, const Command *c) {
    char **argv = NULL;
    int argc = 0;
    if (build_argv(c, sh->last_status, &argv, &argc) != NSH_OK) {
        fprintf(stderr, "nullsh: bad substitution\n");
        return STATUS_BAD_SUBST;
    }
    if (argc == 0) {
        argv_free(argv, argc);
        return sh->last_status;
    }
    if (argv[0][0] == '\0') {
        // Every word expanded away, so there is no name to look up.
        fprintf(stderr, "nullsh: : command not found\n");
        argv_free(argv, argc);
        return STATUS_NOT_FOUND;
    }

    BuiltinFn fn = builtin_lookup(argv[0]);
    int status = (fn != NULL) ? run_builtin(sh, c, fn, argc, argv)
                              : run_external(c, sh->last_status, argv);
    argv_free(argv, argc);
    return status;
}

// Pipe i lives in fds[2 * i] and fds[2 * i + 1]; a closed slot holds -1.
static void close_fds(int *fds, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (fds[i] >= 0 && close(fds[i]) != 0) {
            fprintf(stderr, "nullsh: close: %s\n", strerror(errno));
        }
        fds[i] = -1;
    }
}

// One stage, in the child. An fd of -1 means that end keeps what it inherited.
static void run_stage(Shell *sh, const Command *c, int in_fd, int out_fd,
                      int *fds, size_t nfds) {
    if (in_fd >= 0 && dup2(in_fd, STDIN_FILENO) < 0) {
        die_child("dup2", strerror(errno), 1);
    }
    if (out_fd >= 0 && dup2(out_fd, STDOUT_FILENO) < 0) {
        die_child("dup2", strerror(errno), 1);
    }
    // Every spare copy of a write end must go or no reader ever sees EOF.
    close_fds(fds, nfds);

    char **argv = NULL;
    int argc = 0;
    if (build_argv(c, sh->last_status, &argv, &argc) != NSH_OK) {
        fprintf(stderr, "nullsh: bad substitution\n");
        fflush(stderr);
        _exit(STATUS_BAD_SUBST);
    }
    if (argc == 0) {
        _exit(0);
    }
    if (argv[0][0] == '\0') {
        die_child("", "command not found", STATUS_NOT_FOUND);
    }

    // File redirects land after the pipe wiring, so an explicit > wins.
    NshError err = redirect_apply(c, sh->last_status, NULL);
    if (err != NSH_OK) {
        die_redirect(err);
    }

    BuiltinFn fn = builtin_lookup(argv[0]);
    if (fn != NULL) {
        int status = fn(sh, argc, argv);
        fflush(NULL);
        _exit(status);
    }
    exec_argv(argv);
}

static int run_pipeline(Shell *sh, const Pipeline *pl) {
    size_t n = pl->cmds.len;
    size_t nfds = (n - 1) * 2;
    int *fds = nsh_calloc(nfds, sizeof(*fds));
    for (size_t i = 0; i < nfds; i++) {
        fds[i] = -1;
    }
    for (size_t i = 0; i + 1 < n; i++) {
        if (pipe(&fds[i * 2]) != 0) {
            fprintf(stderr, "nullsh: pipe: %s\n", strerror(errno));
            close_fds(fds, nfds);
            nsh_free(fds);
            return 1;
        }
    }

    pid_t *pids = nsh_calloc(n, sizeof(*pids));
    size_t forked = 0;
    for (size_t i = 0; i < n; i++) {
        const Command *c = vec_get(&pl->cmds, i);
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "nullsh: fork: %s\n", strerror(errno));
            break;
        }
        if (pid == 0) {
            int in_fd = (i == 0) ? -1 : fds[(i - 1) * 2];
            int out_fd = (i + 1 == n) ? -1 : fds[i * 2 + 1];
            run_stage(sh, c, in_fd, out_fd, fds, nfds);
            _exit(STATUS_NOT_EXEC);
        }
        pids[forked++] = pid;
    }

    close_fds(fds, nfds);
    nsh_free(fds);

    // The last stage owns $?; a stage that never forked leaves 1 behind.
    int status = 1;
    for (size_t i = 0; i < forked; i++) {
        int reaped = wait_for_child(pids[i]);
        if (i + 1 == n) {
            status = reaped;
        }
    }
    nsh_free(pids);
    return status;
}

NshError exec_pipeline(Shell *sh, Pipeline *pl) {
    if (sh == NULL || pl == NULL) {
        return NSH_ERR_INVALID;
    }
    if (pl->cmds.len == 0) {
        return NSH_OK;
    }
    if (pl->background) {
        fprintf(stderr, "nullsh: job control arrives in phase 4\n");
        sh->last_status = 1;
        return NSH_OK;
    }

    if (pl->cmds.len == 1) {
        sh->last_status = run_simple(sh, vec_get(&pl->cmds, 0));
    } else {
        sh->last_status = run_pipeline(sh, pl);
    }
    return NSH_OK;
}
