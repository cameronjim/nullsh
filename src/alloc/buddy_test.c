// Unit tests for the buddy strategy: rounding, splitting, XOR merging, and recovery.

#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../tests/harness.h"

#define ARENA1_SIZE ((size_t)1 << 20)
#define ARENA2_SIZE ((size_t)1 << 18)
#define HDR 16

static _Alignas(16) unsigned char mem1[ARENA1_SIZE];
static _Alignas(16) unsigned char mem2[ARENA2_SIZE];

#define ASSERT_STATS_EQ(x, y)                                                 \
    do {                                                                      \
        ASSERT_EQ((x).used_bytes, (y).used_bytes);                            \
        ASSERT_EQ((x).free_bytes, (y).free_bytes);                            \
        ASSERT_EQ((x).live_blocks, (y).live_blocks);                          \
        ASSERT_EQ((x).free_blocks, (y).free_blocks);                          \
        ASSERT_EQ((x).largest_free, (y).largest_free);                        \
        ASSERT_EQ((x).arena_size, (y).arena_size);                            \
    } while (0)

static void arena_make(Arena *a, unsigned char *mem, size_t size) {
    a->base = mem;
    a->size = size;
    a->state = NULL;
    ALLOC_BUDDY.init(a);
}

static AllocStats stats_of(const Arena *a) {
    AllocStats st;
    memset(&st, 0, sizeof(st));
    ALLOC_BUDDY.stats(a, &st);
    return st;
}

static size_t block_off(const Arena *a, const void *p) {
    return (size_t)((const unsigned char *)p - a->base) - HDR;
}

static size_t block_size(const Arena *a, const void *p) {
    return ALLOC_BUDDY.usable_size(a, p) + HDR;
}

static unsigned log2_of(size_t x) {
    unsigned k = 0;
    while (((size_t)1 << k) < x) {
        k++;
    }
    return k;
}

TEST(strategy_is_named_buddy) {
    ASSERT_STR_EQ(ALLOC_BUDDY.name, "buddy");
}

TEST(init_seeds_one_free_block_per_order) {
    Arena a;
    arena_make(&a, mem1, ARENA1_SIZE);
    AllocStats st = stats_of(&a);
    size_t state_bytes = st.arena_size - st.free_bytes;
    unsigned s = log2_of(state_bytes);
    ASSERT_EQ(st.arena_size, ARENA1_SIZE);
    ASSERT_EQ((size_t)1 << s, state_bytes);
    ASSERT_EQ(st.used_bytes, 0);
    ASSERT_EQ(st.live_blocks, 0);
    ASSERT_EQ(st.largest_free, ARENA1_SIZE / 2);
    ASSERT_EQ(st.free_blocks, log2_of(ARENA1_SIZE) - s);
}

TEST(requests_round_up_to_power_of_two_classes) {
    Arena a;
    arena_make(&a, mem1, ARENA1_SIZE);
    void *p1 = ALLOC_BUDDY.alloc(&a, 1);
    void *p17 = ALLOC_BUDDY.alloc(&a, 17);
    void *p33 = ALLOC_BUDDY.alloc(&a, 33);
    void *p100 = ALLOC_BUDDY.alloc(&a, 100);
    void *p4000 = ALLOC_BUDDY.alloc(&a, 4000);
    ASSERT_TRUE(p1 && p17 && p33 && p100 && p4000);
    ASSERT_EQ(ALLOC_BUDDY.usable_size(&a, p1), 32 - HDR);
    ASSERT_EQ(ALLOC_BUDDY.usable_size(&a, p17), 64 - HDR);
    ASSERT_EQ(ALLOC_BUDDY.usable_size(&a, p33), 64 - HDR);
    ASSERT_EQ(ALLOC_BUDDY.usable_size(&a, p100), 128 - HDR);
    ASSERT_EQ(ALLOC_BUDDY.usable_size(&a, p4000), 4096 - HDR);
    ASSERT_EQ(((uintptr_t)p1) % ALLOC_ALIGN, 0);
    ASSERT_EQ(((uintptr_t)p17) % ALLOC_ALIGN, 0);
    ASSERT_EQ(((uintptr_t)p4000) % ALLOC_ALIGN, 0);
}

