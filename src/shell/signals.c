// Signal dispositions for the shell and for children after fork, plus the SIGCHLD flag.

#define _POSIX_C_SOURCE 200809L

#include "signals.h"

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Terminal signals the shell ignores; SIGCHLD gets its own disposition.
static const int IGNORED_SIGNALS[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTOU};

#define IGNORED_COUNT (sizeof IGNORED_SIGNALS / sizeof IGNORED_SIGNALS[0])

static volatile sig_atomic_t g_chld_flag;

// A handler may only touch a volatile sig_atomic_t; the rest is async-signal-unsafe.
static void chld_handler(int sig) {
    (void)sig;
    g_chld_flag = 1;
}

// A shell with broken dispositions is unusable, so a failure here aborts.
static void set_disposition(int sig, void (*handler)(int), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = flags;
    if (sigemptyset(&sa.sa_mask) != 0 || sigaction(sig, &sa, NULL) != 0) {
        fprintf(stderr, "nullsh: sigaction(%d): %s\n", sig, strerror(errno));
        abort();
    }
}

void signals_install_shell(void) {
    // SA_RESTART keeps a SIGCHLD arrival from turning blocking syscalls into EINTR.
    set_disposition(SIGCHLD, chld_handler, SA_RESTART);
    for (size_t i = 0; i < IGNORED_COUNT; i++) {
        set_disposition(IGNORED_SIGNALS[i], SIG_IGN, 0);
    }
}

void signals_reset_child(void) {
    set_disposition(SIGCHLD, SIG_DFL, 0);
    for (size_t i = 0; i < IGNORED_COUNT; i++) {
        set_disposition(IGNORED_SIGNALS[i], SIG_DFL, 0);
    }
}

int signals_chld_take(void) {
    // A SIGCHLD landing between the read and the clear is benign: WNOHANG still reaps it.
    int fired = g_chld_flag != 0;
    g_chld_flag = 0;
    return fired;
}
