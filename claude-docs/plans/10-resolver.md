# phase 10: the dns resolver

nullsh learns to speak a network protocol instead of only watching one. netmon
(phase 7) decodes packets other programs created; `resolve` builds a DNS query
byte by byte from RFC 1035, sends it over UDP, and parses the answer, name
compression and all. Run `netmon` in one job and `resolve` in another and you
can watch your own query cross the wire.

## Concepts

**Network byte order, again, on purpose.** Every multi-byte field in a DNS
message is big endian. As in netmon, the wire codec uses explicit byte shifts
(`(uint16_t)(p[0] << 8 | p[1])`), never ntohs/htons, because writing the
shifts is the lesson. Socket plumbing (sockaddr_in, inet_pton for the user's
dotted server address) is exempt: that is API convention, not protocol.

**Names are labels, not strings.** `example.com` on the wire is
`7 e x a m p l e 3 c o m 0`: length-prefixed labels ending in a zero. A label
caps at 63 bytes, a whole name at 255. There is no NUL-terminated string
anywhere in the packet.

**Compression pointers are the famous trap.** To avoid repeating names, a
length byte with the top two bits set (0xC0) turns the next 14 bits into an
offset back into the packet where the name continues. A malicious or broken
packet can point a pointer at itself or forward into garbage, so the parser
enforces two rules: a pointer must target an offset strictly lower than its
own position, and one name may follow at most 32 pointers. Either violation is
a parse error, never a hang.

**UDP is allowed to lose your packet.** DNS clients own their reliability: a
timeout and a resend with the same id. The reply's id must match the query's
or the reply is a stray (or an off-path forgery; the plan's security note
below). If the reply has the TC bit, the server truncated it to fit 512
bytes; real clients retry over TCP, nullsh prints a truncation notice and
shows what arrived (documented limitation).

**Why ids should be unpredictable.** An attacker who can guess the id and
source port can answer before the real server does (cache poisoning). nullsh
seeds the id from the monotonic clock xor the pid, which is enough for an
educational tool and gets a note in the manual about why real resolvers do
more.

**Ctrl-C during a blocked wait.** The shell ignores SIGINT, so a lone builtin
stuck in poll(2) would shrug it off. Like netmon, net.c installs its own
SIGINT flag handler (no SA_RESTART) for the duration of the exchange and
restores the old disposition on the way out; an EINTR wakes poll, the flag is
checked, and the builtin leaves with status 130.

## The wire format nullsh implements

Query: 12-byte header (id, flags with RD set, qdcount 1), one question
(QNAME as labels, QTYPE A = 1, QCLASS IN = 1). No EDNS.

Reply parsing: full header decode (qr aa tc rd ra, rcode, all four counts),
skip qdcount questions, then read ancount resource records: name (compression
allowed), type, class, ttl, rdlength, rdata. A records (type 1, rdlength 4)
and CNAMEs (type 5, rdata is a name) are decoded; every other type is kept as
name, type and length only. Every read is bounds-checked against the packet
length; a record that runs off the end is a parse error, in the inspect
tradition of validating before following.

## The builtin

```
resolve NAME [--server IP] [--port N] [--timeout MS] [--tries N]
```

Defaults: the first IPv4 `nameserver` line of /etc/resolv.conf (an IPv6
nameserver is skipped; none found is an error telling the user to pass
--server), port 53, timeout 2000 ms per try, 2 tries. QTYPE is always A: this
resolver asks for IPv4 addresses, matching netmon's IPv4-only scope.

Output, pinned exactly so tests can diff it:

```
;; id 4242 flags qr rd ra rcode NOERROR answers 2
example.com. 300 IN CNAME edge.example.net.
edge.example.net. 60 IN A 93.184.216.34
```

- The `;;` line always comes first: id, the flags that are set (of qr aa tc rd
  ra, in that order), `rcode NAME`, `answers N`.
