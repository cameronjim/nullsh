// Execution. One command per call in phase 1: expand the words, dispatch to a
// builtin if the name is one, otherwise fork and let the child walk PATH by
// hand. The child never returns to the REPL; every failure path there ends in
// _exit so a half-built shell can never appear twice.

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

#include "../alloc/alloc.h"
#include "../util/str.h"
#include "../util/vec.h"

// execve takes the environment explicitly. This is the one the C library keeps
// in step with setenv, which is what the export builtin writes to.
extern char **environ;

// The two statuses every shell agrees on for a command that never ran.
#define STATUS_NOT_FOUND 127
#define STATUS_NOT_EXEC 126

// An expansion that cannot be resolved is a usage error, not a failed command.
#define STATUS_BAD_SUBST 2

// The phase 1 executor runs exactly one plain foreground command. Anything the
// parser accepted but this phase cannot run yet is named here, so the user
// learns when it arrives instead of watching it be silently dropped.
static const char *unsupported_reason(const Pipeline *pl) {
    if (pl->cmds.len > 1) {
        return "pipes arrive in phase 3";
    }
    for (size_t i = 0; i < pl->cmds.len; i++) {
        const Command *c = vec_get(&pl->cmds, i);
        if (c != NULL && (c->redir_in != NULL || c->redir_out != NULL ||
                          c->redir_err != NULL)) {
            return "redirection arrives in phase 3";
        }
    }
    if (pl->background) {
        return "job control arrives in phase 4";
    }
    return NULL;
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

// Expands every word of c into a fresh NULL terminated argv. On failure
// nothing is handed back and nothing is left allocated.
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

// Child side only. Prints the one line a shell owes the user and leaves.
static void die_child(const char *name, const char *reason, int status) {
    fprintf(stderr, "nullsh: %s: %s\n", name, reason);
    fflush(stderr);
    _exit(status);
}

// A name holding a slash names a file directly, so PATH never enters into it.
static void exec_direct(char **argv) {
    execve(argv[0], argv, environ);
    if (errno == ENOENT) {
        die_child(argv[0], "command not found", STATUS_NOT_FOUND);
    }
    die_child(argv[0], strerror(errno), STATUS_NOT_EXEC);
}

// Walks PATH left to right, calling execve on every candidate. The search is
// hand rolled rather than execvp because the error accounting is the point:
// only a search that hit nothing but permission failures reports 126.
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
            // POSIX: an empty PATH element means the current directory.
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
            // The file is there and is not runnable for some other reason,
            // ENOEXEC being the common one. Searching on would only hide it.
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

// Blocks until pid is gone and turns the wait status into a shell status. A
// signal that lands on the shell while it waits must not abandon the child.
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

static int run_external(char **argv) {
    // Buffered output belongs to the shell alone. Flushing first stops the
    // child from inheriting a copy and printing it a second time.
    fflush(NULL);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "nullsh: fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        if (strchr(argv[0], '/') != NULL) {
            exec_direct(argv);
        }
        exec_search_path(argv);
    }
    return wait_for_child(pid);
}

NshError exec_pipeline(Shell *sh, Pipeline *pl) {
    if (sh == NULL || pl == NULL) {
        return NSH_ERR_INVALID;
    }
    if (pl->cmds.len == 0) {
        return NSH_OK;
    }

    const char *later = unsupported_reason(pl);
    if (later != NULL) {
        fprintf(stderr, "nullsh: %s\n", later);
        sh->last_status = 1;
        return NSH_OK;
    }

    const Command *c = vec_get(&pl->cmds, 0);
    char **argv = NULL;
    int argc = 0;
    if (build_argv(c, sh->last_status, &argv, &argc) != NSH_OK) {
        fprintf(stderr, "nullsh: bad substitution\n");
        sh->last_status = STATUS_BAD_SUBST;
        return NSH_OK;
    }
    if (argc == 0) {
        argv_free(argv, argc);
        return NSH_OK;
    }
    if (argv[0][0] == '\0') {
        // Every word expanded away. There is no name to look up, and forking
        // to discover that would only cost a process.
        fprintf(stderr, "nullsh: : command not found\n");
        sh->last_status = STATUS_NOT_FOUND;
        argv_free(argv, argc);
        return NSH_OK;
    }

    BuiltinFn fn = builtin_lookup(argv[0]);
    sh->last_status = (fn != NULL) ? fn(sh, argc, argv) : run_external(argv);

    argv_free(argv, argc);
    return NSH_OK;
}
