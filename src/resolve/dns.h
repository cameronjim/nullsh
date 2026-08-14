// DNS wire format per RFC 1035: build queries, parse replies. Pure, no I/O.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../util/error.h"
#include "../util/vec.h"

// A presentation-form name: 255 wire bytes cannot exceed 255 text bytes.
#define DNS_NAME_MAX 255

// Classic UDP DNS caps a message at 512 bytes; queries are far smaller.
#define DNS_QUERY_MAX 512

#define DNS_TYPE_A 1
#define DNS_TYPE_CNAME 5
#define DNS_CLASS_IN 1

typedef struct {
    uint16_t id;
    bool qr;
    bool aa;
    bool tc;
    bool rd;
    bool ra;
    uint8_t rcode;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} DnsHeader;

// One answer record. addr is valid for TYPE_A, target for TYPE_CNAME.
typedef struct {
    char name[DNS_NAME_MAX + 1];
    uint16_t type;
    uint16_t qclass;
    uint32_t ttl;
    uint16_t rdlength;
    uint8_t addr[4];
    char target[DNS_NAME_MAX + 1];
} DnsRecord;

// Encodes a recursive query for an A record on name. out needs DNS_QUERY_MAX.
NshError dns_build_query(const char *name, uint16_t id, uint8_t *out,
                         size_t cap, size_t *out_len);

// Parses the header and every answer into hdr and answers (DnsRecord*,
// nsh-allocated, caller frees them). answers is overwritten and left valid
// and empty on error; every offset is validated before it is followed.
NshError dns_parse_reply(const uint8_t *pkt, size_t len, DnsHeader *hdr,
                         Vec *answers);

// "NOERROR", "NXDOMAIN" and friends; NULL for a code with no common name.
const char *dns_rcode_str(uint8_t rcode);