- One line per answer record: name with trailing dot, ttl, class (`IN`, or
  `CLASS%u`), type, then the address, the CNAME target with trailing dot, or
  `TYPE%u (%u bytes)` for anything else.
- After a reply with tc set: `;; truncated reply` on its own line, then the
  records that did arrive.
- After a reply with ra clear: `;; recursion not available`.
- A reply whose id does not match: `nullsh: resolve: reply id mismatch` and
  status 1 (no retry loop for this; documented simplification).

Exit status: 0 when a reply with rcode NOERROR was received and parsed, even
with zero answers. 1 for everything else: usage errors, no nameserver, socket
failure, timeout after all tries, NXDOMAIN and the other rcodes, id mismatch,
malformed reply. 130 when Ctrl-C interrupted the wait.

Errors print as `nullsh: resolve: <reason>` in the netmon style, and a usage
line on bad arguments.

## Module map (contracts in the headers, frozen)

| File | Owns |
|---|---|
| src/resolve/dns.h/.c | pure wire codec: build query, parse reply, rcode names. No I/O, no sockets, no printing |
| src/resolve/net.h/.c | one UDP exchange: socket, send, poll with timeout, retry, SIGINT watch. No parsing |
| src/resolve/resolve.h/.c | the builtin: argument parsing, resolv.conf, id generation and matching, output formatting |
| src/shell/builtin.c | registration and the help text line |

DnsRecord uses fixed 256-byte name buffers rather than heap strings: the spec
caps names at 255 bytes, so the bound is real, and parsing stays allocation
free except for the record vector itself.

## Agent ownership (one each, disjoint)

| Agent | Files | Notes |
|---|---|---|
| dns | dns.c, dns_test.c | fully testable alone: canned packets both directions |
| net | net.c, net_test.c | tests fork a canned UDP server on 127.0.0.1 |
| resolve | resolve.c, resolve_test.c, src/shell/builtin.c, builtin_test.c, tests/integration/11_resolve.sh | formatting and conf parsing unit-test against hand-built records; end-to-end checks in the integration script run for real only after the codec and net land at integration |
| docs | docs/manual.md, README.md, claude-docs/architecture.md | resolve section, feature line, module row and decision bullets |

## Testing requirements

- dns_test.c: query encoding (label splitting, the 63 and 255 byte caps, a
  trailing dot accepted, empty name and empty labels rejected); reply decoding
  from hand-written byte arrays (single A, CNAME chain, compression with one
  and with several jumps); the two pointer rules (forward pointer rejected,
  self pointer rejected, 33 jumps rejected); a truncation sweep chopping a
  valid reply at every length; rdlength lying beyond the packet; unknown
  types preserved as counts; qdcount of 2 skipped correctly; rcode strings.
- net_test.c: fork a sacrificial child bound to an ephemeral 127.0.0.1 UDP
  port. Round trip; a silent server times out after the configured tries; a
  server that answers only the second datagram proves the retry; oversized
  reply is clipped to the caller's buffer without corruption; SIGINT during
  the wait returns the interrupted code (raise it from a timer in the child or
  the test process).
- resolve_test.c: resolv.conf parsing against fixture files (comments, blank
  lines, IPv6 nameserver skipped, no usable line); argument errors; id
  mismatch handling; the pinned output format from hand-built DnsHeader and
  record vectors, byte for byte.
- 11_resolve.sh: usage errors and their statuses, the no-nameserver error with
  a --server-less call against an empty HOME-style conf (use --server with an
  unroutable address and 1 try and a short timeout for the deterministic
  failure path), help listing resolve, and one real lookup (resolve
  example.com) that SKIPs rather than fails when the sandbox has no DNS.
- Everything green under both allocator strategies; ASan is on.

## Deliberate limitations (manual gets these)

A records only (no AAAA, MX, TXT), no TCP fallback on truncation, no EDNS, one
question per query, IPv4 servers only, /etc/resolv.conf is the only config
read, no search domains or ndots, ids are time-seeded not cryptographic, a
mismatched reply id aborts instead of re-listening.
