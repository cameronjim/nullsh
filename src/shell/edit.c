// Interactive line editing: a pure edit buffer, a key decoder, and the tty loop.

#define _POSIX_C_SOURCE 200809L

#include "edit.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../emu/term.h"

// Room for "\x1b[" plus the digits of a size_t plus 'C' and the NUL.
#define CURSOR_SEQ_BUF 32

#define ESC_CLEAR_EOL "\x1b[K"

static bool is_blank(char c) { return c == ' ' || c == '\t'; }

void edit_init(EditLine *e) {
    str_init(&e->buf);
    e->cursor = 0;
}

void edit_free(EditLine *e) {
    str_free(&e->buf);
    e->cursor = 0;
}

void edit_insert(EditLine *e, char c) {
    if (e->cursor > e->buf.len) {
        e->cursor = e->buf.len;
    }
    size_t at = e->cursor;
    // The push grows the buffer and re-terminates it; the move opens the gap.
    str_push(&e->buf, c);
    memmove(e->buf.data + at + 1, e->buf.data + at, e->buf.len - at - 1);
    e->buf.data[at] = c;
    e->cursor = at + 1;
}

void edit_backspace(EditLine *e) {
    if (e->cursor == 0 || e->buf.len == 0) {
        return;
    }
    char *d = e->buf.data;
    memmove(d + e->cursor - 1, d + e->cursor, e->buf.len - e->cursor + 1);
    e->buf.len--;
    e->cursor--;
}

void edit_delete(EditLine *e) {
    if (e->cursor >= e->buf.len) {
        return;
    }
    char *d = e->buf.data;
    memmove(d + e->cursor, d + e->cursor + 1, e->buf.len - e->cursor);
    e->buf.len--;
}

void edit_left(EditLine *e) {
    if (e->cursor > 0) {
        e->cursor--;
    }
}

void edit_right(EditLine *e) {
    if (e->cursor < e->buf.len) {
        e->cursor++;
    }
}

void edit_home(EditLine *e) { e->cursor = 0; }

void edit_end(EditLine *e) { e->cursor = e->buf.len; }

void edit_kill_to_end(EditLine *e) {
    if (e->cursor >= e->buf.len) {
        return;
    }
    e->buf.len = e->cursor;
    e->buf.data[e->buf.len] = '\0';
}

void edit_kill_to_start(EditLine *e) {
    if (e->cursor == 0) {
        return;
    }
    char *d = e->buf.data;
    memmove(d, d + e->cursor, e->buf.len - e->cursor + 1);
    e->buf.len -= e->cursor;
    e->cursor = 0;
}

void edit_kill_word_left(EditLine *e) {
    size_t i = e->cursor;
    while (i > 0 && is_blank(e->buf.data[i - 1])) {
        i--;
    }
    while (i > 0 && !is_blank(e->buf.data[i - 1])) {
        i--;
    }
    if (i == e->cursor) {
        return;
    }
    char *d = e->buf.data;
    memmove(d + i, d + e->cursor, e->buf.len - e->cursor + 1);
    e->buf.len -= e->cursor - i;
    e->cursor = i;
}

void edit_set(EditLine *e, const char *text) {
    str_clear(&e->buf);
    if (text != NULL) {
        str_append(&e->buf, text);
    }
    e->cursor = e->buf.len;
}

// Decoder states. KS_CSI_IGNORE swallows a sequence we will not act on.
enum { KS_GROUND, KS_ESC, KS_CSI, KS_CSI_PARAM, KS_SS3, KS_CSI_IGNORE };

void editkeys_init(EditKeys *k) {
    k->state = KS_GROUND;
    k->npending = 0;
    memset(k->pending, 0, sizeof k->pending);
}

