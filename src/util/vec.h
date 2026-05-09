// Growable array of void pointers. Token lists and argv vectors are built on
// it. The vector owns its slot array, never the objects the slots point at,
// unless the caller asks for that with vec_free_deep.

#pragma once

#include <stddef.h>

typedef struct {
    void **items;
    size_t len;
    size_t cap;
} Vec;

// After vec_init the slot array is already allocated, so items is non-NULL and
// cap is VEC_INIT_CAP. Iterating v->items for v->len entries is always safe.
void vec_init(Vec *v);

// Frees the slot array only, not the pointed-to elements. Leaves the vector in
// the same state as a fresh zeroed Vec, so calling it twice is safe and a
// pushed-to-again vector reallocates on demand.
void vec_free(Vec *v);

// Calls free_item on every element, then vec_free. A NULL free_item, or a NULL
// element, is skipped rather than called.
void vec_free_deep(Vec *v, void (*free_item)(void *));

// Appends one slot, growing by doubling when full. item may be NULL.
void vec_push(Vec *v, void *item);

// Returns NULL if i is out of range. A NULL return is ambiguous with a stored
// NULL element, so check against v->len when that distinction matters.
void *vec_get(const Vec *v, size_t i);

// Removes and returns the last element, or NULL if the vector is empty.
void *vec_pop(Vec *v);
