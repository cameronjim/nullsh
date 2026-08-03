// Growable byte string. After str_init the buffer is always a readable C
// string, so callers never have to test data for NULL, and embedded NUL bytes
// are preserved in len even though they truncate the C-string view.

#pragma once

#include <stddef.h>

typedef struct {
    char *data; // always non-NULL after str_init, always NUL terminated
    size_t len; // bytes before the terminating NUL
    size_t cap; // allocated bytes, including the NUL slot
} Str;

// Leaves s holding the empty string. Every other call assumes this ran first.
void str_init(Str *s);

// Releases the buffer and zeroes s, so calling it twice is safe.
void str_free(Str *s);

// Truncates to length 0 and keeps the capacity for reuse.
void str_clear(Str *s);

void str_push(Str *s, char c);
void str_append(Str *s, const char *cstr);

// p need not be NUL terminated and may contain NUL bytes.
void str_append_n(Str *s, const char *p, size_t n);

// Hands the buffer to the caller, who frees it with nsh_free, and re-inits s
// to a fresh empty string.
char *str_take(Str *s);
