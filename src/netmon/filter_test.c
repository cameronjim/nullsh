// Tests for netmon's proto and port filter, over hand built Packets.

#include "filter.h"

#include "../../tests/harness.h"

static Packet tcp_packet(uint16_t sport, uint16_t dport) {
    Packet p = {
        .has_ip = true,
        .ip = {.ihl = 5, .protocol = 6, .src = {1, 2, 3, 4}, .dst = {5, 6, 7, 8}},
        .proto = NM_TCP,
        .tcp = {.sport = sport, .dport = dport},
        .frame_len = 60,
    };
    return p;
}

static Packet udp_packet(uint16_t sport, uint16_t dport) {
    Packet p = {
        .has_ip = true,
        .ip = {.ihl = 5, .protocol = 17, .src = {1, 2, 3, 4}, .dst = {5, 6, 7, 8}},
        .proto = NM_UDP,
        .udp = {.sport = sport, .dport = dport},
        .frame_len = 48,
    };
    return p;
}

// ICMP: an IPv4 packet with no ports at all.
static Packet other_ip_packet(void) {
    Packet p = {
        .has_ip = true,
        .ip = {.ihl = 5, .protocol = 1, .src = {1, 2, 3, 4}, .dst = {5, 6, 7, 8}},
        .proto = NM_OTHER_IP,
        .frame_len = 84,
    };
    return p;
}

// ARP: decode stops at the Ethernet header, so proto stays NM_NONE.
static Packet non_ip_packet(void) {
    Packet p = {
        .eth = {.ethertype = 0x0806},
        .has_ip = false,
        .proto = NM_NONE,
        .frame_len = 42,
    };
    return p;
}

TEST(all_with_the_wildcard_port_matches_every_kind_of_frame) {
    NetmonFilter f = {NM_FILTER_ALL, -1};
    Packet tcp = tcp_packet(1234, 80);
    Packet udp = udp_packet(5353, 53);
    Packet other = other_ip_packet();
    Packet eth = non_ip_packet();

    ASSERT_TRUE(filter_match(&tcp, &f));
    ASSERT_TRUE(filter_match(&udp, &f));
    ASSERT_TRUE(filter_match(&other, &f));
    ASSERT_TRUE(filter_match(&eth, &f));
}

TEST(the_tcp_filter_takes_tcp_and_nothing_else) {
    NetmonFilter f = {NM_FILTER_TCP, -1};
    Packet tcp = tcp_packet(1234, 80);
    Packet udp = udp_packet(5353, 53);
    Packet other = other_ip_packet();
    Packet eth = non_ip_packet();

    ASSERT_TRUE(filter_match(&tcp, &f));
    ASSERT_TRUE(!filter_match(&udp, &f));
    ASSERT_TRUE(!filter_match(&other, &f));
    ASSERT_TRUE(!filter_match(&eth, &f));
}

TEST(the_udp_filter_takes_udp_and_nothing_else) {
    NetmonFilter f = {NM_FILTER_UDP, -1};
    Packet tcp = tcp_packet(1234, 80);
    Packet udp = udp_packet(5353, 53);
    Packet other = other_ip_packet();
    Packet eth = non_ip_packet();

    ASSERT_TRUE(!filter_match(&tcp, &f));
    ASSERT_TRUE(filter_match(&udp, &f));
    ASSERT_TRUE(!filter_match(&other, &f));
    ASSERT_TRUE(!filter_match(&eth, &f));
}

TEST(a_port_matches_on_either_side_of_the_conversation) {
    NetmonFilter f = {NM_FILTER_ALL, 443};
    Packet as_source = tcp_packet(443, 51000);
    Packet as_dest = tcp_packet(51000, 443);
    Packet neither = tcp_packet(51000, 80);

    ASSERT_TRUE(filter_match(&as_source, &f));
    ASSERT_TRUE(filter_match(&as_dest, &f));
    ASSERT_TRUE(!filter_match(&neither, &f));
}

