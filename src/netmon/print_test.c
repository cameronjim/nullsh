// Tests for the packet printer: every line shape, exact to the byte.

#include "print.h"

#include <stdio.h>
#include <string.h>

#include "../../tests/harness.h"

#define WANT_BUF 256

static Packet tcp_packet(void) {
    Packet p = {
        .eth = {.dst = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},
                .src = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff},
                .ethertype = 0x0800},
        .has_ip = true,
        .ip = {.ihl = 5,
               .ttl = 64,
               .protocol = 6,
               .total_len = 46,
               .src = {10, 0, 0, 1},
               .dst = {93, 184, 216, 34}},
        .proto = NM_TCP,
        .tcp = {.sport = 49374,
                .dport = 443,
                .seq = 1000,
                .ack = 0,
                .flags = TCP_SYN | TCP_ACK},
        .frame_len = 60,
    };
    return p;
}

static Packet udp_packet(void) {
    Packet p = {
        .eth = {.ethertype = 0x0800},
        .has_ip = true,
        .ip = {.ihl = 5,
               .ttl = 64,
               .protocol = 17,
               .total_len = 34,
               .src = {10, 0, 0, 1},
               .dst = {8, 8, 8, 8}},
        .proto = NM_UDP,
        .udp = {.sport = 5353, .dport = 53, .len = 14},
        .frame_len = 48,
    };
    return p;
}

static Packet other_ip_packet(void) {
    Packet p = {
        .eth = {.ethertype = 0x0800},
        .has_ip = true,
        .ip = {.ihl = 5,
               .ttl = 64,
               .protocol = 1,
               .total_len = 84,
               .src = {10, 0, 0, 1},
               .dst = {8, 8, 8, 8}},
        .proto = NM_OTHER_IP,
        .frame_len = 84,
    };
    return p;
}

static Packet non_ip_packet(void) {
    Packet p = {
        .eth = {.dst = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},
                .src = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff},
                .ethertype = 0x0806},
        .has_ip = false,
        .proto = NM_NONE,
        .frame_len = 42,
    };
    return p;
}

TEST(a_tcp_segment_prints_endpoints_flags_seq_and_frame_length) {
    Packet p = tcp_packet();
    Str out;
    str_init(&out);

    packet_format(&p, &out);
    ASSERT_STR_EQ(out.data,
                  "IP 10.0.0.1:49374 > 93.184.216.34:443 TCP [SA] seq 1000 "
                  "len 60");

    str_free(&out);
}

TEST(a_udp_datagram_prints_endpoints_and_frame_length) {
    Packet p = udp_packet();
    Str out;
    str_init(&out);

    packet_format(&p, &out);
    ASSERT_STR_EQ(out.data, "IP 10.0.0.1:5353 > 8.8.8.8:53 UDP len 48");

    str_free(&out);
}

TEST(another_ip_protocol_prints_bare_addresses_and_the_protocol_number) {
    Packet p = other_ip_packet();
    Str out;
    str_init(&out);

    packet_format(&p, &out);
    ASSERT_STR_EQ(out.data, "IP 10.0.0.1 > 8.8.8.8 proto 1 len 84");

    str_free(&out);
}

TEST(a_non_ip_frame_prints_macs_and_the_ethertype) {
    Packet p = non_ip_packet();
    Str out;
    str_init(&out);

    packet_format(&p, &out);
    ASSERT_STR_EQ(
        out.data,
        "ETH aa:bb:cc:dd:ee:ff > 11:22:33:44:55:66 type 0x0806 len 42");

    str_free(&out);
}

TEST(a_broadcast_destination_prints_as_all_ff) {
    Packet p = non_ip_packet();
    for (size_t i = 0; i < 6; i++) {
        p.eth.dst[i] = 0xff;
        p.eth.src[i] = 0x00;
    }
    Str out;
    str_init(&out);

    packet_format(&p, &out);
    ASSERT_STR_EQ(
        out.data,
        "ETH 00:00:00:00:00:00 > ff:ff:ff:ff:ff:ff type 0x0806 len 42");

    str_free(&out);
}

// Short ethertypes keep their leading zeroes, so the column stays four wide.
TEST(the_ethertype_is_four_lowercase_hex_digits) {
    Packet p = non_ip_packet();
    p.eth.ethertype = 0x86dd;
    Str out;
    str_init(&out);

    packet_format(&p, &out);
    ASSERT_TRUE(strstr(out.data, " type 0x86dd len ") != NULL);

    p.eth.ethertype = 0x0060;
    packet_format(&p, &out);
    ASSERT_TRUE(strstr(out.data, " type 0x0060 len ") != NULL);

    p.eth.ethertype = 0;
    packet_format(&p, &out);
    ASSERT_TRUE(strstr(out.data, " type 0x0000 len ") != NULL);

    str_free(&out);
}