TEST(smallest_request_splits_the_cascade_down_to_min_order) {
    Arena a;
    arena_make(&a, mem1, ARENA1_SIZE);
    AllocStats base = stats_of(&a);
    unsigned s = log2_of(base.arena_size - base.free_bytes);
    void *p = ALLOC_BUDDY.alloc(&a, 16);
    ASSERT_TRUE(p != NULL);
    AllocStats st = stats_of(&a);
    ASSERT_EQ(block_size(&a, p), 32);
    ASSERT_EQ(st.used_bytes, 32);
    ASSERT_EQ(st.live_blocks, 1);
    ASSERT_EQ(st.free_bytes, base.free_bytes - 32);
    ASSERT_EQ(st.free_blocks, base.free_blocks - 1 + (s - 5));
    ASSERT_EQ(st.largest_free, base.largest_free);
    ALLOC_BUDDY.free_block(&a, p);
    st = stats_of(&a);
    ASSERT_STATS_EQ(st, base);
}

TEST(freeing_two_buddies_merges_all_the_way_back) {
    Arena a;
    arena_make(&a, mem1, ARENA1_SIZE);
    AllocStats base = stats_of(&a);
    void *x = ALLOC_BUDDY.alloc(&a, 16);
    void *y = ALLOC_BUDDY.alloc(&a, 16);
    ASSERT_TRUE(x && y);
    ASSERT_EQ(block_off(&a, x) ^ 32, block_off(&a, y));
    ALLOC_BUDDY.free_block(&a, x);
    ALLOC_BUDDY.free_block(&a, y);
    AllocStats st = stats_of(&a);
    ASSERT_STATS_EQ(st, base);
}

TEST(merge_needs_a_free_buddy_of_the_same_order) {
    Arena a;
    arena_make(&a, mem1, ARENA1_SIZE);
    AllocStats base = stats_of(&a);
    void *x = ALLOC_BUDDY.alloc(&a, 16);
    void *y = ALLOC_BUDDY.alloc(&a, 16);
    void *z = ALLOC_BUDDY.alloc(&a, 48);
    ASSERT_TRUE(x && y && z);
    ASSERT_EQ(block_size(&a, z), 64);
    ASSERT_EQ(block_off(&a, z), block_off(&a, x) + 64);
    AllocStats held = stats_of(&a);
    ALLOC_BUDDY.free_block(&a, x);
    AllocStats st = stats_of(&a);
    ASSERT_EQ(st.free_bytes, held.free_bytes + 32);
    ASSERT_EQ(st.free_blocks, held.free_blocks + 1);
    ALLOC_BUDDY.free_block(&a, y);
    st = stats_of(&a);
    ASSERT_EQ(st.free_bytes, held.free_bytes + 64);
    ASSERT_EQ(st.free_blocks, held.free_blocks + 1);
    ASSERT_EQ(st.used_bytes, 64);
    ALLOC_BUDDY.free_block(&a, z);
    st = stats_of(&a);
    ASSERT_STATS_EQ(st, base);
}

TEST(buddy_offsets_are_symmetric_under_xor) {
    Arena a;
    arena_make(&a, mem1, ARENA1_SIZE);
    static const size_t sizes[] = {1, 30, 100, 1000, 9000, 70000};
    void *held[6];
    for (int i = 0; i < 6; i++) {
        held[i] = ALLOC_BUDDY.alloc(&a, sizes[i]);
        ASSERT_TRUE(held[i] != NULL);
        size_t off = block_off(&a, held[i]);
        size_t sz = block_size(&a, held[i]);
        size_t buddy = off ^ sz;
        ASSERT_EQ(off % sz, 0);
        ASSERT_EQ(buddy ^ sz, off);
        ASSERT_TRUE(buddy + sz <= a.size);
        ASSERT_EQ((off < buddy ? off : buddy) % (sz * 2), 0);
    }
    for (int i = 0; i < 6; i++) {
        ALLOC_BUDDY.free_block(&a, held[i]);
    }
}

static void *exhaust_slots[16384];

