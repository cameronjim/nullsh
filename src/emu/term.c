// Terminal lifecycle for the emulator: raw mode, nonblocking stdin, cursor.

#define _POSIX_C_SOURCE 200809L

#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define ESC_HIDE_CURSOR "\x1b[?25l"
#define ESC_SHOW_CURSOR "\x1b[?25h"
#define ESC_CLEAR "\x1b[2J"

// POSIX forces this: the saved state has to outlive the call that took it.
static struct termios saved_termios;
static int saved_flags;
static bool raw_active;

static void warn(const char *what) {
    fprintf(stderr, "nullsh: emu: %s: %s\n", what, strerror(errno));
}

static NshError write_all(const char *s) {
    size_t len = strlen(s);
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(STDOUT_FILENO, s + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            warn("write");
            return NSH_ERR_IO;
        }
        done += (size_t)n;
    }
    return NSH_OK;
}

NshError term_enter_raw(void) {
    if (raw_active) {
        return NSH_OK;
    }
    if (tcgetattr(STDIN_FILENO, &saved_termios) != 0) {
        warn("tcgetattr");
        return NSH_ERR_IO;
    }
    saved_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (saved_flags < 0) {
        warn("fcntl");
        return NSH_ERR_IO;
    }

    struct termios raw = saved_termios;
    // ISIG stays on: Ctrl-C and Ctrl-Z must still reach job control.
    // OPOST stays on: rendered rows end in a bare '\n' and need the CR.
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN);
    raw.c_iflag &= (tcflag_t) ~(ICRNL | IXON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        warn("tcsetattr");
        return NSH_ERR_IO;
    }
    if (fcntl(STDIN_FILENO, F_SETFL, saved_flags | O_NONBLOCK) < 0) {
        warn("fcntl");
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios) != 0) {
            warn("tcsetattr");
        }
        return NSH_ERR_IO;
    }

    raw_active = true;
    if (write_all(ESC_HIDE_CURSOR ESC_CLEAR) != NSH_OK) {
        term_exit_raw();
        return NSH_ERR_IO;
    }
    return NSH_OK;
}

void term_exit_raw(void) {
    if (!raw_active) {
        return;
    }
    raw_active = false;
    (void)write_all(ESC_SHOW_CURSOR);
    if (fcntl(STDIN_FILENO, F_SETFL, saved_flags) < 0) {
        warn("fcntl");
    }
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios) != 0) {
        warn("tcsetattr");
    }
}

int term_read_key(void) {
    unsigned char ch;
    ssize_t n = read(STDIN_FILENO, &ch, 1);
    // EAGAIN means nothing pending, EINTR means a signal landed, 0 means EOF.
    if (n != 1) {
        return -1;
    }
    return (int)ch;
}
