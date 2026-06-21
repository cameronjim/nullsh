// Contract between resolve.c and its tests. Not for use outside src/resolve/.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../util/error.h"
#include "../util/vec.h"
#include "dns.h"

// A dotted quad needs 16; the slack is there to notice an overlong token.
#define RESOLVE_SERVER_MAX 64

// The longest "nullsh: resolve: <reason>" body any path builds.
#define RESOLVE_MSG_MAX 128

typedef struct {
    const char *name;
    // NULL until --server or resolv.conf names one.
    const char *server;
    int port;
    int timeout_ms;
    int tries;
} ResolveOpts;

// false with nothing printed when a word is missing, unknown or out of range.
bool resolve_parse_args(int argc, char **argv, ResolveOpts *opts);

// The first nameserver line holding an IPv4 address, copied into out.
bool resolve_conf_scan(FILE *f, char *out, size_t cap);

// A NULL path reads $NSH_RESOLV_CONF, or /etc/resolv.conf when that is unset.
bool resolve_conf_lookup(const char *path, char *out, size_t cap);

// The pinned ";; id" line, the tc and ra notices, then one line per answer.
void resolve_print_reply(const DnsHeader *hdr, const Vec *answers, FILE *out);

// The status dns_exchange's result earns; msg takes the reason, or "".
int resolve_exchange_status(NshError err, const char *server, int tries,
                            char *msg, size_t cap);

// The status a parsed reply earns: id mismatch, a non-zero rcode, or 0.
int resolve_reply_status(const DnsHeader *hdr, uint16_t want_id, char *msg,
                         size_t cap);
