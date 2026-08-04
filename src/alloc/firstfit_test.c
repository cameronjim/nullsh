// Unit tests for the first-fit strategy, driven directly through the vtable.

#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../tests/harness.h"

static _Alignas(16) unsigned char g_big[1u << 20];
static _Alignas(16) unsigned char g_small[400];
static _Alignas(16) unsigned char g_mid[8192];
static _Alignas(16) unsigned char g_other[4096];

static Arena arena_of(unsigned char *base, size_t size) {
    Arena a;
    a.base = base;
    a.size = size;
    a.state = NULL;
    ALLOC_FIRSTFIT.init(&a);
    return a;
}

static AllocStats stat_of(const Arena *a) {
    AllocStats s;
    memset(&s, 0, sizeof s);
    ALLOC_FIRSTFIT.stats(a, &s);
    return s;
}

static void *ff_get(Arena *a, size_t n) { return ALLOC_FIRSTFIT.alloc(a, n); }

static void ff_put(Arena *a, void *p) { ALLOC_FIRSTFIT.free_block(a, p); }

static size_t ff_usable(const Arena *a, const void *p) {
    return ALLOC_FIRSTFIT.usable_size(a, p);
}

static int at_baseline(const Arena *a, const AllocStats *base) {
    AllocStats s = stat_of(a);
    return s.free_blocks == 1 && s.live_blocks == 0 && s.used_bytes == 0 &&
           s.free_bytes == base->free_bytes &&
           s.largest_free == base->largest_free;
}

static int filled_with(const unsigned char *p, size_t n, unsigned char v) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] != v) {
            return 0;
        }
    }
    return 1;
}

TEST(init_gives_one_free_block) {
    Arena a = arena_of(g_big, sizeof g_big);
    AllocStats s = stat_of(&a);
    ASSERT_EQ(s.arena_size, sizeof g_big);
    ASSERT_EQ(s.free_blocks, 1);
    ASSERT_EQ(s.live_blocks, 0);
    ASSERT_EQ(s.used_bytes, 0);
    ASSERT_EQ(s.free_bytes, s.largest_free);
    ASSERT_TRUE(s.largest_free > sizeof g_big - 256);
}

TEST(alloc_then_free_restores_the_arena) {
    Arena a = arena_of(g_big, sizeof g_big);
    AllocStats base = stat_of(&a);
    void *p = ff_get(&a, 100);
    ASSERT_TRUE(p != NULL);
    AllocStats mid = stat_of(&a);
    ASSERT_EQ(mid.live_blocks, 1);
    ASSERT_EQ(mid.free_blocks, 1);
    ASSERT_TRUE(mid.largest_free < base.largest_free);
    ASSERT_EQ(mid.used_bytes + mid.free_bytes, base.free_bytes);
    ff_put(&a, p);
    ASSERT_TRUE(at_baseline(&a, &base));
}

TEST(split_yields_two_disjoint_blocks) {
    Arena a = arena_of(g_big, sizeof g_big);
    AllocStats base = stat_of(&a);
    unsigned char *p = ff_get(&a, 64);
    unsigned char *q = ff_get(&a, 200);
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(q != NULL);
    ASSERT_TRUE(p != q);
    ASSERT_EQ((uintptr_t)p % ALLOC_ALIGN, 0);
    ASSERT_EQ((uintptr_t)q % ALLOC_ALIGN, 0);
    size_t np = ff_usable(&a, p);
    size_t nq = ff_usable(&a, q);
    ASSERT_TRUE(np >= 64);
    ASSERT_TRUE(nq >= 200);
    memset(p, 0xAA, np);
    memset(q, 0x55, nq);
    ASSERT_TRUE(filled_with(p, np, 0xAA));
    ASSERT_TRUE(filled_with(q, nq, 0x55));
    ASSERT_EQ(stat_of(&a).live_blocks, 2);
    ff_put(&a, p);
    ff_put(&a, q);
    ASSERT_TRUE(at_baseline(&a, &base));
}

// The small arena holds exactly four 64-byte blocks, so merges show up in largest_free.
TEST(coalesce_backward_then_forward) {
    Arena a = arena_of(g_small, sizeof g_small);
    AllocStats base = stat_of(&a);
    void *b[4];
    for (size_t i = 0; i < 4; i++) {
        b[i] = ff_get(&a, 64);
        ASSERT_TRUE(b[i] != NULL);
    }
    AllocStats full = stat_of(&a);
    ASSERT_EQ(full.free_blocks, 0);
    ASSERT_EQ(full.free_bytes, 0);
    ASSERT_EQ(full.live_blocks, 4);
    ASSERT_EQ(full.used_bytes, base.free_bytes);
    ASSERT_TRUE(ff_get(&a, 64) == NULL);
    size_t blk = base.largest_free / 4;
    ASSERT_EQ(blk * 4, base.largest_free);

    ff_put(&a, b[1]);
    AllocStats s = stat_of(&a);
    ASSERT_EQ(s.free_blocks, 1);
    ASSERT_EQ(s.largest_free, blk);

    ff_put(&a, b[2]);
    s = stat_of(&a);
    ASSERT_EQ(s.free_blocks, 1);
    ASSERT_EQ(s.largest_free, 2 * blk);

    ff_put(&a, b[0]);
    s = stat_of(&a);
    ASSERT_EQ(s.free_blocks, 1);
    ASSERT_EQ(s.largest_free, 3 * blk);

    ff_put(&a, b[3]);
    ASSERT_TRUE(at_baseline(&a, &base));
}

