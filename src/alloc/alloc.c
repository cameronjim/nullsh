// Checked wrappers over libc malloc. This file is the single exception to the
// rule that nullsh never calls libc allocation directly. Phase 2 swaps the
// bodies for a real allocator without touching a single call site.

#include "alloc.h"

#include <stdio.h>
#include <stdlib.h>

// A zero-size request still hands back a distinct writable block, which keeps
// callers from having to special case empty strings and empty arrays.
static size_t at_least_one(size_t size) { return size == 0 ? 1 : size; }

static void die_out_of_memory(size_t size) {
    fprintf(stderr, "nullsh: out of memory requesting %zu bytes\n", size);
    abort();
}

void *nsh_malloc(size_t size) {
    size_t want = at_least_one(size);
    void *p = malloc(want);
    if (p == NULL) {
        die_out_of_memory(want);
    }
    return p;
}

void *nsh_calloc(size_t count, size_t size) {
    size_t n = count == 0 ? 1 : count;
    size_t each = at_least_one(size);
    // calloc does the overflow check itself and returns NULL on wraparound,
    // which lands in the same abort path below.
    void *p = calloc(n, each);
    if (p == NULL) {
        die_out_of_memory(n * each);
    }
    return p;
}

void *nsh_realloc(void *ptr, size_t new_size) {
    size_t want = at_least_one(new_size);
    void *p = realloc(ptr, want);
    if (p == NULL) {
        die_out_of_memory(want);
    }
    return p;
}

void nsh_free(void *ptr) { free(ptr); }
