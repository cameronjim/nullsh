// Growable array of void pointers. Capacity starts at VEC_INIT_CAP and
// doubles when full. Every allocation goes through the nsh_* wrappers, which
// abort rather than return NULL, so nothing here checks for a failed alloc.

#include "vec.h"

#include "../alloc/alloc.h"

#define VEC_INIT_CAP 8

// Doubling cannot overflow: reaching a capacity where cap * 2 * sizeof(void *)
// wraps would require an already-live array larger than the address space.
static void vec_grow(Vec *v) {
    size_t new_cap = v->cap ? v->cap * 2 : VEC_INIT_CAP;
    v->items = nsh_realloc(v->items, new_cap * sizeof(*v->items));
    v->cap = new_cap;
}

void vec_init(Vec *v) {
    v->items = nsh_malloc(VEC_INIT_CAP * sizeof(*v->items));
    v->len = 0;
    v->cap = VEC_INIT_CAP;
}

void vec_free(Vec *v) {
    nsh_free(v->items);
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

void vec_free_deep(Vec *v, void (*free_item)(void *)) {
    if (free_item != NULL) {
        for (size_t i = 0; i < v->len; i++) {
            if (v->items[i] != NULL) {
                free_item(v->items[i]);
            }
        }
    }
    vec_free(v);
}

void vec_push(Vec *v, void *item) {
    if (v->len == v->cap) {
        vec_grow(v);
    }
    v->items[v->len] = item;
    v->len++;
}

void *vec_get(const Vec *v, size_t i) {
    if (i >= v->len) {
        return NULL;
    }
    return v->items[i];
}

void *vec_pop(Vec *v) {
    if (v->len == 0) {
        return NULL;
    }
    v->len--;
    return v->items[v->len];
}
