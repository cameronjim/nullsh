// Contract between alloc.c and the strategy implementations. Not for use outside src/alloc/.

#pragma once

#include <stddef.h>
#include <stdio.h>

#include "alloc.h"

// One mmap'd region managed by one strategy. state points at strategy-private
// bookkeeping carved from inside the region itself (strategies cannot call nsh_malloc).
typedef struct {
    unsigned char *base;
    size_t size;
    void *state;
} Arena;

#define ALLOC_ALIGN 16

// A strategy carves blocks out of an Arena. alloc returns NULL when the arena
// cannot satisfy the request (alloc.c decides what that means; strategies never abort).
// usable_size and free_block are only called with pointers alloc returned from this arena.
typedef struct {
    const char *name;
    void (*init)(Arena *a);
    void *(*alloc)(Arena *a, size_t size);
    void (*free_block)(Arena *a, void *p);
    size_t (*usable_size)(const Arena *a, const void *p);
    void (*stats)(const Arena *a, AllocStats *out);
    void (*dump)(const Arena *a, FILE *out);
} AllocStrategy;

extern const AllocStrategy ALLOC_FIRSTFIT;
extern const AllocStrategy ALLOC_BUDDY;
