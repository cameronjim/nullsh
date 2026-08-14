// One UDP DNS exchange: send, wait with a timeout, retry. No parsing here.

#define _POSIX_C_SOURCE 200809L

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_int;
static struct sigaction g_prev_int;

static void int_handler(int sig) {
    (void)sig;
    g_int = 1;
}

// No SA_RESTART: poll has to come back EINTR for the flag to be worth reading.
static bool sigint_install(void) {
    g_int = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = int_handler;
    sa.sa_flags = 0;
    if (sigemptyset(&sa.sa_mask) != 0) {
        return false;
    }
    return sigaction(SIGINT, &sa, &g_prev_int) == 0;
}

static void sigint_restore(void) {
    (void)sigaction(SIGINT, &g_prev_int, NULL);
}

static bool now_ms(int64_t *out) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return false;
    }
    *out = (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
    return true;
}

// Non-blocking so a poll wakeup carrying only POLLERR can never park recvfrom.
static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// One send plus the wait it owns. NSH_ERR_NOT_FOUND means the deadline expired.
static NshError one_try(int fd, const struct sockaddr_in *dst,
                        const uint8_t *query, size_t query_len, uint8_t *reply,
                        size_t cap, size_t *reply_len, int timeout_ms) {
    ssize_t sent = sendto(fd, query, query_len, 0, (const struct sockaddr *)dst,
                          (socklen_t)sizeof *dst);
    if (sent < 0 || (size_t)sent != query_len) {
        return NSH_ERR_IO;
    }

    int64_t deadline = 0;
    if (!now_ms(&deadline)) {
        return NSH_ERR_IO;
    }
    deadline += timeout_ms;

    for (;;) {
        int64_t now = 0;
        if (!now_ms(&now)) {
            return NSH_ERR_IO;
        }
        int64_t left = deadline - now;
        if (left < 0) {
            left = 0;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ready = poll(&pfd, 1, (int)left);
        if (ready < 0) {
            if (errno != EINTR) {
                return NSH_ERR_IO;
            }
            // EINTR with the flag clear is some other signal, so the wait resumes.
            if (g_int) {
                return NSH_INTERRUPT;
            }
            continue;
        }
        if (ready == 0) {
            return NSH_ERR_NOT_FOUND;
        }

        struct sockaddr_in from;
        memset(&from, 0, sizeof from);
        socklen_t from_len = (socklen_t)sizeof from;
        // The kernel clips a datagram longer than cap and drops the rest.
        ssize_t got =
            recvfrom(fd, reply, cap, 0, (struct sockaddr *)&from, &from_len);
        if (got < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            if (errno != EINTR) {
                return NSH_ERR_IO;
            }
            if (g_int) {
                return NSH_INTERRUPT;
            }
            continue;
        }
        // Anything not from the queried address and port is dropped, deadline unchanged.
        if (from_len < (socklen_t)sizeof from || from.sin_family != AF_INET ||
            from.sin_addr.s_addr != dst->sin_addr.s_addr ||
            from.sin_port != dst->sin_port) {
            continue;
        }

        *reply_len = (size_t)got;
        return NSH_OK;
    }
}

NshError dns_exchange(const char *server, uint16_t port, const uint8_t *query,
                      size_t query_len, uint8_t *reply, size_t cap,
                      size_t *reply_len, int timeout_ms, int tries) {
    if (server == NULL || query == NULL || reply == NULL || reply_len == NULL) {
        return NSH_ERR_INVALID;
    }
    *reply_len = 0;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    // Socket plumbing, so htons here; the wire codec in dns.c writes its own shifts.
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, server, &dst.sin_addr) != 1) {
        return NSH_ERR_INVALID;
    }

    if (tries < 1) {
        tries = 1;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return NSH_ERR_IO;
    }
    if (!set_nonblocking(fd)) {
        (void)close(fd);
        return NSH_ERR_IO;
    }
    // The shell ignores SIGINT, so the exchange borrows it and hands it back below.
    if (!sigint_install()) {
        (void)close(fd);
        return NSH_ERR_IO;
    }

    NshError rc = NSH_ERR_NOT_FOUND;
    for (int i = 0; i < tries; i++) {
        rc = one_try(fd, &dst, query, query_len, reply, cap, reply_len,
                     timeout_ms);
        if (rc != NSH_ERR_NOT_FOUND) {
            break;
        }
    }

    sigint_restore();
    (void)close(fd);
    return rc;
}
