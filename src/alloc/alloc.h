// The only allocation API nullsh code is allowed to call. Phase 2 replaces the
// internals with a real free-list allocator; until then these are checked
// wrappers over libc malloc, and src/alloc is the only place libc malloc appears.

#pragma once

#include <stddef.h>

// These never return NULL. A failed allocation prints to stderr and aborts,
// so callers do not repeat the NULL check at every site.
// A request of size 0 returns a unique writable block of at least 1 byte, so
// the result is always safe to pass to nsh_free and always compares unequal
// to any other live allocation.
void *nsh_malloc(size_t size);
void *nsh_calloc(size_t count, size_t size);

// Grows or shrinks a block from nsh_malloc and friends. ptr may be NULL, in
// which case this behaves like nsh_malloc. A new_size of 0 does not free the
// block, it shrinks it to 1 byte.
void *nsh_realloc(void *ptr, size_t new_size);

// Safe on NULL.
void nsh_free(void *ptr);