static EditKey feed_ground(EditKeys *k, unsigned char b, char *ch) {
    switch (b) {
    case 0x1b:
        k->state = KS_ESC;
        k->npending = 0;
        return EK_NONE;
    case 0x0d:
    case 0x0a:
        return EK_ENTER;
    case 0x7f:
    case 0x08:
        return EK_BACKSPACE;
    case 0x01:
        return EK_HOME;
    case 0x03:
        return EK_INTERRUPT;
    case 0x04:
        return EK_EOF;
    case 0x05:
        return EK_END;
    case 0x0b:
        return EK_KILL_END;
    case 0x15:
        return EK_KILL_START;
    case 0x17:
        return EK_KILL_WORD;
    default:
        break;
    }
    // Every other control byte is dropped rather than drawn as garbage.
    if (b < 0x20) {
        return EK_NONE;
    }
    if (ch != NULL) {
        *ch = (char)b;
    }
    return EK_CHAR;
}

static EditKey final_letter(unsigned char b) {
    switch (b) {
    case 'A':
        return EK_UP;
    case 'B':
        return EK_DOWN;
    case 'C':
        return EK_RIGHT;
    case 'D':
        return EK_LEFT;
    case 'H':
        return EK_HOME;
    case 'F':
        return EK_END;
    default:
        return EK_NONE;
    }
}

// The "ESC [ <n> ~" family, which is how several terminals send Home and End.
static EditKey tilde_key(const EditKeys *k) {
    if (k->npending != 1) {
        return EK_NONE;
    }
    switch (k->pending[0]) {
    case '1':
    case '7':
        return EK_HOME;
    case '3':
        return EK_DELETE;
    case '4':
    case '8':
        return EK_END;
    default:
        return EK_NONE;
    }
}

EditKey editkeys_feed(EditKeys *k, unsigned char b, char *ch) {
    switch (k->state) {
    case KS_ESC:
        if (b == '[') {
            k->state = KS_CSI;
            return EK_NONE;
        }
        if (b == 'O') {
            k->state = KS_SS3;
            return EK_NONE;
        }
        // A bare ESC: abandon the sequence and let the byte stand on its own.
        k->state = KS_GROUND;
        return feed_ground(k, b, ch);

    case KS_CSI:
        if (b >= '0' && b <= '9') {
            k->state = KS_CSI_PARAM;
            k->pending[0] = b;
            k->npending = 1;
            return EK_NONE;
        }
        k->state = KS_GROUND;
        return final_letter(b);

    case KS_CSI_PARAM:
        if (b >= '0' && b <= '9') {
            if (k->npending < sizeof k->pending) {
                k->pending[k->npending++] = b;
            } else {
                k->state = KS_CSI_IGNORE;
            }
            return EK_NONE;
        }
        if (b == ';') {
            // A modifier parameter follows, so this is not a key we act on.
            k->state = KS_CSI_IGNORE;
            return EK_NONE;
        }
        k->state = KS_GROUND;
        return (b == '~') ? tilde_key(k) : EK_NONE;

    case KS_CSI_IGNORE:
        if (b >= 0x40 && b <= 0x7e) {
            k->state = KS_GROUND;
        }
        return EK_NONE;

    case KS_SS3:
        k->state = KS_GROUND;
        return final_letter(b);

    default:
        return feed_ground(k, b, ch);
    }
}

static NshError write_all(const char *p, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(STDOUT_FILENO, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return NSH_ERR_IO;
        }
        done += (size_t)n;
    }
    return NSH_OK;
}

// The whole line is repainted every keystroke: carriage return, prompt, buffer,
// clear to end of line, carriage return again, then one cursor-right jump.
// It costs a few bytes per key and it cannot drift out of sync. The cost is
// that a line wider than the terminal wraps and the jump lands on the wrong row.
static NshError redraw(const char *prompt, const EditLine *e) {
    Str o;
    str_init(&o);
    str_push(&o, '\r');
    str_append(&o, prompt);
    str_append_n(&o, e->buf.data, e->buf.len);
    str_append(&o, ESC_CLEAR_EOL);
    str_push(&o, '\r');
    // The prompt is plain text, so its byte count is its column count.
    size_t col = strlen(prompt) + e->cursor;
    if (col > 0) {
        char seq[CURSOR_SEQ_BUF];
        snprintf(seq, sizeof seq, "\x1b[%zuC", col);
        str_append(&o, seq);
    }
    NshError err = write_all(o.data, o.len);
    str_free(&o);
    return err;
}