TEST(exhaustion_then_random_order_free_returns_to_baseline) {
    Arena a;
    arena_make(&a, mem1, ARENA1_SIZE);
    AllocStats base = stats_of(&a);
    size_t n = 0;
    for (;;) {
        void *p = ALLOC_BUDDY.alloc(&a, 100);
        if (p == NULL) {
            break;
        }
        ASSERT_TRUE(n < 16384);
        memset(p, (int)(n & 0xFF), 100);
        exhaust_slots[n++] = p;
    }
    AllocStats full = stats_of(&a);
    ASSERT_EQ(full.free_bytes, 0);
    ASSERT_EQ(full.free_blocks, 0);
    ASSERT_EQ(full.largest_free, 0);
    ASSERT_EQ(full.live_blocks, n);
    ASSERT_EQ(full.used_bytes, n * 128);
    srand(9001);
    for (size_t i = n; i > 1; i--) {
        size_t j = (size_t)rand() % i;
        void *tmp = exhaust_slots[i - 1];
        exhaust_slots[i - 1] = exhaust_slots[j];
        exhaust_slots[j] = tmp;
    }
    for (size_t i = 0; i < n; i++) {
        ALLOC_BUDDY.free_block(&a, exhaust_slots[i]);
    }
    AllocStats st = stats_of(&a);
    ASSERT_STATS_EQ(st, base);
}

TEST(internal_fragmentation_is_reported_honestly) {
    Arena a;
    arena_make(&a, mem1, ARENA1_SIZE);
    void *p = ALLOC_BUDDY.alloc(&a, 5000);
    ASSERT_TRUE(p != NULL);
    AllocStats st = stats_of(&a);
    ASSERT_EQ(block_size(&a, p), 8192);
    ASSERT_EQ(ALLOC_BUDDY.usable_size(&a, p), 8192 - HDR);
    ASSERT_EQ(st.used_bytes, 8192);
    ASSERT_EQ(st.live_blocks, 1);
    ALLOC_BUDDY.free_block(&a, p);
}

TEST(oversized_requests_return_null) {
    Arena a;
    arena_make(&a, mem2, ARENA2_SIZE);
    ASSERT_TRUE(ALLOC_BUDDY.alloc(&a, ARENA2_SIZE) == NULL);
    ASSERT_TRUE(ALLOC_BUDDY.alloc(&a, ARENA2_SIZE * 4) == NULL);
    ASSERT_TRUE(ALLOC_BUDDY.alloc(&a, ARENA2_SIZE / 2) == NULL);
    void *p = ALLOC_BUDDY.alloc(&a, ARENA2_SIZE / 4);
    ASSERT_TRUE(p != NULL);
    ALLOC_BUDDY.free_block(&a, p);
}

TEST(two_arenas_do_not_contaminate_each_other) {
    Arena a;
    Arena b;
    arena_make(&a, mem1, ARENA1_SIZE);
    arena_make(&b, mem2, ARENA2_SIZE);
    AllocStats base_a = stats_of(&a);
    AllocStats base_b = stats_of(&b);
    void *pa[32];
    void *pb[32];
    for (int i = 0; i < 32; i++) {
        pa[i] = ALLOC_BUDDY.alloc(&a, 200 + (size_t)i);
        pb[i] = ALLOC_BUDDY.alloc(&b, 300 + (size_t)i);
        ASSERT_TRUE(pa[i] && pb[i]);
        memset(pa[i], 0xA1, 200 + (size_t)i);
        memset(pb[i], 0xB2, 300 + (size_t)i);
    }
    for (int i = 0; i < 32; i++) {
        ASSERT_TRUE((unsigned char *)pa[i] >= mem1 && (unsigned char *)pa[i] < mem1 + ARENA1_SIZE);
        ASSERT_TRUE((unsigned char *)pb[i] >= mem2 && (unsigned char *)pb[i] < mem2 + ARENA2_SIZE);
        ASSERT_EQ(((unsigned char *)pa[i])[199], 0xA1);
        ASSERT_EQ(((unsigned char *)pb[i])[299], 0xB2);
    }
    ASSERT_EQ(stats_of(&a).live_blocks, 32);
    ASSERT_EQ(stats_of(&b).live_blocks, 32);
    for (int i = 31; i >= 0; i--) {
        ALLOC_BUDDY.free_block(&a, pa[i]);
    }
    for (int i = 0; i < 32; i++) {
        ALLOC_BUDDY.free_block(&b, pb[i]);
    }
    AllocStats end_a = stats_of(&a);
    AllocStats end_b = stats_of(&b);
    ASSERT_STATS_EQ(end_a, base_a);
    ASSERT_STATS_EQ(end_b, base_b);
}

