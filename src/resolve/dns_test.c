// Tests for the DNS wire codec: canned packets, hostile pointers, name caps.

#include "dns.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../tests/harness.h"
#include "../alloc/alloc.h"

// A reply with one question and one A answer reached through a pointer to 12.
static const uint8_t reply_a[45] = {
    // 0: id 0x4242
    0x42, 0x42,
    // 2: flags qr rd ra, rcode NOERROR
    0x81, 0x80,
    // 4: qdcount 1
    0x00, 0x01,
    // 6: ancount 1
    0x00, 0x01,
    // 8: nscount 0
    0x00, 0x00,
    // 10: arcount 0
    0x00, 0x00,
    // 12: question name 7 example 3 com 0
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    // 25: qtype A
    0x00, 0x01,
    // 27: qclass IN
    0x00, 0x01,
    // 29: answer name, pointer to offset 12
    0xC0, 0x0C,
    // 31: type A
    0x00, 0x01,
    // 33: class IN
    0x00, 0x01,
    // 35: ttl 300
    0x00, 0x00, 0x01, 0x2C,
    // 39: rdlength 4
    0x00, 0x04,
    // 41: rdata 93.184.216.34
    0x5D, 0xB8, 0xD8, 0x22,
};

// A CNAME whose target is compressed, then an A whose name points into that rdata.
static const uint8_t reply_cname_a[68] = {
    // 0: id 0xBEEF
    0xBE, 0xEF,
    // 2: flags qr rd ra, rcode NOERROR
    0x81, 0x80,
    // 4: qdcount 1
    0x00, 0x01,
    // 6: ancount 2
    0x00, 0x02,
    // 8: nscount 0
    0x00, 0x00,
    // 10: arcount 0
    0x00, 0x00,
    // 12: question name 3 www 7 example 3 com 0
    0x03, 'w', 'w', 'w',
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    // 29: qtype A
    0x00, 0x01,
    // 31: qclass IN
    0x00, 0x01,
    // 33: answer 1 name, pointer to offset 12
    0xC0, 0x0C,
    // 35: type CNAME
    0x00, 0x05,
    // 37: class IN
    0x00, 0x01,
    // 39: ttl 300
    0x00, 0x00, 0x01, 0x2C,
    // 43: rdlength 7
    0x00, 0x07,
    // 45: rdata 4 edge then a pointer to offset 16, which is "example.com"
    0x04, 'e', 'd', 'g', 'e',
    0xC0, 0x10,
    // 52: answer 2 name, pointer to offset 45, the CNAME target
    0xC0, 0x2D,
    // 54: type A
    0x00, 0x01,
    // 56: class IN
    0x00, 0x01,
    // 58: ttl 60
    0x00, 0x00, 0x00, 0x3C,
    // 62: rdlength 4
    0x00, 0x04,
    // 64: rdata 93.184.216.34
    0x5D, 0xB8, 0xD8, 0x22,
};

// Header only: every decoded flag set and rcode REFUSED.
static const uint8_t reply_flags_all[12] = {
    // 0: id 0x0001
    0x00, 0x01,
    // 2: qr aa tc rd, then ra with rcode 5
    0x87, 0x85,
    // 4: qdcount 0
    0x00, 0x00,
    // 6: ancount 0
    0x00, 0x00,
    // 8: nscount 3
    0x00, 0x03,
    // 10: arcount 7
    0x00, 0x07,
};

// Header only: no flags at all, which is what a raw query header looks like.
static const uint8_t reply_flags_none[12] = {
    // 0: id 0xFFFF
    0xFF, 0xFF,
    // 2: flags all clear, rcode NOERROR
    0x00, 0x00,
    // 4: qdcount 0
    0x00, 0x00,
    // 6: ancount 0
    0x00, 0x00,
    // 8: nscount 0
    0x00, 0x00,
    // 10: arcount 0
    0x00, 0x00,
};