TEST(a_port_reads_the_udp_header_for_udp_packets) {
    NetmonFilter f = {NM_FILTER_ALL, 53};
    Packet as_source = udp_packet(53, 40000);
    Packet as_dest = udp_packet(40000, 53);
    Packet neither = udp_packet(40000, 123);

    ASSERT_TRUE(filter_match(&as_source, &f));
    ASSERT_TRUE(filter_match(&as_dest, &f));
    ASSERT_TRUE(!filter_match(&neither, &f));
}

// The TCP and UDP headers are separate fields, and only the live one counts.
TEST(a_udp_packet_ignores_whatever_sits_in_its_tcp_header) {
    NetmonFilter f = {NM_FILTER_ALL, 9999};
    Packet p = udp_packet(53, 40000);
    p.tcp.sport = 9999;
    p.tcp.dport = 9999;

    ASSERT_TRUE(!filter_match(&p, &f));
}

TEST(a_tcp_packet_ignores_whatever_sits_in_its_udp_header) {
    NetmonFilter f = {NM_FILTER_ALL, 9999};
    Packet p = tcp_packet(443, 51000);
    p.udp.sport = 9999;
    p.udp.dport = 9999;

    ASSERT_TRUE(!filter_match(&p, &f));
}

// A port filter is a statement about the transport layer, so frames without
// one are out even under NM_FILTER_ALL.
TEST(a_port_filter_rejects_non_ip_and_other_ip_frames) {
    Packet other = other_ip_packet();
    Packet eth = non_ip_packet();

    NetmonFilter all = {NM_FILTER_ALL, 80};
    ASSERT_TRUE(!filter_match(&other, &all));
    ASSERT_TRUE(!filter_match(&eth, &all));

    NetmonFilter zero = {NM_FILTER_ALL, 0};
    ASSERT_TRUE(!filter_match(&other, &zero));
    ASSERT_TRUE(!filter_match(&eth, &zero));

    NetmonFilter tcp_only = {NM_FILTER_TCP, 80};
    ASSERT_TRUE(!filter_match(&other, &tcp_only));
    ASSERT_TRUE(!filter_match(&eth, &tcp_only));
}

TEST(port_zero_is_a_real_port_not_a_wildcard) {
    NetmonFilter f = {NM_FILTER_ALL, 0};
    Packet has_it = udp_packet(0, 53);
    Packet lacks_it = udp_packet(5353, 53);

    ASSERT_TRUE(filter_match(&has_it, &f));
    ASSERT_TRUE(!filter_match(&lacks_it, &f));
}

TEST(the_highest_port_number_matches) {
    NetmonFilter f = {NM_FILTER_ALL, 65535};
    Packet has_it = tcp_packet(1234, 65535);
    Packet lacks_it = tcp_packet(1234, 65534);

    ASSERT_TRUE(filter_match(&has_it, &f));
    ASSERT_TRUE(!filter_match(&lacks_it, &f));
}

TEST(the_wildcard_port_lets_every_port_through_each_proto_filter) {
    Packet tcp = tcp_packet(0, 65535);
    Packet udp = udp_packet(0, 65535);

    NetmonFilter tcp_any = {NM_FILTER_TCP, -1};
    NetmonFilter udp_any = {NM_FILTER_UDP, -1};

    ASSERT_TRUE(filter_match(&tcp, &tcp_any));
    ASSERT_TRUE(filter_match(&udp, &udp_any));
}

// Both tests run, so a UDP packet on the port a TCP filter wants is out.
TEST(proto_and_port_both_have_to_pass) {
    NetmonFilter tcp_53 = {NM_FILTER_TCP, 53};
    Packet udp_on_53 = udp_packet(40000, 53);
    Packet tcp_on_53 = tcp_packet(40000, 53);
    Packet tcp_on_80 = tcp_packet(40000, 80);

    ASSERT_TRUE(!filter_match(&udp_on_53, &tcp_53));
    ASSERT_TRUE(filter_match(&tcp_on_53, &tcp_53));
    ASSERT_TRUE(!filter_match(&tcp_on_80, &tcp_53));

    NetmonFilter udp_53 = {NM_FILTER_UDP, 53};
    ASSERT_TRUE(filter_match(&udp_on_53, &udp_53));
    ASSERT_TRUE(!filter_match(&tcp_on_53, &udp_53));
}

TEST_MAIN()
