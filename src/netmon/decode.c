// Defensive Ethernet/IPv4/TCP/UDP decoder: every field built from bytes, every length checked.

#include "decode.h"

#include <string.h>

#define ETH_HDR_LEN 14
#define ETHERTYPE_IPV4 0x0800

#define IP_VERSION_4 4
#define IP_MIN_IHL 5
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

#define TCP_MIN_HDR_LEN 20
#define TCP_MIN_DOFF 5
#define UDP_HDR_LEN 8

#define MAC_LEN 6
#define IPV4_ADDR_LEN 4

// Ethernet field offsets.
#define ETH_OFF_DST 0
#define ETH_OFF_SRC 6
#define ETH_OFF_TYPE 12

// IPv4 field offsets, from the start of the IP header.
#define IP_OFF_VER_IHL 0
#define IP_OFF_TOTAL_LEN 2
#define IP_OFF_TTL 8
#define IP_OFF_PROTO 9
#define IP_OFF_SRC 12
#define IP_OFF_DST 16

// TCP field offsets, from the start of the TCP header.
#define TCP_OFF_SPORT 0
#define TCP_OFF_DPORT 2
#define TCP_OFF_SEQ 4
#define TCP_OFF_ACK 8
#define TCP_OFF_DOFF 12
#define TCP_OFF_FLAGS 13

// UDP field offsets, from the start of the UDP header.
#define UDP_OFF_SPORT 0
#define UDP_OFF_DPORT 2
#define UDP_OFF_LEN 4

// Big-endian readers, so nothing depends on host byte order or pointer alignment.
static uint16_t rd16be(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// avail is the IP payload length, which is total_len minus the IP header.
static NshError decode_tcp(const uint8_t *p, size_t avail, TcpHeader *out) {
    if (avail < TCP_MIN_HDR_LEN) {
        return NSH_ERR_INVALID;
    }
    uint8_t doff = (uint8_t)(p[TCP_OFF_DOFF] >> 4);
    if (doff < TCP_MIN_DOFF || (size_t)doff * 4 > avail) {
        return NSH_ERR_INVALID;
    }
    out->sport = rd16be(p + TCP_OFF_SPORT);
    out->dport = rd16be(p + TCP_OFF_DPORT);
    out->seq = rd32be(p + TCP_OFF_SEQ);
    out->ack = rd32be(p + TCP_OFF_ACK);
    out->flags = p[TCP_OFF_FLAGS];
    return NSH_OK;
}

static NshError decode_udp(const uint8_t *p, size_t avail, UdpHeader *out) {
    if (avail < UDP_HDR_LEN) {
        return NSH_ERR_INVALID;
    }
    uint16_t ulen = rd16be(p + UDP_OFF_LEN);
    if (ulen < UDP_HDR_LEN || (size_t)ulen > avail) {
        return NSH_ERR_INVALID;
    }
    out->sport = rd16be(p + UDP_OFF_SPORT);
    out->dport = rd16be(p + UDP_OFF_DPORT);
    out->len = ulen;
    return NSH_OK;
}

// avail is what the frame actually holds after Ethernet, which total_len may undershoot.
static NshError decode_ipv4(const uint8_t *p, size_t avail, Packet *out) {
    if (avail < (size_t)IP_MIN_IHL * 4) {
        return NSH_ERR_INVALID;
    }
    uint8_t version = (uint8_t)(p[IP_OFF_VER_IHL] >> 4);
    uint8_t ihl = (uint8_t)(p[IP_OFF_VER_IHL] & 0x0F);
    if (version != IP_VERSION_4 || ihl < IP_MIN_IHL) {
        return NSH_ERR_INVALID;
    }
    size_t hdr_len = (size_t)ihl * 4;
    if (hdr_len > avail) {
        return NSH_ERR_INVALID;
    }
    uint16_t total_len = rd16be(p + IP_OFF_TOTAL_LEN);
    if ((size_t)total_len > avail || (size_t)total_len < hdr_len) {
        return NSH_ERR_INVALID;
    }

    out->has_ip = true;
    out->ip.ihl = ihl;
    out->ip.ttl = p[IP_OFF_TTL];
    out->ip.protocol = p[IP_OFF_PROTO];
    out->ip.total_len = total_len;
    memcpy(out->ip.src, p + IP_OFF_SRC, IPV4_ADDR_LEN);
    memcpy(out->ip.dst, p + IP_OFF_DST, IPV4_ADDR_LEN);

    const uint8_t *payload = p + hdr_len;
    size_t payload_len = (size_t)total_len - hdr_len;
    switch (out->ip.protocol) {
    case IP_PROTO_TCP:
        out->proto = NM_TCP;
        return decode_tcp(payload, payload_len, &out->tcp);
    case IP_PROTO_UDP:
        out->proto = NM_UDP;
        return decode_udp(payload, payload_len, &out->udp);
    default:
        out->proto = NM_OTHER_IP;
        return NSH_OK;
    }
}

NshError decode_frame(const uint8_t *frame, size_t len, Packet *out) {
    if (out == NULL) {
        return NSH_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    if (frame == NULL || len < ETH_HDR_LEN) {
        return NSH_ERR_INVALID;
    }

    memcpy(out->eth.dst, frame + ETH_OFF_DST, MAC_LEN);
    memcpy(out->eth.src, frame + ETH_OFF_SRC, MAC_LEN);
    out->eth.ethertype = rd16be(frame + ETH_OFF_TYPE);
    out->frame_len = len;

    if (out->eth.ethertype != ETHERTYPE_IPV4) {
        return NSH_OK;
    }

    NshError e = decode_ipv4(frame + ETH_HDR_LEN, len - ETH_HDR_LEN, out);
    if (e != NSH_OK) {
        memset(out, 0, sizeof(*out));
        return e;
    }
    return NSH_OK;
}
