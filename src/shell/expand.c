// Variable and tilde expansion: one pass per segment, never rescanning output.

#include "expand.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../alloc/alloc.h"
#include "../util/str.h"
#include "../util/vec.h"

// A sign plus the digits of an int fit well under this.
#define STATUS_BUF 24

static bool is_name_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9');
}

// getenv needs a NUL terminated key, and the name here is only a slice.
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

// A leading ~ is HOME only as a whole first component of the word. Returns the
// bytes of text consumed, so 1 when it expanded and 0 when it stays literal.
// An unset or empty HOME leaves the tilde alone rather than aiming at /.
static size_t expand_tilde(const char *text, bool word_end, Str *out) {
    if (text[0] != '~') {
        return 0;
    }
    if (text[1] != '/' && !(text[1] == '\0' && word_end)) {
        return 0;
    }
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return 0;
    }
    str_append(out, home);
    return 1;
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
            // A brace form never continues into the next segment.
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
            // Only the word's first segment can carry a tilde prefix.
            size_t skip = (i == 0) ? expand_tilde(seg->text, tok->segs.len == 1,
                                                  &acc)
                                   : 0;
            NshError err = expand_segment(seg->text + skip, last_status, &acc);
            if (err != NSH_OK) {
                str_free(&acc);
                return err;
            }
        } else {
            str_append(&acc, seg->text);
        }
    }

    *out = str_take(&acc);
    // str_take left acc holding a fresh buffer, which still needs freeing.
    str_free(&acc);
    return NSH_OK;
}
