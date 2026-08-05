// Contracts-commit stub; the net agent replaces this file.

#include "net.h"

NshError dns_exchange(const char *server, uint16_t port, const uint8_t *query,
                      size_t query_len, uint8_t *reply, size_t cap,
                      size_t *reply_len, int timeout_ms, int tries) {
    (void)server;
    (void)port;
    (void)query;
    (void)query_len;
    (void)reply;
    (void)cap;
    (void)reply_len;
    (void)timeout_ms;
    (void)tries;
    return NSH_ERR_INVALID;
}
