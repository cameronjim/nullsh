// Wire-format decoding into structs. Pure bytes in, validated fields out.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../util/error.h"

typedef struct {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype;  // host order
} EthHeader;

typedef struct {
    uint8_t ihl;         // header length in 4-byte words, >= 5
    uint8_t ttl;
    uint8_t protocol;    // 6 tcp, 17 udp
    uint16_t total_len;  // host order, bytes from the IP header on
    uint8_t src[4];
    uint8_t dst[4];
} Ipv4Header;

// TCP flag bits as they sit in the header's 13th byte.
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

typedef struct {
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t flags;
} TcpHeader;

typedef struct {
    uint16_t sport;
    uint16_t dport;
    uint16_t len;  // udp header + payload
} UdpHeader;

typedef enum { NM_NONE, NM_TCP, NM_UDP, NM_OTHER_IP } NmProto;

typedef struct {
    EthHeader eth;
    bool has_ip;      // false for ARP, IPv6, anything not 0x0800
    Ipv4Header ip;    // valid when has_ip
    NmProto proto;    // NM_NONE when !has_ip
    TcpHeader tcp;    // valid when proto == NM_TCP
    UdpHeader udp;    // valid when proto == NM_UDP
    size_t frame_len;
} Packet;

// Decodes one captured frame. Ethernet must be whole; an IPv4 layer and its
// transport header must each fit or the whole frame is NSH_ERR_INVALID.
// Non-IPv4 ethertypes decode successfully with has_ip false.
NshError decode_frame(const uint8_t *frame, size_t len, Packet *out);
