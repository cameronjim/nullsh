// Growable array of void pointers. It owns the slot array, not the elements.

#pragma once

#include <stddef.h>

typedef struct {
    void **items;
    size_t len;
    size_t cap;
} Vec;

// items is non-NULL afterwards, so iterating v->len entries is always safe.
void vec_init(Vec *v);

// Frees the slot array only, not the elements. Safe to call twice.
void vec_free(Vec *v);

// A NULL free_item, or a NULL element, is skipped rather than called.
void vec_free_deep(Vec *v, void (*free_item)(void *));

// item may be NULL.
void vec_push(Vec *v, void *item);

// A NULL return is ambiguous with a stored NULL, so check i against v->len.
void *vec_get(const Vec *v, size_t i);

// NULL when the vector is empty.
void *vec_pop(Vec *v);
