# Phase 7: netmon, the packet viewer

## Goal

`netmon eth0` prints one decoded line per packet arriving on the interface; `--filter tcp|udp` and `--port N` narrow it. Backgroundable (`netmon eth0 &` plus the phase 4 machinery). Requires elevated privileges for the socket, by design.

## Concepts this phase teaches

### Encapsulation is Russian dolls

Every packet on the wire is an Ethernet frame wrapping an IP packet wrapping a TCP segment or UDP datagram wrapping bytes. Decoding is peeling: 14 bytes of Ethernet (dst MAC, src MAC, EtherType), EtherType 0x0800 says IPv4 follows, the IP header's protocol byte says 6=TCP or 17=UDP, and the transport header carries the ports. Each layer knows only its neighbors.

### Network byte order

Multi-byte fields on the wire are big-endian. Reading them on x86 (little-endian) without converting gives garbage like port 20480 instead of 80. Every 16/32-bit field goes through explicit shifts (no ntohs dependence in the pure decoder: build the value from bytes, which is endian-proof and shows the mechanism).

### The IP header lies in interesting ways

IHL (header length in 4-byte words) is variable: options exist, so the transport header starts at ihl*4, not at 20. total_length can be less than the captured frame (Ethernet pads short frames to 60 bytes). Trust neither: validate IHL >= 5, total_length within the frame, transport header within total_length.

### Raw sockets and privilege

socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)) delivers every frame on an interface, which is why it needs CAP_NET_RAW. nullsh takes the honest path: run the shell under sudo when you want netmon (documented), no setcap on the binary (a capability-blessed shell binary is a bad default). Tests that need the socket live behind make test-net and skip cleanly when not root.

### Never trust the wire

Same discipline as ELF: validate lengths at every layer before reading fields. A truncated or malicious frame yields a "short frame" line, never a crash. The decoder is pure bytes-to-struct and is tested with canned hostile frames.

## Contracts (decode.h written by Fable, committed with this plan)

Packet struct with EthHeader, optional Ipv4Header, and a TCP/UDP/OTHER union tail; decode_frame(bytes, len) -> NSH_ERR_INVALID on any truncation. Addresses kept as byte arrays (formatting is print.c's job), ports/lengths as host-order integers built by explicit shifts.

## Waves

- Agent A (clone): src/netmon/decode.c + decode_test.c. Canned frames as hex byte arrays: a real-shaped TCP SYN, a UDP DNS query, ARP (non-IP EtherType -> eth-only result), IPv6 EtherType (other), IP options (IHL 6) shifting the transport offset, and truncations at every boundary (13 bytes, 20, mid-IP, IHL claiming past the end, transport header cut short). Flag decoding for TCP (SYN/ACK/FIN/RST/PSH letters).
- Agent B (clone): src/netmon/print.c/h, pure: format one Packet into a Str: "IP 1.2.3.4:443 > 5.6.7.8:51000 TCP SA len 60" shapes, MAC/eth-only lines for non-IP, plus src/netmon/filter.c/h: bool filter_match(const Packet*, const NetmonFilter*) with proto/port fields; tests hand-build Packets.
- Wave 2 (main repo): src/netmon/capture.c (socket, bind to interface by name via if_nametoindex + sockaddr_ll, recv loop honoring a stop flag) + netmon.c builtin (arg parsing, loop: capture -> decode -> filter -> print, SIGINT-friendly stop when foregrounded... the shell ignores SIGINT; netmon stops on any key? Simplest honest control: netmon runs until Ctrl-C when run under sudo bash, or as a nullsh background job it runs until `kill %1`. Decide in wave 2 with the constraint that it must not wedge the shell) + builtin registration + make test-net target (sudo-gated: loopback capture of a self-generated UDP packet to 127.0.0.1, assert the decoded line appears; skip cleanly when not root).

## Exit criteria

Dual-pass suite green without root; make test-net green under sudo (Fable runs it); manual sudo session decoding real traffic (curl + dig) verified by Fable; docs updated; lowercase tldr commit.
