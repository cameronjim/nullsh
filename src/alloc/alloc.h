// The only allocation API nullsh code is allowed to call.

#pragma once

#include <stddef.h>
#include <stdio.h>

#include "../util/error.h"

// nsh_malloc and nsh_calloc never return NULL: a failed allocation aborts.
// Size 0 returns a unique writable block of at least 1 byte.
void *nsh_malloc(size_t size);
void *nsh_calloc(size_t count, size_t size);

// ptr may be NULL (acts like malloc); new_size 0 shrinks to 1 byte, never frees.
void *nsh_realloc(void *ptr, size_t new_size);

// Safe on NULL.
void nsh_free(void *ptr);

// Live heap numbers, filled by alloc_get_stats.
typedef struct {
    const char *strategy;
    size_t arena_size;
    size_t used_bytes;
    size_t free_bytes;
    size_t live_blocks;
    size_t free_blocks;
    size_t largest_free;
    unsigned long total_mallocs;
    unsigned long total_frees;
} AllocStats;

// NSH_ERR_INVALID for an unknown name. Valid names: "firstfit", "buddy".
NshError alloc_set_strategy(const char *name);

const char *alloc_strategy_name(void);

NshError alloc_get_stats(AllocStats *out);

// Address-ordered free/used map of the active strategy's arena.
void alloc_dump(FILE *out);
