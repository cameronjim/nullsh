// The resolve builtin: arguments, resolv.conf, the query id, and the printing.

#define _POSIX_C_SOURCE 200809L

#include "resolve.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dns.h"
#include "internal.h"
#include "net.h"

#include "../alloc/alloc.h"

#define PORT_MIN 1
#define PORT_MAX 65535
#define TIMEOUT_MIN 1
#define TIMEOUT_MAX 60000
#define TRIES_MIN 1
#define TRIES_MAX 5

#define PORT_DEFAULT 53
#define TIMEOUT_DEFAULT 2000
#define TRIES_DEFAULT 2

// A shell killed by SIGINT reports this, and so does resolve.
#define STATUS_SIGINT 130

// One resolv.conf line, with the rest of an overlong one drained and dropped.
#define CONF_LINE_MAX 512

// A presentation name plus the trailing dot the output format always prints.
#define DOTTED_MAX (DNS_NAME_MAX + 2)

// "CLASS65535" is the longest this ever holds.
#define CLASS_MAX 16

static void resolve_usage(void) {
    fputs("nullsh: resolve: usage: resolve NAME [--server IP] [--port N] "
          "[--timeout MS] [--tries N]\n",
          stderr);
}

static void resolve_err(const char *reason) {
    fprintf(stderr, "nullsh: resolve: %s\n", reason);
}

// Arguments

static bool parse_int(const char *s, int lo, int hi, int *out) {
    // strtol would take leading blanks and a sign; a count is digits only.
    if (s[0] < '0' || s[0] > '9') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0 || v < lo || v > hi) {
        return false;
    }
    *out = (int)v;
    return true;
}