// Two questions, the second one compressed, then one A answer.
static const uint8_t reply_two_questions[45] = {
    // 0: id 0x0007
    0x00, 0x07,
    // 2: flags qr rd ra
    0x81, 0x80,
    // 4: qdcount 2
    0x00, 0x02,
    // 6: ancount 1
    0x00, 0x01,
    // 8: nscount 0
    0x00, 0x00,
    // 10: arcount 0
    0x00, 0x00,
    // 12: question 1 name 1 a 3 com 0
    0x01, 'a',
    0x03, 'c', 'o', 'm',
    0x00,
    // 19: qtype A
    0x00, 0x01,
    // 21: qclass IN
    0x00, 0x01,
    // 23: question 2 name, pointer to offset 12
    0xC0, 0x0C,
    // 25: qtype A
    0x00, 0x01,
    // 27: qclass IN
    0x00, 0x01,
    // 29: answer name, pointer to offset 12
    0xC0, 0x0C,
    // 31: type A
    0x00, 0x01,
    // 33: class IN
    0x00, 0x01,
    // 35: ttl 1
    0x00, 0x00, 0x00, 0x01,
    // 39: rdlength 4
    0x00, 0x04,
    // 41: rdata 1.2.3.4
    0x01, 0x02, 0x03, 0x04,
};

// A root-named record of a type the codec does not decode.
static const uint8_t reply_unknown_type[28] = {
    // 0: id 0x0102
    0x01, 0x02,
    // 2: flags qr rd ra
    0x81, 0x80,
    // 4: qdcount 0
    0x00, 0x00,
    // 6: ancount 1
    0x00, 0x01,
    // 8: nscount 0
    0x00, 0x00,
    // 10: arcount 0
    0x00, 0x00,
    // 12: answer name, the root
    0x00,
    // 13: type 16, TXT
    0x00, 0x10,
    // 15: class IN
    0x00, 0x01,
    // 17: ttl 99
    0x00, 0x00, 0x00, 0x63,
    // 21: rdlength 5
    0x00, 0x05,
    // 23: rdata, four text bytes behind their own length byte
    0x03, 'h', 'e', 'y',
    0x00,
};

// One answer whose name is a single hostile length byte, patched per test.
static const uint8_t reply_bad_name[26] = {
    // 0: id 0x0009
    0x00, 0x09,
    // 2: flags qr rd ra
    0x81, 0x80,
    // 4: qdcount 0
    0x00, 0x00,
    // 6: ancount 1
    0x00, 0x01,
    // 8: nscount 0
    0x00, 0x00,
    // 10: arcount 0
    0x00, 0x00,
    // 12: answer name, two bytes patched by each test
    0xC0, 0x0C,
    // 14: type A
    0x00, 0x01,
    // 16: class IN
    0x00, 0x01,
    // 18: ttl 5
    0x00, 0x00, 0x00, 0x05,
    // 22: rdlength 4
    0x00, 0x04,
    // 24: rdata, only two bytes so a stray label read runs off the end
    0x0A, 0x0B,
};

// A forward pointer aimed at a root byte inside the rdata. Drop the
// strictly-backwards rule and this packet parses cleanly, so the rule shows.
static const uint8_t reply_forward_ptr[28] = {
    // 0: id 0x000B
    0x00, 0x0B,
    // 2: flags qr rd ra
    0x81, 0x80,
    // 4: qdcount 0
    0x00, 0x00,
    // 6: ancount 1
    0x00, 0x01,
    // 8: nscount 0
    0x00, 0x00,
    // 10: arcount 0
    0x00, 0x00,
    // 12: answer name, a pointer to offset 26, which is ahead of it
    0xC0, 0x1A,
    // 14: type A
    0x00, 0x01,
    // 16: class IN
    0x00, 0x01,
    // 18: ttl 8
    0x00, 0x00, 0x00, 0x08,
    // 22: rdlength 4
    0x00, 0x04,
    // 24: rdata 1.2.0.4, whose third byte is the root the pointer aims at
    0x01, 0x02, 0x00, 0x04,
};

static void free_answers(Vec *v) { vec_free_deep(v, nsh_free); }