TEST(tcp_flag_letters_come_out_in_fsrpa_order) {
    static const struct {
        uint8_t bits;
        const char *want;
    } cases[] = {
        {0, "[.]"},
        {TCP_FIN, "[F]"},
        {TCP_SYN, "[S]"},
        {TCP_RST, "[R]"},
        {TCP_PSH, "[P]"},
        {TCP_ACK, "[A]"},
        {TCP_SYN | TCP_ACK, "[SA]"},
        {TCP_ACK | TCP_SYN, "[SA]"},
        {TCP_FIN | TCP_ACK, "[FA]"},
        {TCP_PSH | TCP_ACK, "[PA]"},
        {TCP_RST | TCP_ACK, "[RA]"},
        {TCP_ACK | TCP_PSH | TCP_FIN, "[FPA]"},
        {TCP_FIN | TCP_SYN | TCP_RST | TCP_PSH | TCP_ACK, "[FSRPA]"},
    };

    Str out;
    str_init(&out);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Packet p = tcp_packet();
        p.tcp.flags = cases[i].bits;
        packet_format(&p, &out);

        char want[WANT_BUF];
        snprintf(want, sizeof want,
                 "IP 10.0.0.1:49374 > 93.184.216.34:443 TCP %s seq 1000 len 60",
                 cases[i].want);
        ASSERT_STR_EQ(out.data, want);
    }
    str_free(&out);
}

// URG and the reserved bits above ACK have no letter, so they read as none set.
TEST(flag_bits_the_printer_does_not_name_are_left_out) {
    Packet p = tcp_packet();
    p.tcp.flags = 0xe0;
    Str out;
    str_init(&out);

    packet_format(&p, &out);
    ASSERT_STR_EQ(
        out.data,
        "IP 10.0.0.1:49374 > 93.184.216.34:443 TCP [.] seq 1000 len 60");

    p.tcp.flags = 0xe0 | TCP_ACK;
    packet_format(&p, &out);
    ASSERT_STR_EQ(
        out.data,
        "IP 10.0.0.1:49374 > 93.184.216.34:443 TCP [A] seq 1000 len 60");

    str_free(&out);
}

TEST(port_zero_and_port_65535_print_as_written) {
    Packet tcp = tcp_packet();
    tcp.tcp.sport = 0;
    tcp.tcp.dport = 65535;
    Str out;
    str_init(&out);

    packet_format(&tcp, &out);
    ASSERT_STR_EQ(
        out.data,
        "IP 10.0.0.1:0 > 93.184.216.34:65535 TCP [SA] seq 1000 len 60");

    Packet udp = udp_packet();
    udp.udp.sport = 65535;
    udp.udp.dport = 0;
    packet_format(&udp, &out);
    ASSERT_STR_EQ(out.data, "IP 10.0.0.1:65535 > 8.8.8.8:0 UDP len 48");

    str_free(&out);
}

TEST(address_octets_print_across_the_whole_range) {
    Packet p = other_ip_packet();
    for (size_t i = 0; i < 4; i++) {
        p.ip.src[i] = 0;
        p.ip.dst[i] = 255;
    }
    p.ip.protocol = 255;
    p.frame_len = 0;
    Str out;
    str_init(&out);

    packet_format(&p, &out);
    ASSERT_STR_EQ(out.data,
                  "IP 0.0.0.0 > 255.255.255.255 proto 255 len 0");

    str_free(&out);
}

TEST(the_largest_sequence_number_prints_unsigned) {
    Packet p = tcp_packet();
    p.tcp.seq = 4294967295u;
    Str out;
    str_init(&out);

    packet_format(&p, &out);
    ASSERT_STR_EQ(out.data,
                  "IP 10.0.0.1:49374 > 93.184.216.34:443 TCP [SA] seq "
                  "4294967295 len 60");

    str_free(&out);
}

TEST(the_line_carries_no_trailing_newline) {
    Packet p = tcp_packet();
    Str out;
    str_init(&out);

    packet_format(&p, &out);
    ASSERT_TRUE(out.len > 0);
    ASSERT_EQ(out.len, strlen(out.data));
    ASSERT_TRUE(out.data[out.len - 1] != '\n');
    ASSERT_TRUE(strchr(out.data, '\n') == NULL);

    str_free(&out);
}

TEST(formatting_clears_whatever_the_caller_left_in_the_string) {
    Packet p = udp_packet();
    Str out;
    str_init(&out);
    str_append(&out, "stale text from the previous packet");

    packet_format(&p, &out);
    ASSERT_STR_EQ(out.data, "IP 10.0.0.1:5353 > 8.8.8.8:53 UDP len 48");

    // A second run over a longer line then a shorter one still starts clean.
    Packet tcp = tcp_packet();
    packet_format(&tcp, &out);
    packet_format(&p, &out);
    ASSERT_STR_EQ(out.data, "IP 10.0.0.1:5353 > 8.8.8.8:53 UDP len 48");

    str_free(&out);
}

TEST_MAIN()