// Up walks back from the newest entry; the slot below the newest holds the
// line that was being typed, stashed on the first Up and restored on the way
// back down. Edits made to a recalled entry are dropped by the next recall.
typedef struct {
    History *h;
    size_t count;
    size_t idx;
    Str stash;
} Recall;

static void recall_init(Recall *r, History *h) {
    r->h = h;
    r->count = history_count(h);
    r->idx = r->count;
    str_init(&r->stash);
}

static void recall_free(Recall *r) { str_free(&r->stash); }

static void recall_up(Recall *r, EditLine *e) {
    if (r->idx == 0) {
        return;
    }
    if (r->idx == r->count) {
        str_clear(&r->stash);
        str_append_n(&r->stash, e->buf.data, e->buf.len);
    }
    r->idx--;
    edit_set(e, history_get(r->h, r->idx));
}

static void recall_down(Recall *r, EditLine *e) {
    if (r->idx >= r->count) {
        return;
    }
    r->idx++;
    edit_set(e, r->idx == r->count ? r->stash.data : history_get(r->h, r->idx));
}

// Returns true once the line is finished; *err carries what to hand back.
static bool apply_key(EditKey key, char ch, EditLine *e, Recall *r,
                      NshError *err) {
    switch (key) {
    case EK_CHAR:
        edit_insert(e, ch);
        break;
    case EK_BACKSPACE:
        edit_backspace(e);
        break;
    case EK_DELETE:
        edit_delete(e);
        break;
    case EK_LEFT:
        edit_left(e);
        break;
    case EK_RIGHT:
        edit_right(e);
        break;
    case EK_HOME:
        edit_home(e);
        break;
    case EK_END:
        edit_end(e);
        break;
    case EK_KILL_END:
        edit_kill_to_end(e);
        break;
    case EK_KILL_START:
        edit_kill_to_start(e);
        break;
    case EK_KILL_WORD:
        edit_kill_word_left(e);
        break;
    case EK_UP:
        recall_up(r, e);
        break;
    case EK_DOWN:
        recall_down(r, e);
        break;
    case EK_ENTER:
        *err = NSH_OK;
        return true;
    case EK_INTERRUPT:
        // Bash-like: abandon the line, show a fresh prompt, run nothing.
        edit_set(e, NULL);
        *err = NSH_OK;
        return true;
    case EK_EOF:
        if (e->buf.len == 0) {
            *err = NSH_EOF;
            return true;
        }
        // Ctrl-D on a line with text deletes forward, like every other shell.
        edit_delete(e);
        break;
    default:
        break;
    }
    return false;
}

NshError edit_read_line(const char *prompt, History *h, Str *out) {
    if (prompt == NULL || out == NULL) {
        return NSH_ERR_INVALID;
    }
    str_clear(out);
    if (term_enter_raw_mode(TERM_RAW_LINE) != NSH_OK) {
        return NSH_ERR_IO;
    }

    EditLine e;
    EditKeys keys;
    Recall r;
    edit_init(&e);
    editkeys_init(&keys);
    recall_init(&r, h);

    NshError err = redraw(prompt, &e);
    bool done = false;
    while (err == NSH_OK && !done) {
        unsigned char b = 0;
        NshError rd = term_read_byte(&b);
        if (rd != NSH_OK) {
            err = rd;
            break;
        }
        char ch = 0;
        EditKey key = editkeys_feed(&keys, b, &ch);
        if (key == EK_NONE) {
            continue;
        }
        done = apply_key(key, ch, &e, &r, &err);
        if (!done) {
            err = redraw(prompt, &e);
        }
    }

    if (err == NSH_OK) {
        str_append_n(out, e.buf.data, e.buf.len);
    }
    // The REPL prints its own newline for end of input, so it is skipped here.
    if (err != NSH_EOF) {
        (void)write_all("\r\n", 2);
    }
    // The terminal is cooked again before anything else writes to it.
    term_exit_raw();
    recall_free(&r);
    edit_free(&e);
    return err;
}
