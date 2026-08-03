// Growable byte string on top of nsh_malloc.

#include "str.h"

#include <string.h>

#include "../alloc/alloc.h"

#define STR_INIT_CAP 16

// want counts the NUL slot. Capacity doubles, so n appends cost O(n) copying.
static void str_reserve(Str *s, size_t want) {
    if (want <= s->cap) {
        return;
    }
    size_t cap = s->cap;
    while (cap < want) {
        cap *= 2;
    }
    s->data = nsh_realloc(s->data, cap);
    s->cap = cap;
}

void str_init(Str *s) {
    s->data = nsh_malloc(STR_INIT_CAP);
    s->data[0] = '\0';
    s->len = 0;
    s->cap = STR_INIT_CAP;
}

void str_free(Str *s) {
    nsh_free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void str_clear(Str *s) {
    s->len = 0;
    s->data[0] = '\0';
}

void str_push(Str *s, char c) {
    str_reserve(s, s->len + 2);
    s->data[s->len] = c;
    s->len++;
    s->data[s->len] = '\0';
}

void str_append(Str *s, const char *cstr) { str_append_n(s, cstr, strlen(cstr)); }

void str_append_n(Str *s, const char *p, size_t n) {
    if (n == 0) {
        return;
    }
    str_reserve(s, s->len + n + 1);
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = '\0';
}

char *str_take(Str *s) {
    char *out = s->data;
    str_init(s);
    return out;
}
