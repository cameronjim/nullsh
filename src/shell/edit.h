// Interactive line editing: a pure edit buffer, a key decoder, and the tty loop.

#pragma once

#include <stddef.h>

#include "history.h"
#include "../util/error.h"
#include "../util/str.h"

// The edited line and the insertion point, which is always <= buf.len.
typedef struct {
    Str buf;
    size_t cursor;
} EditLine;

void edit_init(EditLine *e);
void edit_free(EditLine *e);
void edit_insert(EditLine *e, char c);
void edit_backspace(EditLine *e);
void edit_delete(EditLine *e);
void edit_left(EditLine *e);
void edit_right(EditLine *e);
void edit_home(EditLine *e);
void edit_end(EditLine *e);
void edit_kill_to_end(EditLine *e);
void edit_kill_to_start(EditLine *e);

// Drops the run of spaces left of the cursor plus the word before it.
void edit_kill_word_left(EditLine *e);

// Replaces the whole line and parks the cursor at the end. NULL means empty.
void edit_set(EditLine *e, const char *text);

typedef enum {
    EK_NONE,
    EK_CHAR,
    EK_ENTER,
    EK_BACKSPACE,
    EK_DELETE,
    EK_LEFT,
    EK_RIGHT,
    EK_HOME,
    EK_END,
    EK_UP,
    EK_DOWN,
    EK_KILL_END,
    EK_KILL_START,
    EK_KILL_WORD,
    EK_EOF,
    EK_INTERRUPT
} EditKey;

// pending holds the numeric parameter of a CSI sequence while it arrives.
typedef struct {
    int state;
    unsigned char pending[8];
    size_t npending;
} EditKeys;

void editkeys_init(EditKeys *k);

// One byte in, one action out. EK_NONE while a sequence is still incomplete
// and for anything unrecognised, so junk never lands in the buffer.
// *ch is written only for EK_CHAR and may be NULL otherwise.
EditKey editkeys_feed(EditKeys *k, unsigned char byte, char *ch);

// Reads one line with editing and history recall, entering and leaving raw
// mode around the call so external commands see a cooked terminal.
// NSH_OK with out set, NSH_EOF for Ctrl-D on an empty line, NSH_ERR_IO on a
// terminal failure. Ctrl-C returns NSH_OK with an empty line.
NshError edit_read_line(const char *prompt, History *h, Str *out);
