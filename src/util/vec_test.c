// Tests for the pointer vector.

#include "vec.h"

#include <stdint.h>

#include "../alloc/alloc.h"
#include "../../tests/harness.h"

// A distinct non-NULL pointer value, never dereferenced, only compared.
static void *tag(size_t i) {
    return (void *)(uintptr_t)(i + 1);
}

TEST(init_gives_an_empty_but_allocated_vector) {
    Vec v;
    vec_init(&v);
    ASSERT_EQ(v.len, 0);
    ASSERT_TRUE(v.cap > 0);
    ASSERT_TRUE(v.items != NULL);
    vec_free(&v);
}

TEST(free_of_an_empty_vector_resets_it) {
    Vec v;
    vec_init(&v);
    vec_free(&v);
    ASSERT_EQ(v.len, 0);
    ASSERT_EQ(v.cap, 0);
    ASSERT_TRUE(v.items == NULL);
}

TEST(free_twice_is_safe) {
    Vec v;
    vec_init(&v);
    vec_push(&v, tag(0));
    vec_free(&v);
    vec_free(&v);
    ASSERT_EQ(v.len, 0);
    ASSERT_EQ(v.cap, 0);
    ASSERT_TRUE(v.items == NULL);
}

TEST(push_get_pop_round_trip) {
    Vec v;
    vec_init(&v);

    vec_push(&v, tag(0));
    vec_push(&v, tag(1));
    vec_push(&v, tag(2));
    ASSERT_EQ(v.len, 3);

    ASSERT_TRUE(vec_get(&v, 0) == tag(0));
    ASSERT_TRUE(vec_get(&v, 1) == tag(1));
    ASSERT_TRUE(vec_get(&v, 2) == tag(2));

    ASSERT_TRUE(vec_pop(&v) == tag(2));
    ASSERT_EQ(v.len, 2);
    ASSERT_TRUE(vec_pop(&v) == tag(1));
    ASSERT_TRUE(vec_pop(&v) == tag(0));
    ASSERT_EQ(v.len, 0);

    vec_free(&v);
}

TEST(direct_item_access_matches_vec_get) {
    Vec v;
    vec_init(&v);
    for (size_t i = 0; i < 5; i++) {
        vec_push(&v, tag(i));
    }
    for (size_t i = 0; i < v.len; i++) {
        ASSERT_TRUE(v.items[i] == vec_get(&v, i));
    }
    vec_free(&v);
}

TEST(growth_retains_every_item) {
    const size_t n = 10000;
    Vec v;
    vec_init(&v);
    for (size_t i = 0; i < n; i++) {
        vec_push(&v, tag(i));
    }
    ASSERT_EQ(v.len, n);
    ASSERT_TRUE(v.cap >= n);
    for (size_t i = 0; i < n; i++) {
        ASSERT_TRUE(vec_get(&v, i) == tag(i));
    }
    vec_free(&v);
}

TEST(growth_doubles_capacity) {
    Vec v;
    vec_init(&v);
    size_t start_cap = v.cap;
    while (v.len < start_cap) {
        vec_push(&v, tag(v.len));
    }
    ASSERT_EQ(v.cap, start_cap);
    vec_push(&v, tag(v.len));
    ASSERT_EQ(v.cap, start_cap * 2);
    vec_free(&v);
}

TEST(pop_to_empty_then_pop_again_returns_null) {
    Vec v;
    vec_init(&v);
    vec_push(&v, tag(0));
    ASSERT_TRUE(vec_pop(&v) == tag(0));
    ASSERT_EQ(v.len, 0);
    ASSERT_TRUE(vec_pop(&v) == NULL);
    ASSERT_TRUE(vec_pop(&v) == NULL);
    ASSERT_EQ(v.len, 0);
    vec_free(&v);
}

TEST(pop_on_a_fresh_vector_returns_null) {
    Vec v;
    vec_init(&v);
    ASSERT_TRUE(vec_pop(&v) == NULL);
    vec_free(&v);
}

TEST(get_out_of_range_returns_null) {
    Vec v;
    vec_init(&v);
    ASSERT_TRUE(vec_get(&v, 0) == NULL);
    vec_push(&v, tag(0));
    vec_push(&v, tag(1));
    ASSERT_TRUE(vec_get(&v, 2) == NULL);
    ASSERT_TRUE(vec_get(&v, 100) == NULL);
    // cap exceeds len here, so the slot exists but is still out of range.
    ASSERT_TRUE(v.cap > 2);
    ASSERT_TRUE(vec_get(&v, v.cap - 1) == NULL);
    vec_free(&v);
}

// A stored NULL is a real element, told from out of range only by len.
TEST(push_null_is_legal_and_retrievable) {
    Vec v;
    vec_init(&v);
    vec_push(&v, NULL);
    vec_push(&v, tag(0));
    vec_push(&v, NULL);
    ASSERT_EQ(v.len, 3);
    ASSERT_TRUE(vec_get(&v, 0) == NULL);
    ASSERT_TRUE(vec_get(&v, 1) == tag(0));
    ASSERT_TRUE(vec_get(&v, 2) == NULL);
    ASSERT_TRUE(vec_get(&v, 3) == NULL);
    ASSERT_TRUE(vec_pop(&v) == NULL);
    ASSERT_EQ(v.len, 2);
    vec_free(&v);
}

TEST(free_deep_releases_every_element) {
    Vec v;
    vec_init(&v);
    for (size_t i = 0; i < 64; i++) {
        size_t *p = nsh_malloc(sizeof(*p));
        *p = i;
        vec_push(&v, p);
    }
    for (size_t i = 0; i < v.len; i++) {
        ASSERT_EQ(*(size_t *)vec_get(&v, i), i);
    }
    vec_free_deep(&v, nsh_free);
    ASSERT_EQ(v.len, 0);
    ASSERT_EQ(v.cap, 0);
    ASSERT_TRUE(v.items == NULL);
}

TEST(free_deep_skips_null_elements) {
    Vec v;
    vec_init(&v);
    size_t *p = nsh_malloc(sizeof(*p));
    *p = 42;
    vec_push(&v, NULL);
    vec_push(&v, p);
    vec_push(&v, NULL);
    vec_free_deep(&v, nsh_free);
    ASSERT_TRUE(v.items == NULL);
}

TEST(free_deep_on_an_empty_vector_is_safe) {
    Vec v;
    vec_init(&v);
    vec_free_deep(&v, nsh_free);
    ASSERT_EQ(v.cap, 0);
    vec_free_deep(&v, nsh_free);
    ASSERT_TRUE(v.items == NULL);
}

TEST(a_freed_vector_can_be_reused) {
    Vec v;
    vec_init(&v);
    vec_push(&v, tag(0));
    vec_free(&v);
    vec_push(&v, tag(1));
    ASSERT_EQ(v.len, 1);
    ASSERT_TRUE(v.cap > 0);
    ASSERT_TRUE(vec_get(&v, 0) == tag(1));
    vec_free(&v);
}

TEST_MAIN()