// Builds a dotted name of count labels of 'a', with the given lengths.
static void make_name(char *out, const size_t *labels, size_t count) {
    size_t w = 0;
    for (size_t i = 0; i < count; i++) {
        if (i != 0) {
            out[w++] = '.';
        }
        memset(out + w, 'a', labels[i]);
        w += labels[i];
    }
    out[w] = '\0';
}

// Builds a reply whose second answer's name is reached through hops pointers,
// each one aiming two bytes lower than itself, ending on the name "hi".
static size_t build_pointer_chain(uint8_t *buf, size_t hops) {
    size_t k = hops - 1;
    size_t n = 4 + 2 * k;
    // Header: qr rd ra, no questions, two answers.
    buf[0] = 0x33;
    buf[1] = 0x44;
    buf[2] = 0x81;
    buf[3] = 0x80;
    buf[7] = 0x02;
    // Answer 1: root name, type 16, rdata holding the chain.
    buf[12] = 0x00;
    buf[14] = 0x10;
    buf[16] = 0x01;
    buf[21] = (uint8_t)(n >> 8);
    buf[22] = (uint8_t)(n & 0xFF);
    // The chain's destination, at the bottom of the rdata.
    buf[23] = 0x02;
    buf[24] = 'h';
    buf[25] = 'i';
    buf[26] = 0x00;
    for (size_t j = 1; j <= k; j++) {
        size_t at = 27 + 2 * (j - 1);
        size_t target = (j == 1) ? 23 : at - 2;
        buf[at] = (uint8_t)(0xC0 | (target >> 8));
        buf[at + 1] = (uint8_t)(target & 0xFF);
    }
    // Answer 2: an A record whose name enters the chain at its top.
    size_t rr2 = 23 + n;
    size_t top = 27 + 2 * (k - 1);
    buf[rr2] = (uint8_t)(0xC0 | (top >> 8));
    buf[rr2 + 1] = (uint8_t)(top & 0xFF);
    buf[rr2 + 3] = 0x01;
    buf[rr2 + 5] = 0x01;
    buf[rr2 + 9] = 0x0A;
    buf[rr2 + 11] = 0x04;
    buf[rr2 + 12] = 10;
    buf[rr2 + 15] = 1;
    return rr2 + 16;
}

TEST(query_two_label_name_byte_for_byte) {
    static const uint8_t want[29] = {
        // 0: id 0x1234
        0x12, 0x34,
        // 2: flags, recursion desired and nothing else
        0x01, 0x00,
        // 4: qdcount 1
        0x00, 0x01,
        // 6: ancount 0
        0x00, 0x00,
        // 8: nscount 0
        0x00, 0x00,
        // 10: arcount 0
        0x00, 0x00,
        // 12: qname 7 example 3 com 0
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        0x03, 'c', 'o', 'm',
        0x00,
        // 25: qtype A
        0x00, 0x01,
        // 27: qclass IN
        0x00, 0x01,
    };
    uint8_t out[DNS_QUERY_MAX];
    size_t len = 0;
    ASSERT_EQ(dns_build_query("example.com", 0x1234, out, sizeof(out), &len),
              NSH_OK);
    ASSERT_EQ(len, sizeof(want));
    for (size_t i = 0; i < sizeof(want); i++) {
        ASSERT_EQ(out[i], want[i]);
    }
}

TEST(query_trailing_dot_matches_bare_name) {
    uint8_t bare[DNS_QUERY_MAX];
    uint8_t dotted[DNS_QUERY_MAX];
    size_t a = 0;
    size_t b = 0;
    ASSERT_EQ(dns_build_query("example.com", 1, bare, sizeof(bare), &a), NSH_OK);
    ASSERT_EQ(dns_build_query("example.com.", 1, dotted, sizeof(dotted), &b),
              NSH_OK);
    ASSERT_EQ(a, b);
    ASSERT_EQ(memcmp(bare, dotted, a), 0);
}

