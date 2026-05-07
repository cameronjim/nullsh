// Unit tests for the allocator wrappers, including the size-0 contract that
// the header promises.

#include "alloc.h"

#include <string.h>

#include "../../tests/harness.h"

TEST(malloc_returns_writable_memory) {
    const size_t n = 64;
    unsigned char *p = nsh_malloc(n);
    ASSERT_TRUE(p != NULL);
    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)(i & 0xFF);
    }
    for (size_t i = 0; i < n; i++) {
        ASSERT_EQ(p[i], (unsigned char)(i & 0xFF));
    }
    nsh_free(p);
}

TEST(calloc_zeroes_every_byte) {
    const size_t count = 32;
    unsigned char *p = nsh_calloc(count, sizeof(*p));
    ASSERT_TRUE(p != NULL);
    for (size_t i = 0; i < count; i++) {
        ASSERT_EQ(p[i], 0);
    }
    nsh_free(p);
}

TEST(realloc_grows_and_preserves_content) {
    char *p = nsh_malloc(8);
    memcpy(p, "1234567", 8);
    p = nsh_realloc(p, 256);
    ASSERT_TRUE(p != NULL);
    ASSERT_STR_EQ(p, "1234567");
    memcpy(p + 7, "89abcdef", 9);
    ASSERT_STR_EQ(p, "123456789abcdef");
    nsh_free(p);
}

TEST(realloc_of_null_behaves_like_malloc) {
    char *p = nsh_realloc(NULL, 16);
    ASSERT_TRUE(p != NULL);
    memset(p, 'x', 16);
    ASSERT_EQ(p[15], 'x');
    nsh_free(p);
}

TEST(free_of_null_is_safe) {
    nsh_free(NULL);
    ASSERT_TRUE(1);
}

// Documented contract: a size-0 request yields a unique, writable, freeable
// block of at least one byte. It never returns NULL.
TEST(zero_size_requests_return_unique_writable_blocks) {
    unsigned char *a = nsh_malloc(0);
    unsigned char *b = nsh_malloc(0);
    unsigned char *c = nsh_calloc(0, 0);
    ASSERT_TRUE(a != NULL);
    ASSERT_TRUE(b != NULL);
    ASSERT_TRUE(c != NULL);
    ASSERT_TRUE(a != b);
    ASSERT_TRUE(b != c);
    a[0] = 1;
    b[0] = 2;
    ASSERT_EQ(a[0], 1);
    ASSERT_EQ(b[0], 2);
    ASSERT_EQ(c[0], 0);
    nsh_free(a);
    nsh_free(b);
    nsh_free(c);
}

// Documented contract: realloc to 0 shrinks the block, it does not free it,
// so the caller still owns a valid pointer.
TEST(realloc_to_zero_returns_a_live_block) {
    unsigned char *p = nsh_malloc(32);
    p = nsh_realloc(p, 0);
    ASSERT_TRUE(p != NULL);
    p[0] = 7;
    ASSERT_EQ(p[0], 7);
    nsh_free(p);
}

TEST_MAIN()
