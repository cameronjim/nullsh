// DNS wire codec per RFC 1035: build A queries, parse replies, decode names.

#include "dns.h"

#include <string.h>

#include "../alloc/alloc.h"

#define DNS_HDR_LEN 12
#define DNS_RR_FIXED_LEN 10
#define DNS_QUESTION_TAIL 4
#define DNS_LABEL_MAX 63
#define DNS_WIRE_NAME_MAX 255
#define DNS_A_RDLENGTH 4

// One name may follow this many compression pointers before it is a bomb.
#define DNS_MAX_POINTERS 32

// Header field offsets.
#define HDR_OFF_ID 0
#define HDR_OFF_FLAGS 2
#define HDR_OFF_QDCOUNT 4
#define HDR_OFF_ANCOUNT 6
#define HDR_OFF_NSCOUNT 8
#define HDR_OFF_ARCOUNT 10

// Bits inside the 16-bit flags word.
#define FLAG_QR 0x8000
#define FLAG_AA 0x0400
#define FLAG_TC 0x0200
#define FLAG_RD 0x0100
#define FLAG_RA 0x0080
#define FLAG_RCODE 0x000F

// Top two bits of a length byte: 11 is a pointer, 01 and 10 are reserved.
#define LABEL_TOP_BITS 0xC0
#define LABEL_PTR 0xC0
#define LABEL_PTR_OFF_HI 0x3F