TEST(query_rejects_empty_and_empty_labels) {
    uint8_t out[DNS_QUERY_MAX];
    size_t len = 0;
    ASSERT_EQ(dns_build_query("", 1, out, sizeof(out), &len), NSH_ERR_INVALID);
    ASSERT_EQ(dns_build_query(".", 1, out, sizeof(out), &len), NSH_ERR_INVALID);
    ASSERT_EQ(dns_build_query(".com", 1, out, sizeof(out), &len),
              NSH_ERR_INVALID);
    ASSERT_EQ(dns_build_query("a..b", 1, out, sizeof(out), &len),
              NSH_ERR_INVALID);
    ASSERT_EQ(dns_build_query("a..", 1, out, sizeof(out), &len),
              NSH_ERR_INVALID);
    ASSERT_EQ(dns_build_query("example.com..", 1, out, sizeof(out), &len),
              NSH_ERR_INVALID);
    ASSERT_EQ(dns_build_query(NULL, 1, out, sizeof(out), &len),
              NSH_ERR_INVALID);
}

TEST(query_label_cap_63_ok_64_rejected) {
    char name[128];
    uint8_t out[DNS_QUERY_MAX];
    size_t len = 0;

    size_t ok[2] = {63, 3};
    make_name(name, ok, 2);
    ASSERT_EQ(dns_build_query(name, 1, out, sizeof(out), &len), NSH_OK);
    ASSERT_EQ(len, 12 + 64 + 4 + 1 + 4);
    ASSERT_EQ(out[12], 63);

    size_t too_long[2] = {64, 3};
    make_name(name, too_long, 2);
    ASSERT_EQ(dns_build_query(name, 1, out, sizeof(out), &len),
              NSH_ERR_INVALID);
}

TEST(query_name_cap_255_wire_bytes) {
    char name[300];
    uint8_t out[DNS_QUERY_MAX];
    size_t len = 0;

    // 64 + 64 + 64 + 62 + 1 root byte is exactly 255 wire bytes.
    size_t at_cap[4] = {63, 63, 63, 61};
    make_name(name, at_cap, 4);
    ASSERT_EQ(dns_build_query(name, 1, out, sizeof(out), &len), NSH_OK);
    ASSERT_EQ(len, 12 + 255 + 4);

    // One more content byte pushes the wire name to 256.
    size_t over_cap[4] = {63, 63, 63, 62};
    make_name(name, over_cap, 4);
    ASSERT_EQ(dns_build_query(name, 1, out, sizeof(out), &len),
              NSH_ERR_INVALID);
}

TEST(query_rejects_short_buffer) {
    uint8_t out[DNS_QUERY_MAX];
    size_t len = 0;
    ASSERT_EQ(dns_build_query("example.com", 1, out, 28, &len),
              NSH_ERR_INVALID);
    ASSERT_EQ(dns_build_query("example.com", 1, out, 0, &len), NSH_ERR_INVALID);
    ASSERT_EQ(dns_build_query("example.com", 1, out, 29, &len), NSH_OK);
    ASSERT_EQ(len, 29);
}

TEST(header_every_flag_set) {
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(reply_flags_all, sizeof(reply_flags_all), &h,
                              &answers),
              NSH_OK);
    ASSERT_EQ(h.id, 1);
    ASSERT_TRUE(h.qr);
    ASSERT_TRUE(h.aa);
    ASSERT_TRUE(h.tc);
    ASSERT_TRUE(h.rd);
    ASSERT_TRUE(h.ra);
    ASSERT_EQ(h.rcode, 5);
    ASSERT_EQ(h.qdcount, 0);
    ASSERT_EQ(h.ancount, 0);
    ASSERT_EQ(h.nscount, 3);
    ASSERT_EQ(h.arcount, 7);
    ASSERT_EQ(answers.len, 0);
    free_answers(&answers);
}

TEST(header_no_flag_set) {
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(reply_flags_none, sizeof(reply_flags_none), &h,
                              &answers),
              NSH_OK);
    ASSERT_EQ(h.id, 0xFFFF);
    ASSERT_TRUE(!h.qr);
    ASSERT_TRUE(!h.aa);
    ASSERT_TRUE(!h.tc);
    ASSERT_TRUE(!h.rd);
    ASSERT_TRUE(!h.ra);
    ASSERT_EQ(h.rcode, 0);
    ASSERT_EQ(answers.len, 0);
    free_answers(&answers);
}

