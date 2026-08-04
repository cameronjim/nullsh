// The raw AF_PACKET socket: bind to one interface, read frames, close.

// sockaddr_ll and the linux packet headers are outside the POSIX set.
#define _DEFAULT_SOURCE

#include "capture.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void capture_err(const char *ifname, const char *what, int err) {
    if (err == EPERM || err == EACCES) {
        fprintf(stderr, "nullsh: netmon: %s: needs root, try sudo nullsh\n",
                ifname);
        return;
    }
    fprintf(stderr, "nullsh: netmon: %s: %s: %s\n", ifname, what,
            strerror(err));
}

NshError capture_open(Capture *c, const char *ifname) {
    c->fd = -1;
    c->ifname[0] = '\0';

    size_t len = (ifname == NULL) ? 0 : strlen(ifname);
    if (len == 0 || len >= sizeof c->ifname) {
        fprintf(stderr, "nullsh: netmon: interface name is empty or too long\n");
        return NSH_ERR_IO;
    }

    // The socket comes first so a non-root run reports the privilege, not the name.
    int fd = socket(AF_PACKET, SOCK_RAW, (int)htons(ETH_P_ALL));
    if (fd < 0) {
        capture_err(ifname, "socket", errno);
        return NSH_ERR_IO;
    }

    unsigned int index = if_nametoindex(ifname);
    if (index == 0) {
        capture_err(ifname, "if_nametoindex", errno);
        (void)close(fd);
        return NSH_ERR_IO;
    }

    // Binding narrows the socket to one interface; promiscuous mode is not set.
    struct sockaddr_ll addr;
    memset(&addr, 0, sizeof addr);
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    addr.sll_ifindex = (int)index;
    if (bind(fd, (const struct sockaddr *)&addr, sizeof addr) != 0) {
        capture_err(ifname, "bind", errno);
        (void)close(fd);
        return NSH_ERR_IO;
    }

    c->fd = fd;
    memcpy(c->ifname, ifname, len + 1);
    return NSH_OK;
}

ssize_t capture_recv(Capture *c, uint8_t *buf, size_t cap) {
    ssize_t n = recv(c->fd, buf, cap, 0);
    if (n < 0 && errno == EINTR) {
        return 0;
    }
    return n;
}

void capture_close(Capture *c) {
    if (c->fd >= 0) {
        (void)close(c->fd);
    }
    c->fd = -1;
}
