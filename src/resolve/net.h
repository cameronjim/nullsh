// One UDP DNS exchange: send, wait with a timeout, retry. No parsing here.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../util/error.h"

// Sends query to server:port and copies back the first datagram that arrives.
// server is a dotted IPv4 string. The send repeats after each timeout until
// tries runs out. A SIGINT flag handler is installed for the duration, netmon
// style, so Ctrl-C wakes the poll. Returns NSH_OK with reply_len set,
// NSH_ERR_NOT_FOUND for silence after every try, NSH_INTERRUPT for Ctrl-C,
// NSH_ERR_INVALID for a bad server string, NSH_ERR_IO for a socket failure.
NshError dns_exchange(const char *server, uint16_t port, const uint8_t *query,
                      size_t query_len, uint8_t *reply, size_t cap,
                      size_t *reply_len, int timeout_ms, int tries);
