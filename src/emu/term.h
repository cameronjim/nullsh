// Terminal lifecycle for the emulator: raw mode, nonblocking stdin, cursor.

#pragma once

#include "../util/error.h"

// TERM_RAW_SCREEN owns the whole display; TERM_RAW_LINE owns one line.
typedef enum { TERM_RAW_SCREEN, TERM_RAW_LINE } TermRawMode;

// TERM_RAW_SCREEN: nonblocking stdin, cursor hidden, screen cleared, and
// Ctrl-C and Ctrl-Z still generating signals.
// TERM_RAW_LINE: blocking stdin one byte at a time, screen and cursor left
// alone, and ISIG off so Ctrl-C and Ctrl-Z arrive as bytes the editor reads.
// Either way the termios state and the stdin flags are saved for the exit.
// A second call is a no-op, so the first mode wins until term_exit_raw.
NshError term_enter_raw_mode(TermRawMode mode);

// term_enter_raw_mode(TERM_RAW_SCREEN).
NshError term_enter_raw(void);

// Restores termios, the stdin flags and the cursor. Safe without an enter and
// safe to call twice.
void term_exit_raw(void);

// term_exit_raw for a signal handler: only tcsetattr, fcntl and write, all of
// which POSIX lists as async-signal-safe. No error reporting, no stdio.
void term_emergency_restore(void);

// One byte from stdin, or -1 when nothing is pending.
int term_read_key(void);

// One byte from stdin, blocking in TERM_RAW_LINE and retrying past EINTR.
// NSH_EOF when the input ends, NSH_ERR_IO on a real read failure.
NshError term_read_byte(unsigned char *out);
