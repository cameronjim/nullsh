// Tests for the packet decoder: canned real-shaped frames and hostile ones.

#include "decode.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../tests/harness.h"

// Offsets into the canned frames, used by the tests that patch one byte.
#define OFF_ETHERTYPE 12
#define OFF_IP_VER_IHL 14
#define OFF_IP_TOTAL_LEN 16
#define OFF_L4 34
#define OFF_TCP_ACK (OFF_L4 + 8)
#define OFF_TCP_DOFF (OFF_L4 + 12)
#define OFF_TCP_FLAGS (OFF_L4 + 13)
#define OFF_UDP_LEN (OFF_L4 + 4)

// Ethernet + IPv4 + TCP SYN. Source port 0xC0DE must decode as 49374.
static const uint8_t frame_tcp_syn[54] = {
    // 0: dst mac aa:bb:cc:dd:ee:ff
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    // 6: src mac 11:22:33:44:55:66
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
    // 12: ethertype 0x0800
    0x08, 0x00,
    // 14: version 4, ihl 5
    0x45,
    // 15: dscp and ecn
    0x00,
    // 16: total length 40
    0x00, 0x28,
    // 18: identification
    0x1C, 0x46,
    // 20: flags and fragment offset
    0x40, 0x00,
    // 22: ttl 64
    0x40,
    // 23: protocol 6
    0x06,
    // 24: header checksum
    0x00, 0x00,
    // 26: src 192.168.1.10
    0xC0, 0xA8, 0x01, 0x0A,
    // 30: dst 93.184.216.34
    0x5D, 0xB8, 0xD8, 0x22,
    // 34: tcp source port 0xC0DE
    0xC0, 0xDE,
    // 36: tcp dest port 443
    0x01, 0xBB,
    // 38: sequence 0xDEADBEEF
    0xDE, 0xAD, 0xBE, 0xEF,
    // 42: acknowledgement 0
    0x00, 0x00, 0x00, 0x00,
    // 46: data offset 5, reserved bits
    0x50,
    // 47: flags SYN
    0x02,
    // 48: window
    0x72, 0x10,
    // 50: checksum
    0x00, 0x00,
    // 52: urgent pointer
    0x00, 0x00,
};

// Ethernet + IPv4 + UDP DNS query to port 53, 12 bytes of payload.
static const uint8_t frame_udp_dns[54] = {
    // 0: dst mac 00:11:22:33:44:55
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
    // 6: src mac 66:77:88:99:aa:bb
    0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB,
    // 12: ethertype 0x0800
    0x08, 0x00,
    // 14: version 4, ihl 5
    0x45,
    // 15: dscp and ecn
    0x00,
    // 16: total length 40
    0x00, 0x28,
    // 18: identification
    0x00, 0x2A,
    // 20: flags and fragment offset
    0x00, 0x00,
    // 22: ttl 64
    0x40,
    // 23: protocol 17
    0x11,
    // 24: header checksum
    0x00, 0x00,
    // 26: src 10.0.0.5
    0x0A, 0x00, 0x00, 0x05,
    // 30: dst 8.8.8.8
    0x08, 0x08, 0x08, 0x08,
    // 34: udp source port 0xF00D
    0xF0, 0x0D,
    // 36: udp dest port 53
    0x00, 0x35,
    // 38: udp length 20
    0x00, 0x14,
    // 40: udp checksum
    0x00, 0x00,
    // 42: dns transaction id
    0xAB, 0xCD,
    // 44: dns flags, recursion desired
    0x01, 0x00,
    // 46: question count 1
    0x00, 0x01,
    // 48: answer count 0
    0x00, 0x00,
    // 50: authority count 0
    0x00, 0x00,
    // 52: additional count 0
    0x00, 0x00,
};

