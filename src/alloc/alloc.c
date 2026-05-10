// The nullsh heap: mmap'd arenas, strategy dispatch, guard canaries, and counters.

// glibc hides MAP_ANONYMOUS unless a feature test macro asks for it.
#define _DEFAULT_SOURCE

#include "alloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "internal.h"

#define ARENA_SIZE ((size_t)1u << 24)

#define SLOT_FIRSTFIT 0
#define SLOT_BUDDY 1
#define SLOT_COUNT 2

#define GUARD_PREFIX 16
#define GUARD_TRAILER 8
#define GUARD_TOTAL (GUARD_PREFIX + GUARD_TRAILER)

#define CANARY_HEAD ((uint64_t)0x4e554c4c53484421ull)
#define CANARY_TAIL ((uint64_t)0x21444853484f4c45ull)
#define CANARY_FREED ((uint64_t)0x46524545444f4d21ull)
#define POISON_BYTE 0xDD

// Sits immediately before the payload; 16 bytes so the payload stays 16-aligned.
typedef struct {
    size_t requested;
    uint64_t canary;
} Guard;

_Static_assert(sizeof(Guard) == GUARD_PREFIX, "prefix must keep payloads aligned");

typedef struct {
    Arena arena;
    int ready;
} Slot;

static const AllocStrategy *const STRATEGIES[SLOT_COUNT] = {&ALLOC_FIRSTFIT,
                                                            &ALLOC_BUDDY};

// Process-wide heap state: one arena per strategy, both live at once.
static struct {
    Slot slot[SLOT_COUNT];
    int active;
    int booted;
    unsigned long total_mallocs;
    unsigned long total_frees;
} g_heap;

static void die_out_of_memory(size_t size) {
    fprintf(stderr, "nullsh: out of memory requesting %zu bytes\n", size);
    abort();
}

static void die_corrupt(const char *side, const void *payload) {
    fprintf(stderr, "nullsh: heap corruption detected: %s canary at %p\n", side,
            payload);
    abort();
}

static void die_double_free(const void *payload) {
    fprintf(stderr,
            "nullsh: heap corruption detected: block already freed (double "
            "free) at %p\n",
            payload);
    abort();
}

static void die_unknown_pointer(const void *p) {
    fprintf(stderr, "nullsh: free of unknown pointer %p\n", p);
    abort();
}

// The boot strategy is read once; a later alloc_set_strategy wins over the env.
static void heap_boot(void) {
    if (g_heap.booted) {
        return;
    }
    g_heap.booted = 1;
    const char *want = getenv("NSH_ALLOC_STRATEGY");
    g_heap.active =
        (want != NULL && strcmp(want, "buddy") == 0) ? SLOT_BUDDY : SLOT_FIRSTFIT;
}

static Arena *slot_arena(int slot) {
    Slot *s = &g_heap.slot[slot];
    if (!s->ready) {
        void *mem = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
            fprintf(stderr, "nullsh: mmap of %zu heap bytes failed\n",
                    (size_t)ARENA_SIZE);
            abort();
        }
        s->arena.base = mem;
        s->arena.size = ARENA_SIZE;
        s->arena.state = NULL;
        STRATEGIES[slot]->init(&s->arena);
        s->ready = 1;
    }
    return &s->arena;
}

static int slot_of_pointer(const void *p) {
    uintptr_t at = (uintptr_t)p;
    for (int i = 0; i < SLOT_COUNT; i++) {
        const Slot *s = &g_heap.slot[i];
        if (!s->ready) {
            continue;
        }
        uintptr_t base = (uintptr_t)s->arena.base;
        if (at >= base && at < base + s->arena.size) {
            return i;
        }
    }
    return -1;
}

static Guard *guard_of(void *payload) {
    return (Guard *)(void *)((unsigned char *)payload - GUARD_PREFIX);
}

// The trailing canary is unaligned by construction, so it moves through memcpy.
static void guard_write(void *payload, size_t requested) {
    Guard *g = guard_of(payload);
    uint64_t tail = CANARY_TAIL;
    g->requested = requested;
    g->canary = CANARY_HEAD;
    memcpy((unsigned char *)payload + requested, &tail, sizeof tail);
}

