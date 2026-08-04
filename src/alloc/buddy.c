// Buddy-system strategy: power-of-2 blocks, 16-byte headers, XOR buddy merging.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

#define BUDDY_MIN_ORDER 5
#define BUDDY_MIN_BLOCK ((size_t)1 << BUDDY_MIN_ORDER)
#define BUDDY_MAX_ORDER 47
#define BUDDY_ORDERS (BUDDY_MAX_ORDER + 1)
#define BUDDY_MAGIC ((uint64_t)0x42554459424c4b31)

typedef struct BuddyHeader {
    uint64_t magic;
    uint32_t order;
    uint32_t in_use;
} BuddyHeader;

// Free blocks thread their list links through their own payload bytes.
typedef struct BuddyNode {
    struct BuddyNode *next;
    struct BuddyNode *prev;
} BuddyNode;

typedef struct BuddyState {
    size_t region_off;
    unsigned max_order;
    size_t used_bytes;
    size_t free_bytes;
    size_t live_blocks;
    size_t free_blocks;
    BuddyNode *lists[BUDDY_ORDERS];
} BuddyState;

_Static_assert(sizeof(BuddyHeader) == ALLOC_ALIGN, "header must be one alignment unit");
_Static_assert(sizeof(BuddyHeader) + sizeof(BuddyNode) <= BUDDY_MIN_BLOCK,
               "min block must hold header plus free links");

static int is_pow2(size_t x) { return x != 0 && (x & (x - 1)) == 0; }

static unsigned order_of(size_t n) {
    unsigned k = BUDDY_MIN_ORDER;
    while (((size_t)1 << k) < n) {
        k++;
    }
    return k;
}

static BuddyHeader *header_at(const Arena *a, size_t off) {
    return (BuddyHeader *)(void *)(a->base + off);
}

static BuddyNode *node_at(const Arena *a, size_t off) {
    return (BuddyNode *)(void *)(a->base + off + sizeof(BuddyHeader));
}

static size_t node_off(const Arena *a, const BuddyNode *n) {
    return (size_t)((const unsigned char *)n - a->base) - sizeof(BuddyHeader);
}

static void list_push(const Arena *a, BuddyState *s, size_t off, unsigned order) {
    BuddyHeader *h = header_at(a, off);
    h->magic = BUDDY_MAGIC;
    h->order = order;
    h->in_use = 0;
    BuddyNode *n = node_at(a, off);
    n->prev = NULL;
    n->next = s->lists[order];
    if (n->next != NULL) {
        n->next->prev = n;
    }
    s->lists[order] = n;
    s->free_bytes += (size_t)1 << order;
    s->free_blocks++;
}

static void list_remove(const Arena *a, BuddyState *s, size_t off, unsigned order) {
    BuddyNode *n = node_at(a, off);
    if (n->prev != NULL) {
        n->prev->next = n->next;
    } else {
        s->lists[order] = n->next;
    }
    if (n->next != NULL) {
        n->next->prev = n->prev;
    }
    s->free_bytes -= (size_t)1 << order;
    s->free_blocks--;
}

static void buddy_init(Arena *a) {
    assert(is_pow2(a->size));
    assert(a->size <= ((size_t)1 << BUDDY_MAX_ORDER));
    size_t state_bytes = BUDDY_MIN_BLOCK;
    while (state_bytes < sizeof(BuddyState)) {
        state_bytes <<= 1;
    }
    assert(a->size >= state_bytes * 2);
    a->state = a->base;
    BuddyState *s = (BuddyState *)(void *)a->base;
    memset(s, 0, sizeof(*s));
    s->region_off = state_bytes;
    // The state takes the arena's leading power-of-2 bytes, so the remainder seeds exactly one aligned block per order.
    for (size_t off = state_bytes; off < a->size;) {
        size_t block = off & (~off + 1);
        unsigned k = order_of(block);
        list_push(a, s, off, k);
        if (k > s->max_order) {
            s->max_order = k;
        }
        off += block;
    }
}

static void *buddy_alloc(Arena *a, size_t size) {
    BuddyState *s = a->state;
    if (size > a->size) {
        return NULL;
    }
    unsigned want = order_of(size + sizeof(BuddyHeader));
    if (want > s->max_order) {
        return NULL;
    }
    unsigned k = want;
    while (k <= s->max_order && s->lists[k] == NULL) {
        k++;
    }
    if (k > s->max_order) {
        return NULL;
    }
    size_t off = node_off(a, s->lists[k]);
    list_remove(a, s, off, k);
    while (k > want) {
        k--;
        list_push(a, s, off + ((size_t)1 << k), k);
    }
    BuddyHeader *h = header_at(a, off);
    h->magic = BUDDY_MAGIC;
    h->order = want;
    h->in_use = 1;
    s->used_bytes += (size_t)1 << want;
    s->live_blocks++;
    return a->base + off + sizeof(BuddyHeader);
}

static void buddy_free_block(Arena *a, void *p) {
    if (p == NULL) {
        return;
    }
    BuddyState *s = a->state;
    size_t off = (size_t)((unsigned char *)p - a->base) - sizeof(BuddyHeader);
    BuddyHeader *h = header_at(a, off);
    assert(h->magic == BUDDY_MAGIC && h->in_use);
    unsigned k = h->order;
    h->in_use = 0;
    s->used_bytes -= (size_t)1 << k;
    s->live_blocks--;
    while (k < s->max_order) {
        // Buddies are the two halves of one split, so their arena-relative offsets differ in exactly the size bit.
        size_t buddy = off ^ ((size_t)1 << k);
        if (buddy < s->region_off) {
            break;
        }
        const BuddyHeader *bh = header_at(a, buddy);
        if (bh->magic != BUDDY_MAGIC || bh->in_use || bh->order != k) {
            break;
        }
        list_remove(a, s, buddy, k);
        if (buddy < off) {
            off = buddy;
        }
        k++;
    }
    list_push(a, s, off, k);
}

static size_t buddy_usable_size(const Arena *a, const void *p) {
    (void)a;
    const BuddyHeader *h =
        (const BuddyHeader *)(const void *)((const unsigned char *)p - sizeof(BuddyHeader));
    return ((size_t)1 << h->order) - sizeof(BuddyHeader);
}

static void buddy_stats(const Arena *a, AllocStats *out) {
    const BuddyState *s = a->state;
    out->arena_size = a->size;
    out->used_bytes = s->used_bytes;
    out->free_bytes = s->free_bytes;
    out->live_blocks = s->live_blocks;
    out->free_blocks = s->free_blocks;
    out->largest_free = 0;
    for (unsigned k = s->max_order + 1; k-- > BUDDY_MIN_ORDER;) {
        if (s->lists[k] != NULL) {
            out->largest_free = (size_t)1 << k;
            break;
        }
    }
}

static void buddy_dump(const Arena *a, FILE *out) {
    const BuddyState *s = a->state;
    fprintf(out, "buddy: arena %zu, state %zu, orders %d..%u\n", a->size, s->region_off,
            BUDDY_MIN_ORDER, s->max_order);
    for (size_t off = s->region_off; off < a->size;) {
        const BuddyHeader *h = header_at(a, off);
        size_t block = (size_t)1 << h->order;
        fprintf(out, "  %10zu %10zu %s\n", off, block, h->in_use ? "USED" : "FREE");
        off += block;
    }
}

const AllocStrategy ALLOC_BUDDY = {
    .name = "buddy",
    .init = buddy_init,
    .alloc = buddy_alloc,
    .free_block = buddy_free_block,
    .usable_size = buddy_usable_size,
    .stats = buddy_stats,
    .dump = buddy_dump,
};
