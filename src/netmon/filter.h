// Proto and port tests deciding which decoded packets netmon prints.

#pragma once

#include <stdbool.h>

#include "decode.h"

typedef enum { NM_FILTER_ALL, NM_FILTER_TCP, NM_FILTER_UDP } NmFilterProto;

typedef struct {
    NmFilterProto proto;
    int port;  // -1 matches any port
} NetmonFilter;

// True when the packet passes the proto test and the port test.
bool filter_match(const Packet *p, const NetmonFilter *f);
