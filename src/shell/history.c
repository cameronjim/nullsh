// Fixed-capacity ring of command lines, saved one entry per line oldest first.

#include "history.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../alloc/alloc.h"

// strdup would use libc malloc, which is off limits outside src/alloc.
static char *dup_cstr(const char *s, size_t len) {
    char *copy = nsh_malloc(len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

// Ring slot holding the entry i places after the oldest one.
static size_t slot_of(const History *h, size_t i) {
    return (h->start + i) % h->cap;
}

NshError history_init(History *h, size_t cap) {
    if (h == NULL || cap == 0) {
        return NSH_ERR_INVALID;
    }
    h->lines = nsh_calloc(cap, sizeof(*h->lines));
    h->cap = cap;
    h->len = 0;
    h->start = 0;
    return NSH_OK;
}

void history_free(History *h) {
    if (h == NULL || h->lines == NULL) {
        return;
    }
    for (size_t i = 0; i < h->len; i++) {
        nsh_free(h->lines[slot_of(h, i)]);
    }
    nsh_free(h->lines);
    h->lines = NULL;
    h->cap = 0;
    h->len = 0;
    h->start = 0;
}

void history_add(History *h, const char *line) {
    if (h == NULL || h->lines == NULL || line == NULL || line[0] == '\0') {
        return;
    }
    // Only an immediate repeat is dropped; the same line later is a new entry.
    if (h->len > 0) {
        const char *newest = h->lines[slot_of(h, h->len - 1)];
        if (strcmp(newest, line) == 0) {
            return;
        }
    }
    if (h->len == h->cap) {
        nsh_free(h->lines[h->start]);
        h->lines[h->start] = NULL;
        h->start = (h->start + 1) % h->cap;
        h->len--;
    }
    h->lines[slot_of(h, h->len)] = dup_cstr(line, strlen(line));
    h->len++;
}

size_t history_count(const History *h) {
    if (h == NULL || h->lines == NULL) {
        return 0;
    }
    return h->len;
}

const char *history_get(const History *h, size_t i) {
    if (h == NULL || h->lines == NULL || i >= h->len) {
        return NULL;
    }
    return h->lines[slot_of(h, i)];
}

// A history line has no length limit, so the read buffer grows.
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} LineBuf;

static void linebuf_init(LineBuf *b) {
    b->cap = 64;
    b->len = 0;
    b->data = nsh_malloc(b->cap);
    b->data[0] = '\0';
}

static void linebuf_push(LineBuf *b, char c) {
    // Keep one byte spare for the terminator.
    if (b->len + 1 >= b->cap) {
        b->cap *= 2;
        b->data = nsh_realloc(b->data, b->cap);
    }
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void linebuf_reset(LineBuf *b) {
    b->len = 0;
    b->data[0] = '\0';
}

NshError history_load(History *h, const char *path) {
    if (h == NULL || h->lines == NULL || path == NULL) {
        return NSH_ERR_INVALID;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        // No history file yet is the normal state on a fresh machine.
        return errno == ENOENT ? NSH_OK : NSH_ERR_IO;
    }

    LineBuf buf;
    linebuf_init(&buf);
    NshError err = NSH_OK;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) {
            // A last line without a trailing newline still counts.
            if (buf.len > 0) {
                history_add(h, buf.data);
            }
            if (ferror(f)) {
                err = NSH_ERR_IO;
            }
            break;
        }
        if (c == '\n') {
            history_add(h, buf.data);
            linebuf_reset(&buf);
            continue;
        }
        linebuf_push(&buf, (char)c);
    }

    nsh_free(buf.data);
    if (fclose(f) != 0) {
        err = NSH_ERR_IO;
    }
    return err;
}

NshError history_save(const History *h, const char *path) {
    if (h == NULL || h->lines == NULL || path == NULL) {
        return NSH_ERR_INVALID;
    }
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return NSH_ERR_IO;
    }

    NshError err = NSH_OK;
    for (size_t i = 0; i < h->len && err == NSH_OK; i++) {
        if (fputs(h->lines[slot_of(h, i)], f) == EOF || fputc('\n', f) == EOF) {
            err = NSH_ERR_IO;
        }
    }

    // Data can still fail to reach the disk at close, so fclose decides too.
    if (fclose(f) != 0 && err == NSH_OK) {
        err = NSH_ERR_IO;
    }
    return err;
}