TEST(header_too_short_is_invalid) {
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(reply_a, 11, &h, &answers), NSH_ERR_INVALID);
    ASSERT_EQ(answers.len, 0);
    ASSERT_TRUE(answers.items != NULL);
    free_answers(&answers);
    ASSERT_EQ(dns_parse_reply(NULL, 40, &h, &answers), NSH_ERR_INVALID);
    free_answers(&answers);
}

TEST(single_a_answer) {
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(reply_a, sizeof(reply_a), &h, &answers), NSH_OK);
    ASSERT_EQ(h.id, 0x4242);
    ASSERT_TRUE(h.qr);
    ASSERT_TRUE(h.rd);
    ASSERT_TRUE(h.ra);
    ASSERT_TRUE(!h.aa);
    ASSERT_TRUE(!h.tc);
    ASSERT_EQ(h.rcode, 0);
    ASSERT_EQ(h.qdcount, 1);
    ASSERT_EQ(h.ancount, 1);
    ASSERT_EQ(answers.len, 1);

    const DnsRecord *r = vec_get(&answers, 0);
    ASSERT_STR_EQ(r->name, "example.com");
    ASSERT_EQ(r->type, DNS_TYPE_A);
    ASSERT_EQ(r->qclass, DNS_CLASS_IN);
    ASSERT_EQ(r->ttl, 300u);
    ASSERT_EQ(r->rdlength, 4);
    ASSERT_EQ(r->addr[0], 93);
    ASSERT_EQ(r->addr[1], 184);
    ASSERT_EQ(r->addr[2], 216);
    ASSERT_EQ(r->addr[3], 34);
    ASSERT_STR_EQ(r->target, "");
    free_answers(&answers);
}

TEST(cname_then_a_through_compression) {
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(reply_cname_a, sizeof(reply_cname_a), &h,
                              &answers),
              NSH_OK);
    ASSERT_EQ(h.id, 0xBEEF);
    ASSERT_EQ(h.ancount, 2);
    ASSERT_EQ(answers.len, 2);

    const DnsRecord *c = vec_get(&answers, 0);
    ASSERT_STR_EQ(c->name, "www.example.com");
    ASSERT_EQ(c->type, DNS_TYPE_CNAME);
    ASSERT_EQ(c->ttl, 300u);
    ASSERT_EQ(c->rdlength, 7);
    ASSERT_STR_EQ(c->target, "edge.example.com");

    const DnsRecord *a = vec_get(&answers, 1);
    ASSERT_STR_EQ(a->name, "edge.example.com");
    ASSERT_EQ(a->type, DNS_TYPE_A);
    ASSERT_EQ(a->ttl, 60u);
    ASSERT_EQ(a->addr[0], 93);
    ASSERT_EQ(a->addr[3], 34);
    free_answers(&answers);
}

TEST(name_through_three_pointer_hops) {
    uint8_t buf[192];
    memset(buf, 0, sizeof(buf));
    size_t len = build_pointer_chain(buf, 3);
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(buf, len, &h, &answers), NSH_OK);
    ASSERT_EQ(answers.len, 2);
    const DnsRecord *a = vec_get(&answers, 1);
    ASSERT_STR_EQ(a->name, "hi");
    ASSERT_EQ(a->type, DNS_TYPE_A);
    ASSERT_EQ(a->addr[0], 10);
    ASSERT_EQ(a->addr[3], 1);
    free_answers(&answers);
}

