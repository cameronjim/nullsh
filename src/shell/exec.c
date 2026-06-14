// Execution: expand the words, then run a builtin or fork, search PATH, exec.

#define _POSIX_C_SOURCE 200809L

#include "exec.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "builtin.h"
#include "eval.h"
#include "expand.h"
#include "func.h"
#include "jobs.h"
#include "redirect.h"
#include "signals.h"
#include "spawn.h"

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

// Everything the expander reads from the shell, as it stands right now.
static ExpandCtx expand_ctx(const Shell *sh) {
    ExpandCtx ctx = {sh->last_status, sh->argc, sh->argv};
    return ctx;
}

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
static NshError build_argv(const Command *c, const ExpandCtx *ctx, char ***out,
                           int *out_argc) {
    size_t n = c->words.len;
    char **argv = nsh_calloc(n + 1, sizeof(*argv));
    int argc = 0;

    for (size_t i = 0; i < n; i++) {
        const Token *w = vec_get(&c->words, i);
        char *text = NULL;
        NshError err = expand_word(w, ctx, &text);
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

// A builtin runs in the shell, so its redirects move the shell's own fds.
static int run_builtin(Shell *sh, const Command *c, BuiltinFn fn, int argc,
                       char **argv) {
    if (!has_redirect(c)) {
        return fn(sh, argc, argv);
    }
    RedirSave save = REDIR_SAVE_INIT;
    ExpandCtx ctx = expand_ctx(sh);
    NshError err = redirect_apply(c, &ctx, &save);
    int status = (err == NSH_OK) ? fn(sh, argc, argv) : redirect_status(err);
    redirect_restore(&save);
    return status;
}

// A function body runs in the shell too, so its redirects take the same save
// and restore path a builtin's do. False means argv[0] names no function.
static bool run_function(Shell *sh, const Command *c, int argc, char **argv,
                         int *status) {
    if (func_lookup(argv[0]) == NULL) {
        return false;
    }
    if (!has_redirect(c)) {
        return eval_maybe_call_function(sh, argc, argv, status);
    }
    RedirSave save = REDIR_SAVE_INIT;
    ExpandCtx ctx = expand_ctx(sh);
    NshError err = redirect_apply(c, &ctx, &save);
    bool ran = true;
    if (err == NSH_OK) {
        ran = eval_maybe_call_function(sh, argc, argv, status);
    } else {
        *status = redirect_status(err);
    }
    redirect_restore(&save);
    return ran;
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

    ExpandCtx ctx = expand_ctx(sh);
    char **argv = NULL;
    int argc = 0;
    if (build_argv(c, &ctx, &argv, &argc) != NSH_OK) {
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
    NshError err = redirect_apply(c, &ctx, NULL);
    if (err != NSH_OK) {
        die_redirect(err);
    }

    BuiltinFn fn = builtin_lookup(argv[0]);
    if (fn != NULL) {
        int status = fn(sh, argc, argv);
        fflush(NULL);
        _exit(status);
    }
    // A function in a pipeline runs here, in the child, and never comes back.
    int status = 0;
    if (eval_maybe_call_function(sh, argc, argv, &status)) {
        fflush(NULL);
        _exit(status);
    }
    exec_argv(argv);
}

// The job table's label. Rebuilt from the expanded words, so quoting and the
// original spacing are lost; only enough to recognise the job is promised.
static char *pipeline_cmdline(const Shell *sh, const Pipeline *pl) {
    ExpandCtx ctx = expand_ctx(sh);
    Str s;
    str_init(&s);
    for (size_t i = 0; i < pl->cmds.len; i++) {
        if (i > 0) {
            str_append(&s, " | ");
        }
        const Command *c = vec_get(&pl->cmds, i);
        for (size_t k = 0; k < c->words.len; k++) {
            if (k > 0) {
                str_push(&s, ' ');
            }
            char *text = NULL;
            if (expand_word(vec_get(&c->words, k), &ctx, &text) != NSH_OK) {
                continue;
            }
            str_append(&s, text);
            nsh_free(text);
        }
    }
    if (pl->background) {
        str_append(&s, " &");
    }
    char *out = str_take(&s);
    str_free(&s);
    return out;
}

static void announce_stopped(const Job *j) {
    fprintf(stderr, "[%d]  Stopped  %s\n", j->id, j->cmdline);
    fflush(stderr);
}

// The terminal goes to the job and always comes back, stopped or finished.
static int foreground_wait(Shell *sh, const Pipeline *pl, pid_t pgid,
                           const pid_t *pids, size_t n) {
    spawn_set_terminal(sh, pgid);
    bool stopped = false;
    int status = spawn_wait_pids(pids, n, &stopped);
    spawn_set_terminal(sh, sh->shell_pgid);
    if (stopped) {
        char *cmdline = pipeline_cmdline(sh, pl);
        Job *j = jobs_add(pgid, pids, n, cmdline);
        nsh_free(cmdline);
        j->state = JOB_STOPPED;
        announce_stopped(j);
    }
    return status;
}

int exec_wait_foreground(Shell *sh, Job *job) {
    if (sh == NULL || job == NULL) {
        return 1;
    }
    spawn_set_terminal(sh, job->pgid);
    bool stopped = false;
    int status = spawn_wait_pids(job->pids, job->nproc, &stopped);
    spawn_set_terminal(sh, sh->shell_pgid);
    if (stopped) {
        job->state = JOB_STOPPED;
        announce_stopped(job);
        return status;
    }
    // jobs.h has no silent removal, so the next prompt's reap prints the Done.
    job->nleft = 0;
    job->state = JOB_DONE;
    return status;
}

// Every stage lands in one new group whose pgid is the first stage's pid.
static size_t fork_stages(Shell *sh, const Pipeline *pl, pid_t *pids,
                          pid_t *pgid, int *fds, size_t nfds) {
    size_t n = pl->cmds.len;
    size_t forked = 0;
    for (size_t i = 0; i < n; i++) {
        const Command *c = vec_get(&pl->cmds, i);
        // Flush first, or the child inherits the buffers and reprints them.
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "nullsh: fork: %s\n", strerror(errno));
            break;
        }
        if (pid == 0) {
            spawn_join_group(0, (*pgid == 0) ? getpid() : *pgid);
            signals_reset_child();
            int in_fd = (i == 0) ? -1 : fds[(i - 1) * 2];
            int out_fd = (i + 1 == n) ? -1 : fds[i * 2 + 1];
            run_stage(sh, c, in_fd, out_fd, fds, nfds);
            _exit(STATUS_NOT_EXEC);
        }
        if (*pgid == 0) {
            *pgid = pid;
        }
        spawn_join_group(pid, *pgid);
        pids[forked++] = pid;
    }
    return forked;
}

// Forks every stage, then either waits for the group or files it as a job.
static int run_forked(Shell *sh, const Pipeline *pl) {
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
    pid_t pgid = 0;
    size_t forked = fork_stages(sh, pl, pids, &pgid, fds, nfds);
    close_fds(fds, nfds);
    nsh_free(fds);

    // A pipeline that never forked a single stage leaves 1 behind.
    int status = 1;
    if (forked > 0 && pl->background) {
        char *cmdline = pipeline_cmdline(sh, pl);
        Job *j = jobs_add(pgid, pids, forked, cmdline);
        nsh_free(cmdline);
        fprintf(stderr, "[%d] %ld\n", j->id, (long)j->pgid);
        fflush(stderr);
        status = 0;
    } else if (forked > 0) {
        status = foreground_wait(sh, pl, pgid, pids, forked);
    }
    nsh_free(pids);
    return status;
}

// One command with no &: a builtin runs in the shell, anything else forks.
static int run_simple(Shell *sh, const Pipeline *pl) {
    const Command *c = vec_get(&pl->cmds, 0);
    ExpandCtx ctx = expand_ctx(sh);
    char **argv = NULL;
    int argc = 0;
    if (build_argv(c, &ctx, &argv, &argc) != NSH_OK) {
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

    // Dispatch order: builtin, then function, then the PATH search.
    BuiltinFn fn = builtin_lookup(argv[0]);
    int status = 0;
    if (fn != NULL) {
        status = run_builtin(sh, c, fn, argc, argv);
    } else if (!run_function(sh, c, argc, argv, &status)) {
        // The child expands again; expansion is pure, so the words match.
        status = run_forked(sh, pl);
    }
    argv_free(argv, argc);
    return status;
}

NshError exec_pipeline(Shell *sh, Pipeline *pl) {
    if (sh == NULL || pl == NULL) {
        return NSH_ERR_INVALID;
    }
    if (pl->cmds.len == 0) {
        return NSH_OK;
    }

    if (pl->cmds.len == 1 && !pl->background) {
        sh->last_status = run_simple(sh, pl);
    } else {
        sh->last_status = run_forked(sh, pl);
    }
    return NSH_OK;
}
