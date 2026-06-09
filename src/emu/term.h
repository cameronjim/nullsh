// Terminal lifecycle for the emulator: raw mode, nonblocking stdin, cursor.

#pragma once

#include "../util/error.h"

// Saves the termios state and stdin flags, drops echo and canonical mode,
// makes stdin nonblocking, hides the cursor and clears the screen.
// Ctrl-C and Ctrl-Z keep generating signals. A second call is a no-op.
NshError term_enter_raw(void);

// Restores termios, the stdin flags and the cursor. Safe without an enter and
// safe to call twice.
void term_exit_raw(void);

// term_exit_raw for a signal handler: only tcsetattr, fcntl and write, all of
// which POSIX lists as async-signal-safe. No error reporting, no stdio.
void term_emergency_restore(void);

// One byte from stdin, or -1 when nothing is pending.
int term_read_key(void);