TEST(thirty_two_pointers_ok_thirty_three_rejected) {
    uint8_t buf[192];
    DnsHeader h;
    Vec answers;

    memset(buf, 0, sizeof(buf));
    size_t len = build_pointer_chain(buf, 32);
    ASSERT_EQ(dns_parse_reply(buf, len, &h, &answers), NSH_OK);
    ASSERT_EQ(answers.len, 2);
    ASSERT_STR_EQ(((const DnsRecord *)vec_get(&answers, 1))->name, "hi");
    free_answers(&answers);

    memset(buf, 0, sizeof(buf));
    len = build_pointer_chain(buf, 33);
    ASSERT_EQ(dns_parse_reply(buf, len, &h, &answers), NSH_ERR_INVALID);
    ASSERT_EQ(answers.len, 0);
    free_answers(&answers);
}

TEST(forward_pointer_rejected) {
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(reply_forward_ptr, sizeof(reply_forward_ptr), &h,
                              &answers),
              NSH_ERR_INVALID);
    ASSERT_EQ(answers.len, 0);
    free_answers(&answers);

    // The same packet with the pointer aimed backwards at the header is fine,
    // so the rejection above is about direction and nothing else.
    uint8_t pkt[sizeof(reply_forward_ptr)];
    memcpy(pkt, reply_forward_ptr, sizeof(pkt));
    pkt[13] = 0x0B;
    ASSERT_EQ(dns_parse_reply(pkt, sizeof(pkt), &h, &answers), NSH_OK);
    ASSERT_EQ(answers.len, 1);
    ASSERT_STR_EQ(((const DnsRecord *)vec_get(&answers, 0))->name, "");
    free_answers(&answers);
}

TEST(self_pointer_rejected) {
    uint8_t pkt[sizeof(reply_bad_name)];
    memcpy(pkt, reply_bad_name, sizeof(pkt));
    // A pointer at offset 12 aiming at offset 12 would spin forever.
    pkt[12] = 0xC0;
    pkt[13] = 0x0C;
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(pkt, sizeof(pkt), &h, &answers), NSH_ERR_INVALID);
    ASSERT_EQ(answers.len, 0);
    free_answers(&answers);
}

TEST(reserved_length_prefix_bits_rejected) {
    uint8_t pkt[sizeof(reply_bad_name)];
    DnsHeader h;
    Vec answers;

    // 01 in the top two bits is reserved, not a 64-byte label.
    memcpy(pkt, reply_bad_name, sizeof(pkt));
    pkt[12] = 0x40;
    pkt[13] = 0x00;
    ASSERT_EQ(dns_parse_reply(pkt, sizeof(pkt), &h, &answers), NSH_ERR_INVALID);
    ASSERT_EQ(answers.len, 0);
    free_answers(&answers);

    // 10 in the top two bits is reserved too.
    memcpy(pkt, reply_bad_name, sizeof(pkt));
    pkt[12] = 0x80;
    pkt[13] = 0x00;
    ASSERT_EQ(dns_parse_reply(pkt, sizeof(pkt), &h, &answers), NSH_ERR_INVALID);
    ASSERT_EQ(answers.len, 0);
    free_answers(&answers);
}

TEST(label_running_past_the_packet_rejected) {
    uint8_t pkt[sizeof(reply_bad_name)];
    memcpy(pkt, reply_bad_name, sizeof(pkt));
    // A 60-byte label declared 14 bytes from the end of a 26-byte packet.
    pkt[12] = 0x3C;
    pkt[13] = 'x';
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(pkt, sizeof(pkt), &h, &answers), NSH_ERR_INVALID);
    ASSERT_EQ(answers.len, 0);
    free_answers(&answers);
}

TEST(rdlength_past_the_packet_rejected) {
    uint8_t pkt[sizeof(reply_a)];
    memcpy(pkt, reply_a, sizeof(pkt));
    // rdlength 5 puts the last rdata byte one past the 45-byte packet.
    pkt[40] = 0x05;
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(pkt, sizeof(pkt), &h, &answers), NSH_ERR_INVALID);
    ASSERT_EQ(answers.len, 0);
    free_answers(&answers);
}

TEST(a_record_with_wrong_rdlength_rejected) {
    uint8_t pkt[sizeof(reply_a) + 1];
    memcpy(pkt, reply_a, sizeof(reply_a));
    pkt[sizeof(reply_a)] = 0x00;
    // Five bytes of rdata that fit in the packet are still not an address.
    pkt[40] = 0x05;
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(pkt, sizeof(pkt), &h, &answers), NSH_ERR_INVALID);
    ASSERT_EQ(answers.len, 0);
    free_answers(&answers);
}

