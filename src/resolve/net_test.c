// Tests for the UDP exchange: round trip, resend, source filtering, Ctrl-C.

#define _POSIX_C_SOURCE 200809L

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../tests/harness.h"

#define LOOPBACK "127.0.0.1"
#define LOOPBACK_ALIAS "127.0.0.2"

// Nothing here is a real DNS message; net.c never looks inside the bytes.
static const uint8_t QUERY[] = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01};
static const uint8_t ANSWER[] = {0x12, 0x34, 0x81, 0x80, 0xAB, 0xCD, 0x7F};
static const uint8_t DECOY[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11};

typedef enum {
    SRV_ANSWER,      // answers every datagram
    SRV_SILENT,      // never answers
    SRV_SECOND,      // answers from the second datagram on
    SRV_DECOY_FIRST, // decoy from the alt port first, real answer on the resend
    SRV_DECOY_THEN,  // decoy from the alt port then the real answer, back to back
    SRV_ALIAS_FIRST  // decoy from the right port at 127.0.0.2, real answer next
} SrvMode;

// The port travels back over a pipe, and with it whether 127.0.0.2 was bindable.
typedef struct {
    uint16_t port;
    uint8_t alias_ok;
} Hello;

typedef struct {
    pid_t pid;
    uint16_t port;
    bool alias_ok;
    int ctrl_w; // closing this tells the child to report and leave
    int rep_r;  // the child's datagram count arrives here
} Server;

static long long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// dup hands back the lowest free descriptor, so an unchanged number means no leak.
static int lowest_free_fd(void) {
    int fd = dup(0);
    if (fd >= 0) {
        (void)close(fd);
    }
    return fd;
}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static bool bind_at(int fd, const char *ip, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        return false;
    }
    return bind(fd, (const struct sockaddr *)&a, (socklen_t)sizeof a) == 0;
}

static bool bind_ephemeral(int fd, uint16_t *port) {
    if (!bind_at(fd, LOOPBACK, 0)) {
        return false;
    }
    struct sockaddr_in got;
    memset(&got, 0, sizeof got);
    socklen_t len = (socklen_t)sizeof got;
    if (getsockname(fd, (struct sockaddr *)&got, &len) != 0) {
        return false;
    }
    *port = ntohs(got.sin_port);
    return true;
}

static void reply_to(int fd, const struct sockaddr_in *to, const uint8_t *buf,
                     size_t len) {
    ssize_t n = sendto(fd, buf, len, 0, (const struct sockaddr *)to,
                       (socklen_t)sizeof *to);
    (void)n;
}

// The child never returns into the harness; every path ends in _exit.
static void server_child(SrvMode mode, const uint8_t *payload, size_t len,
                         int port_w, int ctrl_r, int rep_w) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int alt = socket(AF_INET, SOCK_DGRAM, 0);
    int alias = socket(AF_INET, SOCK_DGRAM, 0);
    uint16_t port = 0;
    uint16_t alt_port = 0;
    if (sock < 0 || alt < 0 || alias < 0 || !bind_ephemeral(sock, &port) ||
        !bind_ephemeral(alt, &alt_port)) {
        _exit(1);
    }

    // Same port as the real server, different address: only the address differs.
    Hello hello;
    hello.port = port;
    hello.alias_ok = bind_at(alias, LOOPBACK_ALIAS, port) ? 1 : 0;
    if (write(port_w, &hello, sizeof hello) != (ssize_t)sizeof hello) {
        _exit(1);
    }

    unsigned count = 0;
    for (;;) {
        struct pollfd p[2];
        p[0].fd = ctrl_r;
        p[0].events = 0;
        p[0].revents = 0;
        p[1].fd = sock;
        p[1].events = POLLIN;
        p[1].revents = 0;
        // The 5s cap is a safety net: a wedged child must never wedge the suite.
        int ready = poll(p, 2, 5000);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0 || p[0].revents != 0) {
            break;
        }
        if ((p[1].revents & POLLIN) == 0) {
            continue;
        }

        struct sockaddr_in from;
        memset(&from, 0, sizeof from);
        socklen_t from_len = (socklen_t)sizeof from;
        uint8_t buf[1024];
        ssize_t got = recvfrom(sock, buf, sizeof buf, 0,
                               (struct sockaddr *)&from, &from_len);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        count++;

        switch (mode) {
        case SRV_ANSWER:
            reply_to(sock, &from, payload, len);
            break;
        case SRV_SILENT:
            break;
        case SRV_SECOND:
            if (count >= 2) {
                reply_to(sock, &from, payload, len);
            }
            break;
        case SRV_DECOY_FIRST:
            if (count == 1) {
                reply_to(alt, &from, DECOY, sizeof DECOY);
            } else {
                reply_to(sock, &from, payload, len);
            }
            break;
        case SRV_DECOY_THEN:
            reply_to(alt, &from, DECOY, sizeof DECOY);
            reply_to(sock, &from, payload, len);
            break;
        case SRV_ALIAS_FIRST:
            if (count == 1) {
                reply_to(alias, &from, DECOY, sizeof DECOY);
            } else {
                reply_to(sock, &from, payload, len);
            }
            break;
        }
    }

    ssize_t w = write(rep_w, &count, sizeof count);
    (void)w;
    _exit(0);
}

