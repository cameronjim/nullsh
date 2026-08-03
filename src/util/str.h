// Growable byte string, always a readable C string after str_init.

#pragma once

#include <stddef.h>

typedef struct {
    char *data; // always non-NULL after str_init, always NUL terminated
    size_t len; // bytes before the terminating NUL
    size_t cap; // allocated bytes, including the NUL slot
} Str;

// Every other call assumes this ran first.
void str_init(Str *s);

// Zeroes s, so calling it twice is safe.
void str_free(Str *s);

// Keeps the capacity for reuse.
void str_clear(Str *s);

void str_push(Str *s, char c);
void str_append(Str *s, const char *cstr);

// p need not be NUL terminated and may contain NUL bytes.
void str_append_n(Str *s, const char *p, size_t n);

// Hands the buffer to the caller, who nsh_frees it, and re-inits s.
char *str_take(Str *s);
