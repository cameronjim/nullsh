// File redirection: open the expanded target, dup2 it onto the standard fd.

#define _POSIX_C_SOURCE 200809L

#include "redirect.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "expand.h"

#include "../alloc/alloc.h"

#define OPEN_PERMS 0666

#define TRUNC_FLAGS (O_WRONLY | O_CREAT | O_TRUNC)
#define APPEND_FLAGS (O_WRONLY | O_CREAT | O_APPEND)

// A close only fails on a bad fd, which is a bug worth hearing about.
static void close_fd(int fd) {
    if (close(fd) != 0) {
        fprintf(stderr, "nullsh: close: %s\n", strerror(errno));
    }
}

static NshError open_onto(const char *path, int flags, int target) {
    int fd = open(path, flags, OPEN_PERMS);
    if (fd < 0) {
        fprintf(stderr, "nullsh: %s: %s\n", path, strerror(errno));
        return NSH_ERR_IO;
    }
    if (fd == target) {
        return NSH_OK;
    }
    if (dup2(fd, target) < 0) {
        fprintf(stderr, "nullsh: %s: %s\n", path, strerror(errno));
        close_fd(fd);
        return NSH_ERR_IO;
    }
    close_fd(fd);
    return NSH_OK;
}

static NshError apply_one(const Token *word, int last_status, int flags,
                          int target) {
    if (word == NULL) {
        return NSH_OK;
    }
    char *path = NULL;
    NshError err = expand_word(word, last_status, &path);
    if (err != NSH_OK) {
        return err;
    }
    err = open_onto(path, flags, target);
    nsh_free(path);
    return err;
}

// The copies live above fd 2, so applying a redirect cannot clobber them.
static NshError save_std(RedirSave *save) {
    for (int fd = 0; fd < 3; fd++) {
        save->fds[fd] = -1;
    }
    for (int fd = 0; fd < 3; fd++) {
        int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (copy < 0) {
            fprintf(stderr, "nullsh: dup: %s\n", strerror(errno));
            return NSH_ERR_IO;
        }
        save->fds[fd] = copy;
    }
    return NSH_OK;
}

NshError redirect_apply(const Command *c, int last_status, RedirSave *save) {
    if (c == NULL) {
        return NSH_ERR_INVALID;
    }
    // Buffered output must leave before the fd underneath it changes.
    fflush(NULL);

    if (save != NULL) {
        NshError err = save_std(save);
        if (err != NSH_OK) {
            return err;
        }
    }

    NshError err = apply_one(c->redir_in, last_status, O_RDONLY, STDIN_FILENO);
    if (err != NSH_OK) {
        return err;
    }
    int out_flags = c->redir_append ? APPEND_FLAGS : TRUNC_FLAGS;
    err = apply_one(c->redir_out, last_status, out_flags, STDOUT_FILENO);
    if (err != NSH_OK) {
        return err;
    }
    return apply_one(c->redir_err, last_status, TRUNC_FLAGS, STDERR_FILENO);
}

void redirect_restore(RedirSave *save) {
    if (save == NULL) {
        return;
    }
    fflush(NULL);
    for (int fd = 0; fd < 3; fd++) {
        int copy = save->fds[fd];
        if (copy < 0) {
            continue;
        }
        if (dup2(copy, fd) < 0) {
            fprintf(stderr, "nullsh: dup2: %s\n", strerror(errno));
        }
        close_fd(copy);
        save->fds[fd] = -1;
    }
}