static bool server_start(Server *sv, SrvMode mode, const uint8_t *payload,
                         size_t len) {
    int portp[2];
    int ctrlp[2];
    int repp[2];
    if (pipe(portp) != 0) {
        return false;
    }
    if (pipe(ctrlp) != 0 || pipe(repp) != 0) {
        return false;
    }

    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        (void)close(portp[0]);
        (void)close(ctrlp[1]);
        (void)close(repp[0]);
        server_child(mode, payload, len, portp[1], ctrlp[0], repp[1]);
        _exit(0);
    }
    (void)close(portp[1]);
    (void)close(ctrlp[0]);
    (void)close(repp[1]);
    if (pid < 0) {
        return false;
    }

    Hello hello;
    memset(&hello, 0, sizeof hello);
    bool ok = read(portp[0], &hello, sizeof hello) == (ssize_t)sizeof hello;
    (void)close(portp[0]);
    sv->pid = pid;
    sv->port = hello.port;
    sv->alias_ok = hello.alias_ok != 0;
    sv->ctrl_w = ctrlp[1];
    sv->rep_r = repp[0];
    return ok && hello.port != 0;
}

// Closing the control pipe is the stop signal; the reply is the datagram count.
static bool server_stop(Server *sv, unsigned *count) {
    (void)close(sv->ctrl_w);
    unsigned c = 0;
    bool ok = read(sv->rep_r, &c, sizeof c) == (ssize_t)sizeof c;
    (void)close(sv->rep_r);
    int status = 0;
    (void)waitpid(sv->pid, &status, 0);
    *count = c;
    return ok;
}

static void noop_handler(int sig) {
    (void)sig;
}

static bool install_handler(int sig, void (*fn)(int), struct sigaction *prev) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sa.sa_flags = 0;
    if (sigemptyset(&sa.sa_mask) != 0) {
        return false;
    }
    return sigaction(sig, &sa, prev) == 0;
}

static pid_t spawn_signal_after(int sig, long ms) {
    fflush(NULL);
    pid_t target = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        sleep_ms(ms);
        (void)kill(target, sig);
        _exit(0);
    }
    return pid;
}

TEST(round_trip_returns_the_canned_bytes) {
    Server sv;
    ASSERT_TRUE(server_start(&sv, SRV_ANSWER, ANSWER, sizeof ANSWER));

    int before = lowest_free_fd();
    uint8_t buf[512];
    size_t len = 0;
    ASSERT_EQ(dns_exchange(LOOPBACK, sv.port, QUERY, sizeof QUERY, buf,
                           sizeof buf, &len, 500, 2),
              NSH_OK);
    ASSERT_EQ(len, sizeof ANSWER);
    ASSERT_EQ(memcmp(buf, ANSWER, sizeof ANSWER), 0);
    ASSERT_EQ(lowest_free_fd(), before);

    unsigned count = 0;
    ASSERT_TRUE(server_stop(&sv, &count));
    ASSERT_EQ(count, 1);
}

TEST(silence_costs_one_timeout_per_try) {
    Server sv;
    ASSERT_TRUE(server_start(&sv, SRV_SILENT, ANSWER, sizeof ANSWER));

    uint8_t buf[512];
    size_t len = 7;
    long long start = now_ms();
    ASSERT_EQ(dns_exchange(LOOPBACK, sv.port, QUERY, sizeof QUERY, buf,
                           sizeof buf, &len, 60, 3),
              NSH_ERR_NOT_FOUND);
    long long spent = now_ms() - start;
    ASSERT_EQ(len, 0);
    ASSERT_TRUE(spent >= 150);

    unsigned count = 0;
    ASSERT_TRUE(server_stop(&sv, &count));
    ASSERT_EQ(count, 3);
}

