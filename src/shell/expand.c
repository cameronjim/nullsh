// Variable expansion. One pass per expandable segment: copy bytes through
// until a $, decide which of the three forms it starts, append the replacement,
// continue after it. Substituted text is never rescanned, so a value holding
// "$HOME" stays those five characters, and no expansion ever splits a word.

#include "expand.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../alloc/alloc.h"
#include "../util/str.h"
#include "../util/vec.h"

// Widest $? can get is a sign plus the digits of an int, far under this.
#define STATUS_BUF 24

static bool is_name_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9');
}

// Appends the environment value of the len-byte name at p, or nothing when the
// variable is unset. getenv needs a NUL terminated key, and the name here is a
// slice of the segment, so it gets its own copy.
static void append_env(Str *out, const char *p, size_t len) {
    char *key = nsh_malloc(len + 1);
    memcpy(key, p, len);
    key[len] = '\0';
    const char *val = getenv(key);
    nsh_free(key);
    if (val != NULL) {
        str_append(out, val);
    }
}

// Length of the longest name starting at p, zero when p starts no name.
static size_t name_len(const char *p) {
    if (!is_name_start(p[0])) {
        return 0;
    }
    size_t n = 1;
    while (is_name_char(p[n])) {
        n++;
    }
    return n;
}

static NshError expand_segment(const char *text, int last_status, Str *out) {
    size_t i = 0;
    while (text[i] != '\0') {
        if (text[i] != '$') {
            str_push(out, text[i]);
            i++;
            continue;
        }

        char next = text[i + 1];
        if (next == '?') {
            char buf[STATUS_BUF];
            snprintf(buf, sizeof(buf), "%d", last_status);
            str_append(out, buf);
            i += 2;
        } else if (next == '{') {
            size_t n = name_len(text + i + 2);
            // Empty braces and a name not closed by } inside this segment are
            // both errors; the brace form never continues into the next
            // segment, since quoting cannot split a variable reference.
            if (n == 0 || text[i + 2 + n] != '}') {
                return NSH_ERR_SYNTAX;
            }
            append_env(out, text + i + 2, n);
            i += n + 3;
        } else {
            size_t n = name_len(text + i + 1);
            if (n == 0) {
                // $5, $-, a lone $ at the end: the dollar is ordinary text.
                str_push(out, '$');
                i++;
            } else {
                append_env(out, text + i + 1, n);
                i += n + 1;
            }
        }
    }
    return NSH_OK;
}

NshError expand_word(const Token *tok, int last_status, char **out) {
    if (out == NULL) {
        return NSH_ERR_INVALID;
    }
    *out = NULL;
    if (tok == NULL || tok->kind != TOK_WORD) {
        return NSH_ERR_INVALID;
    }

    Str acc;
    str_init(&acc);
    for (size_t i = 0; i < tok->segs.len; i++) {
        const WordSeg *seg = vec_get(&tok->segs, i);
        if (seg == NULL || seg->text == NULL) {
            continue;
        }
        if (seg->expand) {
            NshError err = expand_segment(seg->text, last_status, &acc);
            if (err != NSH_OK) {
                str_free(&acc);
                return err;
            }
        } else {
            str_append(&acc, seg->text);
        }
    }

    *out = str_take(&acc);
    // str_take handed the buffer over and left acc holding a fresh empty one.
    str_free(&acc);
    return NSH_OK;
}