bool resolve_parse_args(int argc, char **argv, ResolveOpts *opts) {
    opts->name = NULL;
    opts->server = NULL;
    opts->port = PORT_DEFAULT;
    opts->timeout_ms = TIMEOUT_DEFAULT;
    opts->tries = TRIES_DEFAULT;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] != '-' || arg[1] == '\0') {
            if (opts->name != NULL || arg[0] == '\0') {
                return false;
            }
            opts->name = arg;
            continue;
        }
        if (i + 1 >= argc) {
            return false;
        }
        const char *value = argv[++i];
        if (strcmp(arg, "--server") == 0) {
            if (value[0] == '\0') {
                return false;
            }
            opts->server = value;
        } else if (strcmp(arg, "--port") == 0) {
            if (!parse_int(value, PORT_MIN, PORT_MAX, &opts->port)) {
                return false;
            }
        } else if (strcmp(arg, "--timeout") == 0) {
            if (!parse_int(value, TIMEOUT_MIN, TIMEOUT_MAX,
                           &opts->timeout_ms)) {
                return false;
            }
        } else if (strcmp(arg, "--tries") == 0) {
            if (!parse_int(value, TRIES_MIN, TRIES_MAX, &opts->tries)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return opts->name != NULL;
}

// resolv.conf

static char *skip_blanks(char *p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static char *token_end(char *p) {
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        p++;
    }
    return p;
}

// A comment runs to the end of the line wherever it starts.
static void strip_comment(char *line) {
    for (char *p = line; *p != '\0'; p++) {
        if (*p == '#' || *p == ';') {
            *p = '\0';
            return;
        }
    }
}

// true once out holds the address; a colon means IPv6 and is skipped.
static bool conf_line_server(char *line, char *out, size_t cap) {
    strip_comment(line);
    char *key = skip_blanks(line);
    char *key_end = token_end(key);
    if ((size_t)(key_end - key) != strlen("nameserver") ||
        strncmp(key, "nameserver", strlen("nameserver")) != 0) {
        return false;
    }
    char *addr = skip_blanks(key_end);
    char *addr_end = token_end(addr);
    size_t len = (size_t)(addr_end - addr);
    if (len == 0 || len >= cap || memchr(addr, ':', len) != NULL) {
        return false;
    }
    memcpy(out, addr, len);
    out[len] = '\0';
    return true;
}

bool resolve_conf_scan(FILE *f, char *out, size_t cap) {
    char line[CONF_LINE_MAX];
    bool found = false;
    while (!found && fgets(line, sizeof line, f) != NULL) {
        bool whole = strchr(line, '\n') != NULL;
        found = conf_line_server(line, out, cap);
        // An overlong line's tail must not be read back as a line of its own.
        while (!whole) {
            char rest[CONF_LINE_MAX];
            if (fgets(rest, sizeof rest, f) == NULL) {
                break;
            }
            whole = strchr(rest, '\n') != NULL;
        }
    }
    return found;
}

bool resolve_conf_lookup(const char *path, char *out, size_t cap) {
    if (path == NULL) {
        path = getenv("NSH_RESOLV_CONF");
    }
    if (path == NULL || path[0] == '\0') {
        path = "/etc/resolv.conf";
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    bool found = resolve_conf_scan(f, out, cap);
    fclose(f);
    return found;
}

// Output

// The root name prints as a lone dot, everything else gains a trailing one.
static void dotted_name(const char *in, char *out, size_t cap) {
    size_t len = strlen(in);
    if (len + 2 > cap) {
        len = cap - 2;
    }
    memcpy(out, in, len);
    if (len == 0 || out[len - 1] != '.') {
        out[len++] = '.';
    }
    out[len] = '\0';
}

static void class_name(uint16_t qclass, char *out, size_t cap) {
    if (qclass == DNS_CLASS_IN) {
        snprintf(out, cap, "IN");
    } else {
        snprintf(out, cap, "CLASS%u", (unsigned)qclass);
    }
}

static void print_flags(const DnsHeader *hdr, FILE *out) {
    static const char *const NAMES[] = {"qr", "aa", "tc", "rd", "ra"};
    const bool set[] = {hdr->qr, hdr->aa, hdr->tc, hdr->rd, hdr->ra};
    for (size_t i = 0; i < sizeof set / sizeof set[0]; i++) {
        if (set[i]) {
            fprintf(out, " %s", NAMES[i]);
        }
    }
}

static void print_record(const DnsRecord *r, FILE *out) {
    char name[DOTTED_MAX];
    char qclass[CLASS_MAX];
    dotted_name(r->name, name, sizeof name);
    class_name(r->qclass, qclass, sizeof qclass);
    fprintf(out, "%s %u %s ", name, (unsigned)r->ttl, qclass);

    if (r->type == DNS_TYPE_A) {
        fprintf(out, "A %u.%u.%u.%u\n", r->addr[0], r->addr[1], r->addr[2],
                r->addr[3]);
    } else if (r->type == DNS_TYPE_CNAME) {
        char target[DOTTED_MAX];
        dotted_name(r->target, target, sizeof target);
        fprintf(out, "CNAME %s\n", target);
    } else {
        fprintf(out, "TYPE%u (%u bytes)\n", (unsigned)r->type,
                (unsigned)r->rdlength);
    }
}

void resolve_print_reply(const DnsHeader *hdr, const Vec *answers, FILE *out) {
    fprintf(out, ";; id %u flags", (unsigned)hdr->id);
    print_flags(hdr, out);
    const char *rcode = dns_rcode_str(hdr->rcode);
    if (rcode != NULL) {
        fprintf(out, " rcode %s", rcode);
    } else {
        fprintf(out, " rcode %u", (unsigned)hdr->rcode);
    }
    fprintf(out, " answers %u\n", (unsigned)hdr->ancount);

    // Both notices sit between the header line and the records, tc first.
    if (hdr->tc) {
        fputs(";; truncated reply\n", out);
    }
    if (!hdr->ra) {
        fputs(";; recursion not available\n", out);
    }
    for (size_t i = 0; i < answers->len; i++) {
        const DnsRecord *r = answers->items[i];
        if (r != NULL) {
            print_record(r, out);
        }
    }
}

// Outcomes

int resolve_exchange_status(NshError err, const char *server, int tries,
                            char *msg, size_t cap) {
    msg[0] = '\0';
    switch (err) {
    case NSH_OK:
        return 0;
    case NSH_INTERRUPT:
        return STATUS_SIGINT;
    case NSH_ERR_NOT_FOUND:
        snprintf(msg, cap, "no reply from %s after %d %s", server, tries,
                 (tries == 1) ? "try" : "tries");
        return 1;
    case NSH_ERR_INVALID:
        snprintf(msg, cap, "bad server address: %s", server);
        return 1;
    case NSH_ERR_IO:
        snprintf(msg, cap, "socket failure talking to %s", server);
        return 1;
    default:
        snprintf(msg, cap, "lookup failed: %s", nsh_error_str(err));
        return 1;
    }
}

int resolve_reply_status(const DnsHeader *hdr, uint16_t want_id, char *msg,
                         size_t cap) {
    msg[0] = '\0';
    if (hdr->id != want_id) {
        snprintf(msg, cap, "reply id mismatch");
        return 1;
    }
    if (hdr->rcode != 0) {
        const char *rcode = dns_rcode_str(hdr->rcode);
        if (rcode != NULL) {
            snprintf(msg, cap, "server returned %s", rcode);
        } else {
            snprintf(msg, cap, "server returned rcode %u",
                     (unsigned)hdr->rcode);
        }
        return 1;
    }
    return 0;
}

// The builtin

// Unpredictable enough for a teaching tool; the manual says why that is low.
static uint16_t query_id(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        ts.tv_nsec = 0;
    }
    unsigned long mixed = (unsigned long)ts.tv_nsec ^ (unsigned long)getpid();
    return (uint16_t)(mixed & 0xFFFFu);
}

static void free_answers(Vec *answers) {
    vec_free_deep(answers, nsh_free);
}

int resolve_builtin(Shell *sh, int argc, char **argv) {
    (void)sh;
    ResolveOpts opts;
    if (!resolve_parse_args(argc, argv, &opts)) {
        resolve_usage();
        return 1;
    }

    char from_conf[RESOLVE_SERVER_MAX];
    const char *server = opts.server;
    if (server == NULL) {
        if (!resolve_conf_lookup(NULL, from_conf, sizeof from_conf)) {
            resolve_err("no nameserver found; use --server");
            return 1;
        }
        server = from_conf;
    }

    uint16_t id = query_id();
    uint8_t query[DNS_QUERY_MAX];
    size_t query_len = 0;
    if (dns_build_query(opts.name, id, query, sizeof query, &query_len) !=
        NSH_OK) {
        fprintf(stderr, "nullsh: resolve: %s: not a valid name\n", opts.name);
        return 1;
    }

    // The same 512 byte cap bounds a classic UDP reply.
    uint8_t reply[DNS_QUERY_MAX];
    size_t reply_len = 0;
    NshError err =
        dns_exchange(server, (uint16_t)opts.port, query, query_len, reply,
                     sizeof reply, &reply_len, opts.timeout_ms, opts.tries);

    char msg[RESOLVE_MSG_MAX];
    int status = resolve_exchange_status(err, server, opts.tries, msg,
                                         sizeof msg);
    if (status != 0) {
        if (msg[0] != '\0') {
            resolve_err(msg);
        }
        return status;
    }

    DnsHeader hdr;
    Vec answers;
    if (dns_parse_reply(reply, reply_len, &hdr, &answers) != NSH_OK) {
        free_answers(&answers);
        resolve_err("malformed reply");
        return 1;
    }

    status = resolve_reply_status(&hdr, id, msg, sizeof msg);
    if (status != 0) {
        free_answers(&answers);
        resolve_err(msg);
        return status;
    }

    resolve_print_reply(&hdr, &answers, stdout);
    free_answers(&answers);
    return 0;
}