TEST(coalesce_both_neighbours_at_once) {
    Arena a = arena_of(g_small, sizeof g_small);
    AllocStats base = stat_of(&a);
    void *b[4];
    for (size_t i = 0; i < 4; i++) {
        b[i] = ff_get(&a, 64);
        ASSERT_TRUE(b[i] != NULL);
    }
    size_t blk = base.largest_free / 4;

    ff_put(&a, b[0]);
    AllocStats s = stat_of(&a);
    ASSERT_EQ(s.free_blocks, 1);
    ASSERT_EQ(s.largest_free, blk);

    ff_put(&a, b[2]);
    s = stat_of(&a);
    ASSERT_EQ(s.free_blocks, 2);
    ASSERT_EQ(s.largest_free, blk);

    ff_put(&a, b[1]);
    s = stat_of(&a);
    ASSERT_EQ(s.free_blocks, 1);
    ASSERT_EQ(s.largest_free, 3 * blk);

    ff_put(&a, b[3]);
    ASSERT_TRUE(at_baseline(&a, &base));
}

TEST(first_fit_prefers_the_earliest_hole) {
    Arena a = arena_of(g_big, sizeof g_big);
    AllocStats base = stat_of(&a);
    void *small = ff_get(&a, 64);
    void *keep = ff_get(&a, 64);
    void *large = ff_get(&a, 4096);
    void *tail = ff_get(&a, 64);
    ASSERT_TRUE(small != NULL && keep != NULL && large != NULL && tail != NULL);
    ff_put(&a, small);
    ff_put(&a, large);
    ASSERT_EQ(stat_of(&a).free_blocks, 3);

    void *again = ff_get(&a, 64);
    ASSERT_TRUE(again == small);
    void *big = ff_get(&a, 4096);
    ASSERT_TRUE(big == large);

    ff_put(&a, again);
    ff_put(&a, big);
    ff_put(&a, keep);
    ff_put(&a, tail);
    ASSERT_TRUE(at_baseline(&a, &base));
}

TEST(exhaustion_then_mixed_free_returns_to_baseline) {
    Arena a = arena_of(g_mid, sizeof g_mid);
    AllocStats base = stat_of(&a);
    unsigned char *ptrs[256];
    size_t n = 0;
    while (n < 256) {
        unsigned char *p = ff_get(&a, 100);
        if (p == NULL) {
            break;
        }
        memset(p, (unsigned char)(n & 0xFF), 100);
        ptrs[n++] = p;
    }
    ASSERT_TRUE(n > 8);
    ASSERT_TRUE(ff_get(&a, 100) == NULL);
    ASSERT_EQ(stat_of(&a).live_blocks, n);

    for (size_t i = 1; i < n; i += 2) {
        ASSERT_TRUE(filled_with(ptrs[i], 100, (unsigned char)(i & 0xFF)));
        ff_put(&a, ptrs[i]);
    }
    for (size_t i = (n % 2 == 0) ? n - 2 : n - 1;; i -= 2) {
        ASSERT_TRUE(filled_with(ptrs[i], 100, (unsigned char)(i & 0xFF)));
        ff_put(&a, ptrs[i]);
        if (i < 2) {
            break;
        }
    }
    ASSERT_TRUE(at_baseline(&a, &base));
}

TEST(usable_size_covers_every_request) {
    Arena a = arena_of(g_big, sizeof g_big);
    AllocStats base = stat_of(&a);
    for (size_t n = 1; n <= 4096; n++) {
        unsigned char *p = ff_get(&a, n);
        ASSERT_TRUE(p != NULL);
        ASSERT_EQ((uintptr_t)p % ALLOC_ALIGN, 0);
        size_t usable = ff_usable(&a, p);
        ASSERT_TRUE(usable >= n);
        memset(p, 0xC3, usable);
        ff_put(&a, p);
    }
    ASSERT_TRUE(at_baseline(&a, &base));
}