// Big-endian readers, so nothing depends on host byte order or alignment.
static uint16_t rd16be(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

// Turns a dotted name into length-prefixed labels plus the root byte.
static NshError encode_name(const char *name, uint8_t *out, size_t *out_len) {
    size_t n = strlen(name);
    if (n == 0) {
        return NSH_ERR_INVALID;
    }
    // Exactly one trailing dot is the root separator, a second one is an empty label.
    if (name[n - 1] == '.') {
        n--;
    }
    if (n == 0 || name[n - 1] == '.') {
        return NSH_ERR_INVALID;
    }

    size_t w = 0;
    size_t i = 0;
    while (i < n) {
        size_t start = i;
        while (i < n && name[i] != '.') {
            i++;
        }
        size_t label = i - start;
        if (label == 0 || label > DNS_LABEL_MAX) {
            return NSH_ERR_INVALID;
        }
        // The root byte still has to fit once this label is written.
        if (w + 1 + label + 1 > DNS_WIRE_NAME_MAX) {
            return NSH_ERR_INVALID;
        }
        out[w] = (uint8_t)label;
        memcpy(out + w + 1, name + start, label);
        w += 1 + label;
        if (i < n) {
            i++;
        }
    }
    out[w] = 0;
    *out_len = w + 1;
    return NSH_OK;
}

NshError dns_build_query(const char *name, uint16_t id, uint8_t *out,
                         size_t cap, size_t *out_len) {
    if (name == NULL || out == NULL || out_len == NULL) {
        return NSH_ERR_INVALID;
    }

    uint8_t qname[DNS_WIRE_NAME_MAX];
    size_t qlen = 0;
    NshError e = encode_name(name, qname, &qlen);
    if (e != NSH_OK) {
        return e;
    }
    size_t total = DNS_HDR_LEN + qlen + DNS_QUESTION_TAIL;
    if (cap < total) {
        return NSH_ERR_INVALID;
    }

    memset(out, 0, DNS_HDR_LEN);
    wr16be(out + HDR_OFF_ID, id);
    wr16be(out + HDR_OFF_FLAGS, FLAG_RD);
    wr16be(out + HDR_OFF_QDCOUNT, 1);
    memcpy(out + DNS_HDR_LEN, qname, qlen);
    wr16be(out + DNS_HDR_LEN + qlen, DNS_TYPE_A);
    wr16be(out + DNS_HDR_LEN + qlen + 2, DNS_CLASS_IN);
    *out_len = total;
    return NSH_OK;
}

// Reads the name at *off into out as dotted text, following compression
// pointers. *off lands after the name as it appears here, not after the
// pointer target. out needs DNS_NAME_MAX + 1 bytes.
static NshError decode_name(const uint8_t *pkt, size_t len, size_t *off,
                            char *out) {
    size_t pos = *off;
    size_t hops = 0;
    size_t wire = 0;
    size_t text = 0;
    bool jumped = false;

    for (;;) {
        if (pos >= len) {
            return NSH_ERR_INVALID;
        }
        uint8_t b = pkt[pos];

        if ((b & LABEL_TOP_BITS) == LABEL_PTR) {
            if (pos + 1 >= len) {
                return NSH_ERR_INVALID;
            }
            if (hops >= DNS_MAX_POINTERS) {
                return NSH_ERR_INVALID;
            }
            size_t target =
                ((size_t)(b & LABEL_PTR_OFF_HI) << 8) | (size_t)pkt[pos + 1];
            // Strictly backwards, so a chain can never loop or run forward.
            if (target >= pos) {
                return NSH_ERR_INVALID;
            }
            hops++;
            if (!jumped) {
                *off = pos + 2;
                jumped = true;
            }
            pos = target;
            continue;
        }
        // 01 and 10 in the top two bits are reserved by RFC 1035.
        if ((b & LABEL_TOP_BITS) != 0) {
            return NSH_ERR_INVALID;
        }

        if (b == 0) {
            if (!jumped) {
                *off = pos + 1;
            }
            out[text] = '\0';
            return NSH_OK;
        }

        // b <= 63 here, so the label cap comes free from the top-bit check.
        size_t label = b;
        if (pos + 1 + label > len) {
            return NSH_ERR_INVALID;
        }
        if (wire + 1 + label + 1 > DNS_WIRE_NAME_MAX) {
            return NSH_ERR_INVALID;
        }
        if (text != 0) {
            out[text++] = '.';
        }
        memcpy(out + text, pkt + pos + 1, label);
        text += label;
        wire += 1 + label;
        pos += 1 + label;
    }
}

static NshError parse_record(const uint8_t *pkt, size_t len, size_t *off,
                             DnsRecord *r) {
    NshError e = decode_name(pkt, len, off, r->name);
    if (e != NSH_OK) {
        return e;
    }
    if (*off + DNS_RR_FIXED_LEN > len) {
        return NSH_ERR_INVALID;
    }
    r->type = rd16be(pkt + *off);
    r->qclass = rd16be(pkt + *off + 2);
    r->ttl = rd32be(pkt + *off + 4);
    r->rdlength = rd16be(pkt + *off + 8);
    *off += DNS_RR_FIXED_LEN;

    if (*off + r->rdlength > len) {
        return NSH_ERR_INVALID;
    }
    if (r->type == DNS_TYPE_A) {
        if (r->rdlength != DNS_A_RDLENGTH) {
            return NSH_ERR_INVALID;
        }
        memcpy(r->addr, pkt + *off, DNS_A_RDLENGTH);
    } else if (r->type == DNS_TYPE_CNAME) {
        size_t rdata = *off;
        e = decode_name(pkt, len, &rdata, r->target);
        if (e != NSH_OK) {
            return e;
        }
    }
    *off += r->rdlength;
    return NSH_OK;
}

NshError dns_parse_reply(const uint8_t *pkt, size_t len, DnsHeader *hdr,
                         Vec *answers) {
    if (answers == NULL) {
        return NSH_ERR_INVALID;
    }
    vec_init(answers);
    if (pkt == NULL || hdr == NULL) {
        return NSH_ERR_INVALID;
    }
    memset(hdr, 0, sizeof(*hdr));
    if (len < DNS_HDR_LEN) {
        return NSH_ERR_INVALID;
    }

    uint16_t flags = rd16be(pkt + HDR_OFF_FLAGS);
    hdr->id = rd16be(pkt + HDR_OFF_ID);
    hdr->qr = (flags & FLAG_QR) != 0;
    hdr->aa = (flags & FLAG_AA) != 0;
    hdr->tc = (flags & FLAG_TC) != 0;
    hdr->rd = (flags & FLAG_RD) != 0;
    hdr->ra = (flags & FLAG_RA) != 0;
    hdr->rcode = (uint8_t)(flags & FLAG_RCODE);
    hdr->qdcount = rd16be(pkt + HDR_OFF_QDCOUNT);
    hdr->ancount = rd16be(pkt + HDR_OFF_ANCOUNT);
    hdr->nscount = rd16be(pkt + HDR_OFF_NSCOUNT);
    hdr->arcount = rd16be(pkt + HDR_OFF_ARCOUNT);

    size_t off = DNS_HDR_LEN;
    char skipped[DNS_NAME_MAX + 1];
    for (uint16_t i = 0; i < hdr->qdcount; i++) {
        if (decode_name(pkt, len, &off, skipped) != NSH_OK) {
            goto fail;
        }
        if (off + DNS_QUESTION_TAIL > len) {
            goto fail;
        }
        off += DNS_QUESTION_TAIL;
    }

    for (uint16_t i = 0; i < hdr->ancount; i++) {
        DnsRecord *r = nsh_calloc(1, sizeof(*r));
        if (parse_record(pkt, len, &off, r) != NSH_OK) {
            nsh_free(r);
            goto fail;
        }
        vec_push(answers, r);
    }
    return NSH_OK;

fail:
    vec_free_deep(answers, nsh_free);
    vec_init(answers);
    memset(hdr, 0, sizeof(*hdr));
    return NSH_ERR_INVALID;
}

const char *dns_rcode_str(uint8_t rcode) {
    switch (rcode) {
    case 0:
        return "NOERROR";
    case 1:
        return "FORMERR";
    case 2:
        return "SERVFAIL";
    case 3:
        return "NXDOMAIN";
    case 4:
        return "NOTIMP";
    case 5:
        return "REFUSED";
    default:
        return NULL;
    }
}