static void guard_check(void *payload) {
    const Guard *g = guard_of(payload);
    uint64_t tail = 0;
    if (g->canary == CANARY_FREED) {
        die_double_free(payload);
    }
    if (g->canary != CANARY_HEAD) {
        die_corrupt("prefix (underflow)", payload);
    }
    memcpy(&tail, (unsigned char *)payload + g->requested, sizeof tail);
    if (tail != CANARY_TAIL) {
        die_corrupt("trailing (overflow)", payload);
    }
}

static void *block_new(int slot, size_t requested) {
    if (requested > SIZE_MAX - GUARD_TOTAL) {
        die_out_of_memory(requested);
    }
    Arena *a = slot_arena(slot);
    void *raw = STRATEGIES[slot]->alloc(a, requested + GUARD_TOTAL);
    if (raw == NULL) {
        die_out_of_memory(requested);
    }
    void *payload = (unsigned char *)raw + GUARD_PREFIX;
    guard_write(payload, requested);
    g_heap.total_mallocs++;
    return payload;
}

static void block_release(int slot, void *payload) {
    Guard *g = guard_of(payload);
    // Firstfit leaves the guard alone on free, so a second free lands on this marker.
    g->canary = CANARY_FREED;
    memset(payload, POISON_BYTE, g->requested);
    STRATEGIES[slot]->free_block(&g_heap.slot[slot].arena, g);
    g_heap.total_frees++;
}

void *nsh_malloc(size_t size) {
    heap_boot();
    return block_new(g_heap.active, size == 0 ? 1 : size);
}

void *nsh_calloc(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) {
        fprintf(stderr, "nullsh: out of memory requesting %zu x %zu bytes\n",
                count, size);
        abort();
    }
    size_t want = count * size;
    if (want == 0) {
        want = 1;
    }
    void *p = nsh_malloc(want);
    memset(p, 0, want);
    return p;
}

void nsh_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    int slot = slot_of_pointer(ptr);
    if (slot < 0) {
        die_unknown_pointer(ptr);
    }
    guard_check(ptr);
    block_release(slot, ptr);
}

void *nsh_realloc(void *ptr, size_t new_size) {
    size_t want = new_size == 0 ? 1 : new_size;
    if (ptr == NULL) {
        return nsh_malloc(want);
    }
    int slot = slot_of_pointer(ptr);
    if (slot < 0) {
        die_unknown_pointer(ptr);
    }
    guard_check(ptr);
    size_t old = guard_of(ptr)->requested;
    size_t capacity =
        STRATEGIES[slot]->usable_size(&g_heap.slot[slot].arena, guard_of(ptr)) -
        GUARD_TOTAL;
    if (capacity >= want) {
        guard_write(ptr, want);
        return ptr;
    }
    void *fresh = nsh_malloc(want);
    memcpy(fresh, ptr, old < want ? old : want);
    block_release(slot, ptr);
    return fresh;
}

NshError alloc_set_strategy(const char *name) {
    heap_boot();
    if (name == NULL) {
        return NSH_ERR_INVALID;
    }
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (strcmp(name, STRATEGIES[i]->name) == 0) {
            g_heap.active = i;
            return NSH_OK;
        }
    }
    return NSH_ERR_INVALID;
}

const char *alloc_strategy_name(void) {
    heap_boot();
    return STRATEGIES[g_heap.active]->name;
}

NshError alloc_get_stats(AllocStats *out) {
    if (out == NULL) {
        return NSH_ERR_INVALID;
    }
    heap_boot();
    const Slot *s = &g_heap.slot[g_heap.active];
    memset(out, 0, sizeof *out);
    if (s->ready) {
        STRATEGIES[g_heap.active]->stats(&s->arena, out);
    }
    out->strategy = STRATEGIES[g_heap.active]->name;
    out->total_mallocs = g_heap.total_mallocs;
    out->total_frees = g_heap.total_frees;
    return NSH_OK;
}

void alloc_dump(FILE *out) {
    if (out == NULL) {
        return;
    }
    heap_boot();
    const Slot *s = &g_heap.slot[g_heap.active];
    if (!s->ready) {
        fprintf(out, "heap not initialized\n");
        return;
    }
    STRATEGIES[g_heap.active]->dump(&s->arena, out);
}