TEST(tries_below_one_acts_as_one) {
    Server sv;
    ASSERT_TRUE(server_start(&sv, SRV_SILENT, ANSWER, sizeof ANSWER));

    uint8_t buf[512];
    size_t len = 0;
    ASSERT_EQ(dns_exchange(LOOPBACK, sv.port, QUERY, sizeof QUERY, buf,
                           sizeof buf, &len, 40, 0),
              NSH_ERR_NOT_FOUND);

    unsigned count = 0;
    ASSERT_TRUE(server_stop(&sv, &count));
    ASSERT_EQ(count, 1);
}

TEST(resend_wins_when_only_the_second_datagram_is_answered) {
    Server sv;
    ASSERT_TRUE(server_start(&sv, SRV_SECOND, ANSWER, sizeof ANSWER));

    uint8_t buf[512];
    size_t len = 0;
    ASSERT_EQ(dns_exchange(LOOPBACK, sv.port, QUERY, sizeof QUERY, buf,
                           sizeof buf, &len, 80, 2),
              NSH_OK);
    ASSERT_EQ(len, sizeof ANSWER);
    ASSERT_EQ(memcmp(buf, ANSWER, sizeof ANSWER), 0);

    unsigned count = 0;
    ASSERT_TRUE(server_stop(&sv, &count));
    ASSERT_EQ(count, 2);
}

TEST(reply_longer_than_cap_is_clipped) {
    uint8_t big[200];
    for (size_t i = 0; i < sizeof big; i++) {
        big[i] = (uint8_t)(i + 1);
    }
    Server sv;
    ASSERT_TRUE(server_start(&sv, SRV_ANSWER, big, sizeof big));

    uint8_t buf[64];
    memset(buf, 0x5A, sizeof buf);
    size_t len = 0;
    ASSERT_EQ(dns_exchange(LOOPBACK, sv.port, QUERY, sizeof QUERY, buf, 16,
                           &len, 500, 2),
              NSH_OK);
    ASSERT_EQ(len, 16);
    ASSERT_EQ(memcmp(buf, big, 16), 0);
    for (size_t i = 16; i < sizeof buf; i++) {
        ASSERT_EQ(buf[i], 0x5A);
    }

    unsigned count = 0;
    ASSERT_TRUE(server_stop(&sv, &count));
    ASSERT_EQ(count, 1);
}

TEST(decoy_from_another_port_loses_to_the_real_answer) {
    Server sv;
    ASSERT_TRUE(server_start(&sv, SRV_DECOY_THEN, ANSWER, sizeof ANSWER));

    uint8_t buf[512];
    size_t len = 0;
    ASSERT_EQ(dns_exchange(LOOPBACK, sv.port, QUERY, sizeof QUERY, buf,
                           sizeof buf, &len, 500, 2),
              NSH_OK);
    ASSERT_EQ(len, sizeof ANSWER);
    ASSERT_EQ(memcmp(buf, ANSWER, sizeof ANSWER), 0);

    unsigned count = 0;
    ASSERT_TRUE(server_stop(&sv, &count));
    ASSERT_EQ(count, 1);
}

// A decoy must not satisfy the try, so the answer can only come from the resend.
TEST(decoy_alone_does_not_end_a_try) {
    Server sv;
    ASSERT_TRUE(server_start(&sv, SRV_DECOY_FIRST, ANSWER, sizeof ANSWER));

    uint8_t buf[512];
    size_t len = 0;
    ASSERT_EQ(dns_exchange(LOOPBACK, sv.port, QUERY, sizeof QUERY, buf,
                           sizeof buf, &len, 80, 2),
              NSH_OK);
    ASSERT_EQ(len, sizeof ANSWER);
    ASSERT_EQ(memcmp(buf, ANSWER, sizeof ANSWER), 0);

    unsigned count = 0;
    ASSERT_TRUE(server_stop(&sv, &count));
    ASSERT_EQ(count, 2);
}

// Right port, wrong address. Nothing here passes if only the port is compared.
TEST(decoy_from_another_address_on_the_right_port_is_ignored) {
    Server sv;
    ASSERT_TRUE(server_start(&sv, SRV_ALIAS_FIRST, ANSWER, sizeof ANSWER));
    unsigned count = 0;
    if (!sv.alias_ok) {
        // No 127.0.0.2 here, so there is no decoy to ignore and nothing to prove.
        ASSERT_TRUE(server_stop(&sv, &count));
        return;
    }

    uint8_t buf[512];
    size_t len = 0;
    ASSERT_EQ(dns_exchange(LOOPBACK, sv.port, QUERY, sizeof QUERY, buf,
                           sizeof buf, &len, 80, 2),
              NSH_OK);
    ASSERT_EQ(len, sizeof ANSWER);
    ASSERT_EQ(memcmp(buf, ANSWER, sizeof ANSWER), 0);

    ASSERT_TRUE(server_stop(&sv, &count));
    ASSERT_EQ(count, 2);
}

