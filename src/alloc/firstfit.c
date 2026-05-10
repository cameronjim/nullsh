// First-fit strategy: one implicit list of boundary-tagged blocks carved from an Arena.

#include "internal.h"

#include <stdint.h>
#include <stdio.h>

#include "alloc.h"

#define FF_TAG_USED ((size_t)0x46465553u)
#define FF_TAG_FREE ((size_t)0x46465246u)

typedef struct {
    size_t size; // whole block bytes: header + payload + footer
    size_t tag;  // FF_TAG_USED or FF_TAG_FREE, mirrored by the footer
} FfTag;

typedef struct {
    unsigned char *heap_start; // first block header, ALLOC_ALIGN aligned
    unsigned char *heap_end;   // one past the last block
} FfState;

_Static_assert(sizeof(FfTag) == ALLOC_ALIGN, "header must keep payloads aligned");

#define FF_OVERHEAD (2 * sizeof(FfTag))
#define FF_MIN_BLOCK (FF_OVERHEAD + ALLOC_ALIGN)

static size_t ff_round_up(size_t n) {
    return (n + (ALLOC_ALIGN - 1)) & ~(size_t)(ALLOC_ALIGN - 1);
}

static FfState *ff_state(const Arena *a) { return (FfState *)a->state; }

static void *ff_payload(FfTag *b) { return (unsigned char *)b + sizeof(FfTag); }

static FfTag *ff_block_of(void *p) {
    return (FfTag *)((unsigned char *)p - sizeof(FfTag));
}

static const FfTag *ff_block_of_const(const void *p) {
    return (const FfTag *)((const unsigned char *)p - sizeof(FfTag));
}

static void ff_mark(FfTag *b, size_t size, size_t tag) {
    FfTag *foot = (FfTag *)((unsigned char *)b + size - sizeof(FfTag));
    b->size = size;
    b->tag = tag;
    foot->size = size;
    foot->tag = tag;
}

static FfTag *ff_first(const FfState *s) {
    return s->heap_start < s->heap_end ? (FfTag *)s->heap_start : NULL;
}

static FfTag *ff_next(const FfState *s, FfTag *b) {
    unsigned char *n = (unsigned char *)b + b->size;
    return n < s->heap_end ? (FfTag *)n : NULL;
}

// Reachable only because every block mirrors its size in a footer.
static FfTag *ff_prev(const FfState *s, FfTag *b) {
    unsigned char *at = (unsigned char *)b;
    if (at <= s->heap_start) {
        return NULL;
    }
    const FfTag *foot = (const FfTag *)(at - sizeof(FfTag));
    return (FfTag *)(at - foot->size);
}

static void ff_init(Arena *a) {
    uintptr_t raw = (uintptr_t)a->base;
    size_t pad = (size_t)(ff_round_up((size_t)raw) - raw);
    if (a->size < pad) {
        pad = a->size;
    }
    unsigned char *state = a->base + pad;
    size_t left = a->size - pad;
    size_t reserved = ff_round_up(sizeof(FfState));
    a->state = state;
    FfState *s = (FfState *)state;
    s->heap_start = state + (left < reserved ? left : reserved);
    s->heap_end = s->heap_start;
    if (left < reserved) {
        return;
    }
    size_t span = (left - reserved) & ~(size_t)(ALLOC_ALIGN - 1);
    if (span < FF_MIN_BLOCK) {
        return;
    }
    s->heap_end = s->heap_start + span;
    ff_mark((FfTag *)s->heap_start, span, FF_TAG_FREE);
}

static void *ff_alloc(Arena *a, size_t size) {
    const FfState *s = ff_state(a);
    if (size > SIZE_MAX - FF_OVERHEAD - ALLOC_ALIGN) {
        return NULL;
    }
    size_t need = ff_round_up(size);
    if (need < ALLOC_ALIGN) {
        need = ALLOC_ALIGN;
    }
    size_t total = need + FF_OVERHEAD;
    for (FfTag *b = ff_first(s); b != NULL; b = ff_next(s, b)) {
        if (b->tag != FF_TAG_FREE || b->size < total) {
            continue;
        }
        size_t rest = b->size - total;
        if (rest >= FF_MIN_BLOCK) {
            ff_mark(b, total, FF_TAG_USED);
            ff_mark((FfTag *)((unsigned char *)b + total), rest, FF_TAG_FREE);
        } else {
            ff_mark(b, b->size, FF_TAG_USED);
        }
        return ff_payload(b);
    }
    return NULL;
}

static void ff_free_block(Arena *a, void *p) {
    if (p == NULL) {
        return;
    }
    const FfState *s = ff_state(a);
    FfTag *b = ff_block_of(p);
    size_t size = b->size;
    FfTag *next = ff_next(s, b);
    if (next != NULL && next->tag == FF_TAG_FREE) {
        size += next->size;
    }
    FfTag *prev = ff_prev(s, b);
    if (prev != NULL && prev->tag == FF_TAG_FREE) {
        size += prev->size;
        b = prev;
    }
    ff_mark(b, size, FF_TAG_FREE);
}

static size_t ff_usable_size(const Arena *a, const void *p) {
    (void)a;
    return ff_block_of_const(p)->size - FF_OVERHEAD;
}

// used_bytes and free_bytes count whole blocks, so they include per-block overhead.
static void ff_stats(const Arena *a, AllocStats *out) {
    const FfState *s = ff_state(a);
    out->arena_size = a->size;
    out->used_bytes = 0;
    out->free_bytes = 0;
    out->live_blocks = 0;
    out->free_blocks = 0;
    out->largest_free = 0;
    for (FfTag *b = ff_first(s); b != NULL; b = ff_next(s, b)) {
        if (b->tag == FF_TAG_FREE) {
            out->free_bytes += b->size;
            out->free_blocks++;
            if (b->size > out->largest_free) {
                out->largest_free = b->size;
            }
        } else {
            out->used_bytes += b->size;
            out->live_blocks++;
        }
    }
}

static void ff_dump(const Arena *a, FILE *out) {
    const FfState *s = ff_state(a);
    for (FfTag *b = ff_first(s); b != NULL; b = ff_next(s, b)) {
        fprintf(out, "%08zx %10zu %s\n",
                (size_t)((unsigned char *)b - a->base), b->size,
                b->tag == FF_TAG_FREE ? "FREE" : "USED");
    }
}

const AllocStrategy ALLOC_FIRSTFIT = {
    "firstfit", ff_init, ff_alloc, ff_free_block, ff_usable_size, ff_stats,
    ff_dump,
};