// Ethernet + ARP request. Nothing past the ethertype should be looked at.
static const uint8_t frame_arp[42] = {
    // 0: dst mac ff:ff:ff:ff:ff:ff
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    // 6: src mac 00:1a:2b:3c:4d:5e
    0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E,
    // 12: ethertype 0x0806
    0x08, 0x06,
    // 14: hardware type ethernet
    0x00, 0x01,
    // 16: protocol type ipv4
    0x08, 0x00,
    // 18: hardware address length
    0x06,
    // 19: protocol address length
    0x04,
    // 20: operation request
    0x00, 0x01,
    // 22: sender hardware address
    0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E,
    // 28: sender protocol address 192.168.1.10
    0xC0, 0xA8, 0x01, 0x0A,
    // 32: target hardware address
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 38: target protocol address 192.168.1.1
    0xC0, 0xA8, 0x01, 0x01,
};

// Ethernet + IPv6. Ethertype 0x86DD is not IPv4, so no IP layer is decoded.
static const uint8_t frame_ipv6[54] = {
    // 0: dst mac 33:33:00:00:00:01
    0x33, 0x33, 0x00, 0x00, 0x00, 0x01,
    // 6: src mac 00:1a:2b:3c:4d:5e
    0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E,
    // 12: ethertype 0x86DD
    0x86, 0xDD,
    // 14: version 6, traffic class, flow label
    0x60, 0x00, 0x00, 0x00,
    // 18: payload length 0
    0x00, 0x00,
    // 20: next header icmpv6
    0x3A,
    // 21: hop limit
    0x40,
    // 22: source address
    0xFE, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x1A, 0x2B, 0xFF, 0xFE, 0x3C, 0x4D, 0x5E,
    // 38: destination address
    0xFF, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

// IPv4 with 4 bytes of options (ihl 6), so TCP starts at 14+24 not 14+20.
static const uint8_t frame_ip_options[58] = {
    // 0: dst mac 02:00:00:00:00:01
    0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    // 6: src mac 02:00:00:00:00:02
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02,
    // 12: ethertype 0x0800
    0x08, 0x00,
    // 14: version 4, ihl 6
    0x46,
    // 15: dscp and ecn
    0x00,
    // 16: total length 44
    0x00, 0x2C,
    // 18: identification
    0x00, 0x01,
    // 20: flags and fragment offset
    0x00, 0x00,
    // 22: ttl 32
    0x20,
    // 23: protocol 6
    0x06,
    // 24: header checksum
    0x00, 0x00,
    // 26: src 172.16.0.1
    0xAC, 0x10, 0x00, 0x01,
    // 30: dst 172.16.0.2
    0xAC, 0x10, 0x00, 0x02,
    // 34: ip options, nop nop nop end
    0x01, 0x01, 0x01, 0x00,
    // 38: tcp source port 8080
    0x1F, 0x90,
    // 40: tcp dest port 80
    0x00, 0x50,
    // 42: sequence 1
    0x00, 0x00, 0x00, 0x01,
    // 46: acknowledgement 2
    0x00, 0x00, 0x00, 0x02,
    // 50: data offset 5, reserved bits
    0x50,
    // 51: flags ACK
    0x10,
    // 52: window
    0x01, 0x00,
    // 54: checksum
    0x00, 0x00,
    // 56: urgent pointer
    0x00, 0x00,
};

// TCP with data offset 8, so 12 bytes of TCP options follow the fixed header.
static const uint8_t frame_tcp_opts[66] = {
    // 0: dst mac 02:00:00:00:00:03
    0x02, 0x00, 0x00, 0x00, 0x00, 0x03,
    // 6: src mac 02:00:00:00:00:04
    0x02, 0x00, 0x00, 0x00, 0x00, 0x04,
    // 12: ethertype 0x0800
    0x08, 0x00,
    // 14: version 4, ihl 5
    0x45,
    // 15: dscp and ecn
    0x00,
    // 16: total length 52
    0x00, 0x34,
    // 18: identification
    0x00, 0x02,
    // 20: flags and fragment offset
    0x40, 0x00,
    // 22: ttl 64
    0x40,
    // 23: protocol 6
    0x06,
    // 24: header checksum
    0x00, 0x00,
    // 26: src 10.1.2.3
    0x0A, 0x01, 0x02, 0x03,
    // 30: dst 10.4.5.6
    0x0A, 0x04, 0x05, 0x06,
    // 34: tcp source port 22
    0x00, 0x16,
    // 36: tcp dest port 50000
    0xC3, 0x50,
    // 38: sequence 0x11223344
    0x11, 0x22, 0x33, 0x44,
    // 42: acknowledgement 0x55667788
    0x55, 0x66, 0x77, 0x88,
    // 46: data offset 8, reserved bits
    0x80,
    // 47: flags PSH ACK
    0x18,
    // 48: window
    0x20, 0x00,
    // 50: checksum
    0x00, 0x00,
    // 52: urgent pointer
    0x00, 0x00,
    // 54: tcp options, mss then sack permitted then timestamp padding
    0x02, 0x04, 0x05, 0xB4, 0x04, 0x02, 0x08, 0x0A,
    0x00, 0x00, 0x00, 0x00,
};

// IPv4 carrying protocol 1, which the decoder reports as NM_OTHER_IP.
static const uint8_t frame_icmp[42] = {
    // 0: dst mac 02:00:00:00:00:05
    0x02, 0x00, 0x00, 0x00, 0x00, 0x05,
    // 6: src mac 02:00:00:00:00:06
    0x02, 0x00, 0x00, 0x00, 0x00, 0x06,
    // 12: ethertype 0x0800
    0x08, 0x00,
    // 14: version 4, ihl 5
    0x45,
    // 15: dscp and ecn
    0x00,
    // 16: total length 28
    0x00, 0x1C,
    // 18: identification
    0x00, 0x03,
    // 20: flags and fragment offset
    0x00, 0x00,
    // 22: ttl 128
    0x80,
    // 23: protocol 1
    0x01,
    // 24: header checksum
    0x00, 0x00,
    // 26: src 192.168.0.1
    0xC0, 0xA8, 0x00, 0x01,
    // 30: dst 192.168.0.2
    0xC0, 0xA8, 0x00, 0x02,
    // 34: icmp echo request type and code
    0x08, 0x00,
    // 36: icmp checksum
    0xF7, 0xFF,
    // 38: icmp identifier
    0x00, 0x01,
    // 40: icmp sequence
    0x00, 0x01,
};

static int packet_is_zeroed(const Packet *p) {
    Packet zero;
    memset(&zero, 0, sizeof(zero));
    return memcmp(p, &zero, sizeof(zero)) == 0;
}

TEST(tcp_syn_every_field) {
    Packet p;
    ASSERT_EQ(decode_frame(frame_tcp_syn, sizeof(frame_tcp_syn), &p), NSH_OK);
    ASSERT_EQ(p.frame_len, sizeof(frame_tcp_syn));

    ASSERT_EQ(p.eth.dst[0], 0xAA);
    ASSERT_EQ(p.eth.dst[1], 0xBB);
    ASSERT_EQ(p.eth.dst[2], 0xCC);
    ASSERT_EQ(p.eth.dst[3], 0xDD);
    ASSERT_EQ(p.eth.dst[4], 0xEE);
    ASSERT_EQ(p.eth.dst[5], 0xFF);
    ASSERT_EQ(p.eth.src[0], 0x11);
    ASSERT_EQ(p.eth.src[1], 0x22);
    ASSERT_EQ(p.eth.src[2], 0x33);
    ASSERT_EQ(p.eth.src[3], 0x44);
    ASSERT_EQ(p.eth.src[4], 0x55);
    ASSERT_EQ(p.eth.src[5], 0x66);
    ASSERT_EQ(p.eth.ethertype, 0x0800);

    ASSERT_TRUE(p.has_ip);
    ASSERT_EQ(p.ip.ihl, 5);
    ASSERT_EQ(p.ip.ttl, 64);
    ASSERT_EQ(p.ip.protocol, 6);
    ASSERT_EQ(p.ip.total_len, 40);
    ASSERT_EQ(p.ip.src[0], 192);
    ASSERT_EQ(p.ip.src[1], 168);
    ASSERT_EQ(p.ip.src[2], 1);
    ASSERT_EQ(p.ip.src[3], 10);
    ASSERT_EQ(p.ip.dst[0], 93);
    ASSERT_EQ(p.ip.dst[1], 184);
    ASSERT_EQ(p.ip.dst[2], 216);
    ASSERT_EQ(p.ip.dst[3], 34);

    ASSERT_EQ(p.proto, NM_TCP);
    // 0xC0DE read big-endian is 49374, not the byte-swapped 56896.
    ASSERT_EQ(p.tcp.sport, 49374);
    ASSERT_EQ(p.tcp.dport, 443);
    ASSERT_EQ(p.tcp.seq, 0xDEADBEEFu);
    ASSERT_EQ(p.tcp.ack, 0u);
    ASSERT_EQ(p.tcp.flags, TCP_SYN);
}

TEST(udp_dns_query) {
    Packet p;
    ASSERT_EQ(decode_frame(frame_udp_dns, sizeof(frame_udp_dns), &p), NSH_OK);
    ASSERT_EQ(p.frame_len, sizeof(frame_udp_dns));
    ASSERT_EQ(p.eth.ethertype, 0x0800);
    ASSERT_TRUE(p.has_ip);
    ASSERT_EQ(p.ip.protocol, 17);
    ASSERT_EQ(p.ip.total_len, 40);
    ASSERT_EQ(p.ip.src[0], 10);
    ASSERT_EQ(p.ip.dst[0], 8);
    ASSERT_EQ(p.ip.dst[3], 8);
    ASSERT_EQ(p.proto, NM_UDP);
    ASSERT_EQ(p.udp.sport, 61453);
    ASSERT_EQ(p.udp.dport, 53);
    ASSERT_EQ(p.udp.len, 20);
    ASSERT_EQ(p.tcp.sport, 0);
}

TEST(arp_frame_is_eth_only) {
    Packet p;
    ASSERT_EQ(decode_frame(frame_arp, sizeof(frame_arp), &p), NSH_OK);
    ASSERT_EQ(p.frame_len, 42);
    ASSERT_EQ(p.eth.dst[0], 0xFF);
    ASSERT_EQ(p.eth.dst[1], 0xFF);
    ASSERT_EQ(p.eth.dst[2], 0xFF);
    ASSERT_EQ(p.eth.dst[3], 0xFF);
    ASSERT_EQ(p.eth.dst[4], 0xFF);
    ASSERT_EQ(p.eth.dst[5], 0xFF);
    ASSERT_EQ(p.eth.src[0], 0x00);
    ASSERT_EQ(p.eth.src[1], 0x1A);
    ASSERT_EQ(p.eth.src[2], 0x2B);
    ASSERT_EQ(p.eth.src[3], 0x3C);
    ASSERT_EQ(p.eth.src[4], 0x4D);
    ASSERT_EQ(p.eth.src[5], 0x5E);
    ASSERT_EQ(p.eth.ethertype, 0x0806);
    ASSERT_TRUE(!p.has_ip);
    ASSERT_EQ(p.proto, NM_NONE);
    ASSERT_EQ(p.ip.ihl, 0);
    ASSERT_EQ(p.ip.total_len, 0);
}

TEST(ipv6_ethertype_is_eth_only) {
    Packet p;
    ASSERT_EQ(decode_frame(frame_ipv6, sizeof(frame_ipv6), &p), NSH_OK);
    ASSERT_EQ(p.eth.ethertype, 0x86DD);
    ASSERT_TRUE(!p.has_ip);
    ASSERT_EQ(p.proto, NM_NONE);
    ASSERT_EQ(p.frame_len, sizeof(frame_ipv6));
}

TEST(eth_only_minimum_length) {
    // Exactly 14 bytes of Ethernet and nothing else is still a valid frame.
    Packet p;
    ASSERT_EQ(decode_frame(frame_arp, 14, &p), NSH_OK);
    ASSERT_EQ(p.eth.ethertype, 0x0806);
    ASSERT_TRUE(!p.has_ip);
    ASSERT_EQ(p.frame_len, 14);
}

TEST(ip_options_shift_transport_offset) {
    Packet p;
    ASSERT_EQ(decode_frame(frame_ip_options, sizeof(frame_ip_options), &p),
              NSH_OK);
    ASSERT_TRUE(p.has_ip);
    ASSERT_EQ(p.ip.ihl, 6);
    ASSERT_EQ(p.ip.total_len, 44);
    ASSERT_EQ(p.ip.ttl, 32);
    ASSERT_EQ(p.proto, NM_TCP);
    // Reading TCP at a fixed 14+20 would land on the options and give 257.
    ASSERT_EQ(p.tcp.sport, 8080);
    ASSERT_EQ(p.tcp.dport, 80);
    ASSERT_EQ(p.tcp.seq, 1u);
    ASSERT_EQ(p.tcp.ack, 2u);
    ASSERT_EQ(p.tcp.flags, TCP_ACK);
}

TEST(tcp_data_offset_eight) {
    Packet p;
    ASSERT_EQ(decode_frame(frame_tcp_opts, sizeof(frame_tcp_opts), &p), NSH_OK);
    ASSERT_EQ(p.frame_len, 66);
    ASSERT_EQ(p.ip.total_len, 52);
    ASSERT_EQ(p.proto, NM_TCP);
    ASSERT_EQ(p.tcp.sport, 22);
    ASSERT_EQ(p.tcp.dport, 50000);
    ASSERT_EQ(p.tcp.seq, 0x11223344u);
    ASSERT_EQ(p.tcp.ack, 0x55667788u);
    ASSERT_EQ(p.tcp.flags, TCP_PSH | TCP_ACK);
}

TEST(tcp_flags_syn_ack) {
    uint8_t f[sizeof(frame_tcp_syn)];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    f[OFF_TCP_FLAGS] = TCP_SYN | TCP_ACK;
    f[OFF_TCP_ACK] = 0x12;
    f[OFF_TCP_ACK + 1] = 0x34;
    f[OFF_TCP_ACK + 2] = 0x56;
    f[OFF_TCP_ACK + 3] = 0x78;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_OK);
    ASSERT_EQ(p.tcp.flags, 0x12);
    ASSERT_TRUE((p.tcp.flags & TCP_SYN) != 0);
    ASSERT_TRUE((p.tcp.flags & TCP_ACK) != 0);
    ASSERT_TRUE((p.tcp.flags & TCP_FIN) == 0);
    ASSERT_EQ(p.tcp.ack, 0x12345678u);
}