TEST(a_bad_server_string_is_invalid_and_opens_nothing) {
    static const char *bad[] = {"not.an.ip", "", "256.1.1.1", "1.2.3",
                                "::1",       "1.2.3.4.5"};
    uint8_t buf[64];
    int before = lowest_free_fd();
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        size_t len = 9;
        ASSERT_EQ(dns_exchange(bad[i], 53, QUERY, sizeof QUERY, buf, sizeof buf,
                               &len, 500, 2),
                  NSH_ERR_INVALID);
        ASSERT_EQ(len, 0);
    }
    ASSERT_EQ(lowest_free_fd(), before);
}

TEST(sigint_during_the_wait_returns_interrupt_and_gives_the_handler_back) {
    struct sigaction saved;
    ASSERT_TRUE(install_handler(SIGINT, noop_handler, &saved));

    Server sv;
    ASSERT_TRUE(server_start(&sv, SRV_SILENT, ANSWER, sizeof ANSWER));
    pid_t helper = spawn_signal_after(SIGINT, 60);
    ASSERT_TRUE(helper > 0);

    uint8_t buf[512];
    size_t len = 0;
    long long start = now_ms();
    ASSERT_EQ(dns_exchange(LOOPBACK, sv.port, QUERY, sizeof QUERY, buf,
                           sizeof buf, &len, 1000, 1),
              NSH_INTERRUPT);
    ASSERT_TRUE(now_ms() - start < 900);

    struct sigaction back;
    memset(&back, 0, sizeof back);
    ASSERT_EQ(sigaction(SIGINT, NULL, &back), 0);
    ASSERT_TRUE(back.sa_handler == noop_handler);

    int status = 0;
    ASSERT_EQ(waitpid(helper, &status, 0), helper);
    unsigned count = 0;
    ASSERT_TRUE(server_stop(&sv, &count));
    ASSERT_EQ(count, 1);
    (void)sigaction(SIGINT, &saved, NULL);
}

// EINTR from a signal that is not SIGINT leaves the flag clear, so the wait resumes.
TEST(another_signal_does_not_end_the_wait) {
    struct sigaction saved_usr1;
    struct sigaction saved_int;
    ASSERT_TRUE(install_handler(SIGUSR1, noop_handler, &saved_usr1));
    ASSERT_TRUE(install_handler(SIGINT, noop_handler, &saved_int));

    Server sv;
    ASSERT_TRUE(server_start(&sv, SRV_SILENT, ANSWER, sizeof ANSWER));
    pid_t helper = spawn_signal_after(SIGUSR1, 40);
    ASSERT_TRUE(helper > 0);

    uint8_t buf[512];
    size_t len = 0;
    long long start = now_ms();
    ASSERT_EQ(dns_exchange(LOOPBACK, sv.port, QUERY, sizeof QUERY, buf,
                           sizeof buf, &len, 200, 1),
              NSH_ERR_NOT_FOUND);
    ASSERT_TRUE(now_ms() - start >= 150);

    int status = 0;
    ASSERT_EQ(waitpid(helper, &status, 0), helper);
    unsigned count = 0;
    ASSERT_TRUE(server_stop(&sv, &count));
    ASSERT_EQ(count, 1);
    (void)sigaction(SIGUSR1, &saved_usr1, NULL);
    (void)sigaction(SIGINT, &saved_int, NULL);
}

TEST(a_normal_exchange_also_restores_the_disposition) {
    struct sigaction saved;
    ASSERT_TRUE(install_handler(SIGINT, SIG_IGN, &saved));

    Server sv;
    ASSERT_TRUE(server_start(&sv, SRV_ANSWER, ANSWER, sizeof ANSWER));
    uint8_t buf[512];
    size_t len = 0;
    ASSERT_EQ(dns_exchange(LOOPBACK, sv.port, QUERY, sizeof QUERY, buf,
                           sizeof buf, &len, 500, 2),
              NSH_OK);

    struct sigaction back;
    memset(&back, 0, sizeof back);
    ASSERT_EQ(sigaction(SIGINT, NULL, &back), 0);
    ASSERT_TRUE(back.sa_handler == SIG_IGN);

    unsigned count = 0;
    ASSERT_TRUE(server_stop(&sv, &count));
    ASSERT_EQ(count, 1);
    (void)sigaction(SIGINT, &saved, NULL);
}

TEST_MAIN()
