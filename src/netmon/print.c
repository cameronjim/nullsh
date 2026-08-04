// One line rendering of a decoded Packet. Pure formatting, no I/O.

#include "print.h"

#include <stdio.h>

// Wide enough for the longest piece any helper below formats.
#define PIECE_BUF 32

static void append_uint(Str *out, unsigned long long v) {
    char buf[PIECE_BUF];
    snprintf(buf, sizeof buf, "%llu", v);
    str_append(out, buf);
}

static void append_hex16(Str *out, uint16_t v) {
    char buf[PIECE_BUF];
    snprintf(buf, sizeof buf, "0x%04x", v);
    str_append(out, buf);
}

static void append_ipv4(Str *out, const uint8_t addr[4]) {
    char buf[PIECE_BUF];
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", addr[0], addr[1], addr[2],
             addr[3]);
    str_append(out, buf);
}

static void append_mac(Str *out, const uint8_t mac[6]) {
    char buf[PIECE_BUF];
    snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    str_append(out, buf);
}

static void append_endpoint(Str *out, const uint8_t addr[4], uint16_t port) {
    append_ipv4(out, addr);
    str_push(out, ':');
    append_uint(out, port);
}

// Always in FSRPA order, a lone dot when the segment carries none of them.
static void append_flags(Str *out, uint8_t flags) {
    char buf[8];
    size_t n = 0;
    if (flags & TCP_FIN) {
        buf[n++] = 'F';
    }
    if (flags & TCP_SYN) {
        buf[n++] = 'S';
    }
    if (flags & TCP_RST) {
        buf[n++] = 'R';
    }
    if (flags & TCP_PSH) {
        buf[n++] = 'P';
    }
    if (flags & TCP_ACK) {
        buf[n++] = 'A';
    }
    if (n == 0) {
        buf[n++] = '.';
    }
    buf[n] = '\0';
    str_push(out, '[');
    str_append(out, buf);
    str_push(out, ']');
}

static void append_eth_line(const Packet *p, Str *out) {
    str_append(out, "ETH ");
    append_mac(out, p->eth.src);
    str_append(out, " > ");
    append_mac(out, p->eth.dst);
    str_append(out, " type ");
    append_hex16(out, p->eth.ethertype);
}

static void append_ip_line(const Packet *p, Str *out) {
    str_append(out, "IP ");
    if (p->proto == NM_TCP) {
        append_endpoint(out, p->ip.src, p->tcp.sport);
        str_append(out, " > ");
        append_endpoint(out, p->ip.dst, p->tcp.dport);
        str_append(out, " TCP ");
        append_flags(out, p->tcp.flags);
        str_append(out, " seq ");
        append_uint(out, p->tcp.seq);
        return;
    }
    if (p->proto == NM_UDP) {
        append_endpoint(out, p->ip.src, p->udp.sport);
        str_append(out, " > ");
        append_endpoint(out, p->ip.dst, p->udp.dport);
        str_append(out, " UDP");
        return;
    }
    append_ipv4(out, p->ip.src);
    str_append(out, " > ");
    append_ipv4(out, p->ip.dst);
    str_append(out, " proto ");
    append_uint(out, p->ip.protocol);
}

void packet_format(const Packet *p, Str *out) {
    str_clear(out);
    if (p->has_ip) {
        append_ip_line(p, out);
    } else {
        append_eth_line(p, out);
    }
    str_append(out, " len ");
    append_uint(out, p->frame_len);
}