// External fragmentation: plenty of free bytes, no single hole big enough.
TEST(fragmentation_separates_free_bytes_from_largest_free) {
    Arena a = arena_of(g_mid, sizeof g_mid);
    AllocStats base = stat_of(&a);
    unsigned char *ptrs[256];
    size_t n = 0;
    while (n < 256) {
        unsigned char *p = ff_get(&a, 100);
        if (p == NULL) {
            break;
        }
        ptrs[n++] = p;
    }
    ASSERT_TRUE(n > 8);
    for (size_t i = 0; i < n; i += 2) {
        ff_put(&a, ptrs[i]);
    }
    AllocStats s = stat_of(&a);
    ASSERT_TRUE(s.free_blocks > 1);
    ASSERT_TRUE(s.largest_free < s.free_bytes);
    ASSERT_TRUE(ff_get(&a, s.largest_free) == NULL);

    for (size_t i = 1; i < n; i += 2) {
        ff_put(&a, ptrs[i]);
    }
    ASSERT_TRUE(at_baseline(&a, &base));
}

TEST(two_arenas_stay_independent) {
    Arena a = arena_of(g_big, sizeof g_big);
    Arena b = arena_of(g_other, sizeof g_other);
    AllocStats base_a = stat_of(&a);
    AllocStats base_b = stat_of(&b);
    ASSERT_TRUE(base_a.largest_free > base_b.largest_free);

    unsigned char *a1 = ff_get(&a, 200);
    unsigned char *b1 = ff_get(&b, 200);
    unsigned char *a2 = ff_get(&a, 50);
    unsigned char *b2 = ff_get(&b, 50);
    ASSERT_TRUE(a1 != NULL && b1 != NULL && a2 != NULL && b2 != NULL);
    memset(a1, 0x11, 200);
    memset(b1, 0x22, 200);
    memset(a2, 0x33, 50);
    memset(b2, 0x44, 50);
    ASSERT_TRUE(b1 >= g_other && b1 < g_other + sizeof g_other);
    ASSERT_TRUE(a1 >= g_big && a1 < g_big + sizeof g_big);

    ff_put(&a, a1);
    unsigned char *b3 = ff_get(&b, 300);
    ASSERT_TRUE(b3 != NULL);
    memset(b3, 0x55, 300);
    ASSERT_TRUE(filled_with(b1, 200, 0x22));
    ASSERT_TRUE(filled_with(a2, 50, 0x33));
    ASSERT_TRUE(filled_with(b2, 50, 0x44));

    AllocStats sa = stat_of(&a);
    AllocStats sb = stat_of(&b);
    ASSERT_EQ(sa.arena_size, sizeof g_big);
    ASSERT_EQ(sb.arena_size, sizeof g_other);
    ASSERT_EQ(sa.live_blocks, 1);
    ASSERT_EQ(sb.live_blocks, 3);

    ff_put(&a, a2);
    ff_put(&b, b1);
    ff_put(&b, b3);
    ff_put(&b, b2);
    ASSERT_TRUE(at_baseline(&a, &base_a));
    ASSERT_TRUE(at_baseline(&b, &base_b));
}

typedef struct {
    unsigned char *p;
    size_t n;
    unsigned char fill;
} Shadow;

TEST(random_soak_keeps_every_pattern) {
    Arena a = arena_of(g_big, sizeof g_big);
    AllocStats base = stat_of(&a);
    static Shadow live[1024];
    size_t count = 0;
    srand(12345);
    for (int op = 0; op < 5000; op++) {
        int grow = count == 0 || (count < 1024 && rand() % 3 != 0);
        if (grow) {
            size_t n = (size_t)(rand() % 2048) + 1;
            unsigned char fill = (unsigned char)(rand() & 0xFF);
            unsigned char *p = ff_get(&a, n);
            if (p == NULL) {
                continue;
            }
            ASSERT_EQ((uintptr_t)p % ALLOC_ALIGN, 0);
            ASSERT_TRUE(ff_usable(&a, p) >= n);
            memset(p, fill, n);
            live[count].p = p;
            live[count].n = n;
            live[count].fill = fill;
            count++;
        } else {
            size_t k = (size_t)rand() % count;
            ASSERT_TRUE(filled_with(live[k].p, live[k].n, live[k].fill));
            ff_put(&a, live[k].p);
            live[k] = live[--count];
        }
    }
    while (count > 0) {
        count--;
        ASSERT_TRUE(filled_with(live[count].p, live[count].n, live[count].fill));
        ff_put(&a, live[count].p);
    }
    ASSERT_TRUE(at_baseline(&a, &base));
}

TEST(dump_lists_blocks_in_address_order) {
    Arena a = arena_of(g_small, sizeof g_small);
    void *p = ff_get(&a, 64);
    ASSERT_TRUE(p != NULL);
    FILE *out = tmpfile();
    ASSERT_TRUE(out != NULL);
    ALLOC_FIRSTFIT.dump(&a, out);
    rewind(out);
    char line[128];
    ASSERT_TRUE(fgets(line, sizeof line, out) != NULL);
    ASSERT_TRUE(strstr(line, "USED") != NULL);
    ASSERT_TRUE(fgets(line, sizeof line, out) != NULL);
    ASSERT_TRUE(strstr(line, "FREE") != NULL);
    ASSERT_TRUE(fgets(line, sizeof line, out) == NULL);
    fclose(out);
    ff_put(&a, p);
}

TEST_MAIN()
