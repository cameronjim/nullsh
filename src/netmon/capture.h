// The raw AF_PACKET socket: bind to one interface, read frames, close.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "../util/error.h"

// IFNAMSIZ is 16, so 32 leaves room and keeps the struct copyable.
typedef struct {
    int fd;
    char ifname[32];
} Capture;

// Opens SOCK_RAW on ifname. Every failure prints one stderr line first.
NshError capture_open(Capture *c, const char *ifname);

// Blocks for one frame: bytes read, 0 on EINTR so the caller re-checks its stop flag, -1 on error.
ssize_t capture_recv(Capture *c, uint8_t *buf, size_t cap);

void capture_close(Capture *c);
