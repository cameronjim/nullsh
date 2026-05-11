// Process group placement, tcsetpgrp handoff, and waiting on a group of children.

#define _POSIX_C_SOURCE 200809L

#include "spawn.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Parent and child both call setpgid so the group exists before either races on.
void spawn_join_group(pid_t pid, pid_t pgid) {
    if (setpgid(pid, pgid) == 0) {
        return;
    }
    // EACCES means the child already exec'd, ESRCH that it already left.
    if (errno != EACCES && errno != ESRCH) {
        fprintf(stderr, "nullsh: setpgid: %s\n", strerror(errno));
    }
}

void spawn_set_terminal(const Shell *sh, pid_t pgid) {
    if (sh == NULL || !sh->interactive || sh->tty_fd < 0 || pgid <= 0) {
        return;
    }
    // SIGTTOU is ignored shell wide, so this call never stops the shell itself.
    if (tcsetpgrp(sh->tty_fd, pgid) != 0 && errno != ENOTTY && errno != EPERM) {
        fprintf(stderr, "nullsh: tcsetpgrp: %s\n", strerror(errno));
    }
}

// One wait status in the form $? uses.
static int status_code(int wstatus) {
    if (WIFEXITED(wstatus)) {
        return WEXITSTATUS(wstatus);
    }
    if (WIFSIGNALED(wstatus)) {
        return 128 + WTERMSIG(wstatus);
    }
    if (WIFSTOPPED(wstatus)) {
        return 128 + WSTOPSIG(wstatus);
    }
    return 1;
}

int spawn_wait_pids(const pid_t *pids, size_t n, bool *stopped) {
    int status = 0;
    *stopped = false;
    for (size_t i = 0; i < n; i++) {
        if (pids[i] <= 0) {
            continue;
        }
        // Once one stage has stopped the rest must not block behind it.
        int flags = WUNTRACED | (*stopped ? WNOHANG : 0);
        int wstatus = 0;
        pid_t got;
        while ((got = waitpid(pids[i], &wstatus, flags)) < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            // ECHILD: the prompt's reap loop already collected this one.
            continue;
        }
        if (WIFSTOPPED(wstatus)) {
            *stopped = true;
        }
        if (i + 1 == n) {
            status = status_code(wstatus);
        }
    }
    return status;
}