TEST(dump_walks_every_block_in_address_order) {
    Arena a;
    arena_make(&a, mem1, ARENA1_SIZE);
    void *p1 = ALLOC_BUDDY.alloc(&a, 40);
    void *p2 = ALLOC_BUDDY.alloc(&a, 5000);
    ASSERT_TRUE(p1 && p2);
    FILE *f = tmpfile();
    ASSERT_TRUE(f != NULL);
    ALLOC_BUDDY.dump(&a, f);
    rewind(f);
    char line[128];
    ASSERT_TRUE(fgets(line, sizeof(line), f) != NULL);
    size_t prev_end = 0;
    size_t total = 0;
    size_t used = 0;
    int lines = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        size_t off = 0;
        size_t sz = 0;
        char kind[8];
        ASSERT_EQ(sscanf(line, "%zu %zu %7s", &off, &sz, kind), 3);
        ASSERT_TRUE(sz >= 32 && (sz & (sz - 1)) == 0);
        ASSERT_EQ(off % sz, 0);
        if (lines > 0) {
            ASSERT_EQ(off, prev_end);
        }
        if (strcmp(kind, "USED") == 0) {
            used += sz;
        } else {
            ASSERT_STR_EQ(kind, "FREE");
        }
        prev_end = off + sz;
        total += sz;
        lines++;
    }
    fclose(f);
    AllocStats st = stats_of(&a);
    ASSERT_EQ(used, st.used_bytes);
    ASSERT_EQ(total, st.used_bytes + st.free_bytes);
    ASSERT_EQ(prev_end, ARENA1_SIZE);
    ALLOC_BUDDY.free_block(&a, p1);
    ALLOC_BUDDY.free_block(&a, p2);
}

typedef struct {
    unsigned char *p;
    size_t size;
    unsigned char fill;
} Shadow;

static Shadow shadow[512];

TEST(randomized_soak_keeps_every_pattern_intact) {
    Arena a;
    arena_make(&a, mem1, ARENA1_SIZE);
    AllocStats base = stats_of(&a);
    memset(shadow, 0, sizeof(shadow));
    srand(54321);
    for (int op = 0; op < 5000; op++) {
        int slot = rand() % 512;
        if (shadow[slot].p != NULL) {
            for (size_t i = 0; i < shadow[slot].size; i++) {
                ASSERT_EQ(shadow[slot].p[i], shadow[slot].fill);
            }
            ALLOC_BUDDY.free_block(&a, shadow[slot].p);
            shadow[slot].p = NULL;
            continue;
        }
        size_t size = (size_t)(rand() % 4000) + 1;
        unsigned char *p = ALLOC_BUDDY.alloc(&a, size);
        if (p == NULL) {
            continue;
        }
        ASSERT_TRUE(ALLOC_BUDDY.usable_size(&a, p) >= size);
        ASSERT_EQ(((uintptr_t)p) % ALLOC_ALIGN, 0);
        shadow[slot].p = p;
        shadow[slot].size = size;
        shadow[slot].fill = (unsigned char)(op & 0xFF);
        memset(p, shadow[slot].fill, size);
    }
    for (int slot = 0; slot < 512; slot++) {
        if (shadow[slot].p == NULL) {
            continue;
        }
        for (size_t i = 0; i < shadow[slot].size; i++) {
            ASSERT_EQ(shadow[slot].p[i], shadow[slot].fill);
        }
        ALLOC_BUDDY.free_block(&a, shadow[slot].p);
        shadow[slot].p = NULL;
    }
    AllocStats st = stats_of(&a);
    ASSERT_STATS_EQ(st, base);
}

TEST_MAIN()
