// Proto and port tests deciding which decoded packets netmon prints.

#include "filter.h"

static bool proto_match(const Packet *p, NmFilterProto want) {
    switch (want) {
    case NM_FILTER_TCP:
        return p->proto == NM_TCP;
    case NM_FILTER_UDP:
        return p->proto == NM_UDP;
    case NM_FILTER_ALL:
        break;
    }
    return true;
}

// Ports live in the transport header, so anything without one cannot match.
static bool port_match(const Packet *p, int port) {
    if (port < 0) {
        return true;
    }
    uint16_t want = (uint16_t)port;
    if (p->proto == NM_TCP) {
        return p->tcp.sport == want || p->tcp.dport == want;
    }
    if (p->proto == NM_UDP) {
        return p->udp.sport == want || p->udp.dport == want;
    }
    return false;
}

bool filter_match(const Packet *p, const NetmonFilter *f) {
    return proto_match(p, f->proto) && port_match(p, f->port);
}