TEST(tcp_flags_fin_psh_ack) {
    uint8_t f[sizeof(frame_tcp_syn)];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    f[OFF_TCP_FLAGS] = TCP_FIN | TCP_PSH | TCP_ACK;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_OK);
    ASSERT_EQ(p.tcp.flags, 0x19);
    ASSERT_TRUE((p.tcp.flags & TCP_FIN) != 0);
    ASSERT_TRUE((p.tcp.flags & TCP_PSH) != 0);
    ASSERT_TRUE((p.tcp.flags & TCP_ACK) != 0);
    ASSERT_TRUE((p.tcp.flags & TCP_SYN) == 0);
    ASSERT_TRUE((p.tcp.flags & TCP_RST) == 0);
}

TEST(tcp_flags_rst) {
    uint8_t f[sizeof(frame_tcp_syn)];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    f[OFF_TCP_FLAGS] = TCP_RST;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_OK);
    ASSERT_EQ(p.tcp.flags, TCP_RST);
    ASSERT_TRUE((p.tcp.flags & TCP_SYN) == 0);
    ASSERT_TRUE((p.tcp.flags & TCP_ACK) == 0);
}

TEST(icmp_is_other_ip) {
    Packet p;
    ASSERT_EQ(decode_frame(frame_icmp, sizeof(frame_icmp), &p), NSH_OK);
    ASSERT_TRUE(p.has_ip);
    ASSERT_EQ(p.proto, NM_OTHER_IP);
    ASSERT_EQ(p.ip.protocol, 1);
    ASSERT_EQ(p.ip.ihl, 5);
    ASSERT_EQ(p.ip.ttl, 128);
    ASSERT_EQ(p.ip.total_len, 28);
    ASSERT_EQ(p.ip.src[0], 192);
    ASSERT_EQ(p.ip.dst[3], 2);
    ASSERT_EQ(p.tcp.sport, 0);
    ASSERT_EQ(p.udp.sport, 0);
}