TEST(two_questions_skipped) {
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(reply_two_questions, sizeof(reply_two_questions),
                              &h, &answers),
              NSH_OK);
    ASSERT_EQ(h.qdcount, 2);
    ASSERT_EQ(answers.len, 1);
    const DnsRecord *r = vec_get(&answers, 0);
    ASSERT_STR_EQ(r->name, "a.com");
    ASSERT_EQ(r->ttl, 1u);
    ASSERT_EQ(r->addr[0], 1);
    ASSERT_EQ(r->addr[1], 2);
    ASSERT_EQ(r->addr[2], 3);
    ASSERT_EQ(r->addr[3], 4);
    free_answers(&answers);
}

TEST(question_count_larger_than_the_packet_rejected) {
    uint8_t pkt[sizeof(reply_a)];
    memcpy(pkt, reply_a, sizeof(pkt));
    // qdcount 3 walks the parser off the end before it reaches the answer.
    pkt[5] = 0x03;
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(pkt, sizeof(pkt), &h, &answers), NSH_ERR_INVALID);
    ASSERT_EQ(answers.len, 0);
    free_answers(&answers);
}

TEST(unknown_type_kept_as_counts_and_root_name_is_empty) {
    DnsHeader h;
    Vec answers;
    ASSERT_EQ(dns_parse_reply(reply_unknown_type, sizeof(reply_unknown_type),
                              &h, &answers),
              NSH_OK);
    ASSERT_EQ(answers.len, 1);
    const DnsRecord *r = vec_get(&answers, 0);
    ASSERT_STR_EQ(r->name, "");
    ASSERT_EQ(r->type, 16);
    ASSERT_EQ(r->qclass, DNS_CLASS_IN);
    ASSERT_EQ(r->ttl, 99u);
    ASSERT_EQ(r->rdlength, 5);
    ASSERT_STR_EQ(r->target, "");
    ASSERT_EQ(r->addr[0], 0);
    ASSERT_EQ(r->addr[3], 0);
    free_answers(&answers);
}

TEST(truncation_sweep_never_crashes) {
    DnsHeader h;
    Vec answers;
    // The prefix is copied flush with the end of the array, so any read past
    // the declared length is a stack overflow ASan can see.
    uint8_t tail[sizeof(reply_cname_a)];
    for (size_t len = 0; len < sizeof(reply_cname_a); len++) {
        uint8_t *pkt = tail + (sizeof(tail) - len);
        memcpy(pkt, reply_cname_a, len);
        ASSERT_EQ(dns_parse_reply(pkt, len, &h, &answers), NSH_ERR_INVALID);
        ASSERT_EQ(answers.len, 0);
        ASSERT_TRUE(answers.items != NULL);
        free_answers(&answers);
    }
    ASSERT_EQ(dns_parse_reply(reply_cname_a, sizeof(reply_cname_a), &h,
                              &answers),
              NSH_OK);
    ASSERT_EQ(answers.len, 2);
    free_answers(&answers);
}

TEST(rcode_names_and_the_null_fallback) {
    ASSERT_STR_EQ(dns_rcode_str(0), "NOERROR");
    ASSERT_STR_EQ(dns_rcode_str(1), "FORMERR");
    ASSERT_STR_EQ(dns_rcode_str(2), "SERVFAIL");
    ASSERT_STR_EQ(dns_rcode_str(3), "NXDOMAIN");
    ASSERT_STR_EQ(dns_rcode_str(4), "NOTIMP");
    ASSERT_STR_EQ(dns_rcode_str(5), "REFUSED");
    ASSERT_TRUE(dns_rcode_str(6) == NULL);
    ASSERT_TRUE(dns_rcode_str(15) == NULL);
    ASSERT_TRUE(dns_rcode_str(255) == NULL);
}

TEST_MAIN()
