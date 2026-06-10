// The netmon builtin: capture an interface and print one line per packet.

#define _POSIX_C_SOURCE 200809L

#include "netmon.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture.h"
#include "decode.h"
#include "filter.h"
#include "print.h"

#include "../alloc/alloc.h"
#include "../util/str.h"

// Room for a jumbo frame, so nothing is ever truncated by the read buffer.
#define FRAME_MAX 65536
#define PORT_MAX 65535

// A shell killed by SIGINT reports this, and so does netmon.
#define STATUS_SIGINT 130

static volatile sig_atomic_t g_stop;
static struct sigaction g_prev_int;

static void stop_handler(int sig) {
    (void)sig;
    g_stop = 1;
}

// No SA_RESTART: the blocking recv has to come back EINTR for the flag to be read.
static bool sigint_install(void) {
    g_stop = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = stop_handler;
    sa.sa_flags = 0;
    if (sigemptyset(&sa.sa_mask) != 0) {
        return false;
    }
    return sigaction(SIGINT, &sa, &g_prev_int) == 0;
}

static void sigint_restore(void) {
    (void)sigaction(SIGINT, &g_prev_int, NULL);
}

static void netmon_usage(void) {
    fputs("nullsh: netmon: usage: netmon IFACE [--filter tcp|udp] [--port N]\n",
          stderr);
}

static bool parse_port(const char *s, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0 || v < 0 || v > PORT_MAX) {
        return false;
    }
    *out = (int)v;
    return true;
}

// Fills ifname and f, or returns false with nothing printed yet.
static bool parse_args(int argc, char **argv, const char **ifname,
                       NetmonFilter *f) {
    if (argc < 2 || argv[1][0] == '-' || argv[1][0] == '\0') {
        return false;
    }
    *ifname = argv[1];
    f->proto = NM_FILTER_ALL;
    f->port = -1;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "tcp") == 0) {
                f->proto = NM_FILTER_TCP;
            } else if (strcmp(argv[i], "udp") == 0) {
                f->proto = NM_FILTER_UDP;
            } else {
                return false;
            }
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            i++;
            if (!parse_port(argv[i], &f->port)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

typedef struct {
    unsigned long total;
    unsigned long shown;
    unsigned long malformed;
} Counts;

// A write failure means the reader is gone, so the loop stops rather than spin.
static bool emit(const Str *line) {
    if (fputs(line->data, stdout) < 0 || fputc('\n', stdout) < 0) {
        return false;
    }
    // Per line, so a piped or backgrounded netmon streams instead of buffering.
    return fflush(stdout) == 0;
}

static int capture_loop(Capture *cap, const NetmonFilter *f, Counts *n) {
    uint8_t *buf = nsh_malloc(FRAME_MAX);
    Str line;
    str_init(&line);
    int status = 0;

    while (!g_stop) {
        ssize_t got = capture_recv(cap, buf, FRAME_MAX);
        if (got < 0) {
            fprintf(stderr, "nullsh: netmon: %s: recv: %s\n", cap->ifname,
                    strerror(errno));
            status = 1;
            break;
        }
        if (got == 0) {
            continue;
        }
        n->total++;
        Packet p;
        if (decode_frame(buf, (size_t)got, &p) != NSH_OK) {
            n->malformed++;
            continue;
        }
        if (!filter_match(&p, f)) {
            continue;
        }
        packet_format(&p, &line);
        if (!emit(&line)) {
            status = 1;
            break;
        }
        n->shown++;
    }

    str_free(&line);
    nsh_free(buf);
    return status;
}

int netmon_builtin(Shell *sh, int argc, char **argv) {
    (void)sh;
    const char *ifname = NULL;
    NetmonFilter f;
    if (!parse_args(argc, argv, &ifname, &f)) {
        netmon_usage();
        return 1;
    }

    Capture cap;
    if (capture_open(&cap, ifname) != NSH_OK) {
        return 1;
    }

    // Foreground: this runs in the shell, which ignores SIGINT, so netmon borrows it and gives it back.
    // Background: the stage is a forked child back on default dispositions, so a job kill lands here too.
    if (!sigint_install()) {
        fprintf(stderr, "nullsh: netmon: sigaction: %s\n", strerror(errno));
        capture_close(&cap);
        return 1;
    }

    Counts n = {0, 0, 0};
    int status = capture_loop(&cap, &f, &n);
    sigint_restore();
    capture_close(&cap);

    fprintf(stderr, "%lu packets, %lu shown, %lu malformed\n", n.total, n.shown,
            n.malformed);
    fflush(stderr);
    if (status == 0 && g_stop) {
        status = STATUS_SIGINT;
    }
    return status;
}
