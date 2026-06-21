// Contracts-commit stub; the dns agent replaces this file.

#include "dns.h"

NshError dns_build_query(const char *name, uint16_t id, uint8_t *out,
                         size_t cap, size_t *out_len) {
    (void)name;
    (void)id;
    (void)out;
    (void)cap;
    (void)out_len;
    return NSH_ERR_INVALID;
}

NshError dns_parse_reply(const uint8_t *pkt, size_t len, DnsHeader *hdr,
                         Vec *answers) {
    (void)pkt;
    (void)len;
    (void)hdr;
    vec_init(answers);
    return NSH_ERR_INVALID;
}

const char *dns_rcode_str(uint8_t rcode) {
    (void)rcode;
    return NULL;
}