TEST(ethernet_padding_is_tolerated) {
    // A 60-byte wire frame carrying a 40-byte IP packet plus 6 bytes of pad.
    uint8_t f[60];
    Packet p;
    memset(f, 0, sizeof(f));
    memcpy(f, frame_tcp_syn, sizeof(frame_tcp_syn));
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_OK);
    ASSERT_EQ(p.frame_len, 60);
    ASSERT_EQ(p.ip.total_len, 40);
    ASSERT_EQ(p.proto, NM_TCP);
    ASSERT_EQ(p.tcp.sport, 49374);
    ASSERT_EQ(p.tcp.dport, 443);
    ASSERT_EQ(p.tcp.flags, TCP_SYN);
}

TEST(hostile_len_zero) {
    Packet p;
    ASSERT_EQ(decode_frame(frame_tcp_syn, 0, &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_len_thirteen) {
    Packet p;
    ASSERT_EQ(decode_frame(frame_tcp_syn, 13, &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_ip_header_cut_short) {
    // 19 bytes of a 20-byte IPv4 header.
    Packet p;
    ASSERT_EQ(decode_frame(frame_tcp_syn, 14 + 19, &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_ihl_overruns_frame) {
    uint8_t f[sizeof(frame_tcp_syn)];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    // ihl 15 claims a 60-byte header inside 40 captured bytes.
    f[OFF_IP_VER_IHL] = 0x4F;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_total_len_past_frame) {
    uint8_t f[sizeof(frame_tcp_syn)];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    // total length 255 in a frame that holds 40 bytes after Ethernet.
    f[OFF_IP_TOTAL_LEN] = 0x00;
    f[OFF_IP_TOTAL_LEN + 1] = 0xFF;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_total_len_under_header) {
    uint8_t f[sizeof(frame_tcp_syn)];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    // total length 16 is shorter than the 20-byte header it describes.
    f[OFF_IP_TOTAL_LEN] = 0x00;
    f[OFF_IP_TOTAL_LEN + 1] = 0x10;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_version_six_in_ipv4_frame) {
    uint8_t f[sizeof(frame_tcp_syn)];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    f[OFF_IP_VER_IHL] = 0x65;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_ihl_four) {
    // ICMP has no transport check, so only the ihl >= 5 rule can reject this.
    uint8_t f[sizeof(frame_icmp)];
    Packet p;
    memcpy(f, frame_icmp, sizeof(f));
    f[OFF_IP_VER_IHL] = 0x44;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_ihl_four_tcp) {
    uint8_t f[sizeof(frame_tcp_syn)];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    f[OFF_IP_VER_IHL] = 0x44;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_tcp_payload_under_twenty) {
    uint8_t f[sizeof(frame_tcp_syn)];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    // total length 36 leaves 16 bytes for a header that needs 20.
    f[OFF_IP_TOTAL_LEN] = 0x00;
    f[OFF_IP_TOTAL_LEN + 1] = 0x24;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_tcp_header_cut_at_frame_end) {
    // Exact-size buffer: touching the data offset byte would run off the end.
    uint8_t f[38];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    // total length 24 leaves 4 bytes where a 20-byte TCP header must sit.
    f[OFF_IP_TOTAL_LEN] = 0x00;
    f[OFF_IP_TOTAL_LEN + 1] = 0x18;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_udp_header_cut_at_frame_end) {
    // Exact-size buffer: touching the length field would run off the end.
    uint8_t f[36];
    Packet p;
    memcpy(f, frame_udp_dns, sizeof(f));
    // total length 22 leaves 2 bytes where an 8-byte UDP header must sit.
    f[OFF_IP_TOTAL_LEN] = 0x00;
    f[OFF_IP_TOTAL_LEN + 1] = 0x16;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_tcp_data_offset_under_five) {
    uint8_t f[sizeof(frame_tcp_syn)];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    f[OFF_TCP_DOFF] = 0x40;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_tcp_data_offset_past_payload) {
    uint8_t f[sizeof(frame_tcp_syn)];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    // data offset 15 claims 60 header bytes inside a 20-byte IP payload.
    f[OFF_TCP_DOFF] = 0xF0;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_udp_len_seven) {
    uint8_t f[sizeof(frame_udp_dns)];
    Packet p;
    memcpy(f, frame_udp_dns, sizeof(f));
    f[OFF_UDP_LEN] = 0x00;
    f[OFF_UDP_LEN + 1] = 0x07;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_udp_len_past_payload) {
    uint8_t f[sizeof(frame_udp_dns)];
    Packet p;
    memcpy(f, frame_udp_dns, sizeof(f));
    // udp length 100 inside a 20-byte IP payload.
    f[OFF_UDP_LEN] = 0x00;
    f[OFF_UDP_LEN + 1] = 0x64;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_udp_payload_under_eight) {
    uint8_t f[sizeof(frame_udp_dns)];
    Packet p;
    memcpy(f, frame_udp_dns, sizeof(f));
    // total length 24 leaves 4 bytes for an 8-byte UDP header.
    f[OFF_IP_TOTAL_LEN] = 0x00;
    f[OFF_IP_TOTAL_LEN + 1] = 0x18;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(hostile_null_frame) {
    Packet p;
    ASSERT_EQ(decode_frame(NULL, 64, &p), NSH_ERR_INVALID);
    ASSERT_TRUE(packet_is_zeroed(&p));
}

TEST(unknown_ethertype_leaves_no_stale_ip) {
    uint8_t f[sizeof(frame_tcp_syn)];
    Packet p;
    memcpy(f, frame_tcp_syn, sizeof(f));
    // An IPv4 body behind a non-IPv4 ethertype must not be decoded.
    f[OFF_ETHERTYPE] = 0x88;
    f[OFF_ETHERTYPE + 1] = 0xA8;
    ASSERT_EQ(decode_frame(f, sizeof(f), &p), NSH_OK);
    ASSERT_EQ(p.eth.ethertype, 0x88A8);
    ASSERT_TRUE(!p.has_ip);
    ASSERT_EQ(p.proto, NM_NONE);
    ASSERT_EQ(p.ip.protocol, 0);
    ASSERT_EQ(p.tcp.dport, 0);
}

TEST(reused_out_struct_is_cleared) {
    Packet p;
    ASSERT_EQ(decode_frame(frame_tcp_syn, sizeof(frame_tcp_syn), &p), NSH_OK);
    ASSERT_EQ(p.tcp.dport, 443);
    ASSERT_EQ(decode_frame(frame_udp_dns, sizeof(frame_udp_dns), &p), NSH_OK);
    ASSERT_EQ(p.proto, NM_UDP);
    ASSERT_EQ(p.udp.dport, 53);
    ASSERT_EQ(p.tcp.dport, 0);
    ASSERT_EQ(p.tcp.seq, 0u);
    ASSERT_EQ(decode_frame(frame_arp, sizeof(frame_arp), &p), NSH_OK);
    ASSERT_TRUE(!p.has_ip);
    ASSERT_EQ(p.udp.dport, 0);
    ASSERT_EQ(p.ip.total_len, 0);
}

TEST_MAIN()
