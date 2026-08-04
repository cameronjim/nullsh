// The nullsh test framework: TEST, the ASSERT_* macros, and TEST_MAIN.

#pragma once

#include <stdio.h>
#include <string.h>

#define NSH_MAX_TESTS 256

typedef void (*NshTestFn)(int *nsh_failed);

typedef struct {
    const char *name;
    NshTestFn fn;
} NshTestCase;

static NshTestCase nsh_test_cases[NSH_MAX_TESTS];
static int nsh_test_count;

// The hidden nsh_failed flag is what the ASSERT_* macros set before returning.
#define TEST(test_name)                                                       \
    static void nsh_test_##test_name(int *nsh_failed);                        \
    __attribute__((constructor)) static void nsh_reg_##test_name(void) {      \
        if (nsh_test_count < NSH_MAX_TESTS) {                                 \
            nsh_test_cases[nsh_test_count].name = #test_name;                 \
            nsh_test_cases[nsh_test_count].fn = nsh_test_##test_name;         \
            nsh_test_count++;                                                 \
        }                                                                     \
    }                                                                         \
    static void nsh_test_##test_name(int *nsh_failed)

#define ASSERT_TRUE(x)                                                        \
    do {                                                                      \
        if (!(x)) {                                                           \
            printf("    %s:%d: ASSERT_TRUE(%s) is false\n", __FILE__,         \
                   __LINE__, #x);                                             \
            *nsh_failed = 1;                                                  \
            return;                                                           \
        }                                                                     \
    } while (0)

#define ASSERT_EQ(a, b)                                                       \
    do {                                                                      \
        long long nsh_lhs = (long long)(a);                                   \
        long long nsh_rhs = (long long)(b);                                   \
        if (nsh_lhs != nsh_rhs) {                                             \
            printf("    %s:%d: ASSERT_EQ(%s, %s): %lld != %lld\n", __FILE__,  \
                   __LINE__, #a, #b, nsh_lhs, nsh_rhs);                       \
            *nsh_failed = 1;                                                  \
            return;                                                           \
        }                                                                     \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                   \
    do {                                                                      \
        const char *nsh_lhs = (a);                                            \
        const char *nsh_rhs = (b);                                            \
        if (nsh_lhs == NULL || nsh_rhs == NULL ||                             \
            strcmp(nsh_lhs, nsh_rhs) != 0) {                                  \
            printf("    %s:%d: ASSERT_STR_EQ(%s, %s): \"%s\" != \"%s\"\n",    \
                   __FILE__, __LINE__, #a, #b,                                \
                   nsh_lhs ? nsh_lhs : "(null)",                              \
                   nsh_rhs ? nsh_rhs : "(null)");                             \
            *nsh_failed = 1;                                                  \
            return;                                                           \
        }                                                                     \
    } while (0)

#define TEST_MAIN()                                                           \
    int main(void) {                                                          \
        int failures = 0;                                                     \
        for (int i = 0; i < nsh_test_count; i++) {                            \
            int failed = 0;                                                   \
            nsh_test_cases[i].fn(&failed);                                    \
            printf("  %s %s\n", failed ? "FAIL" : "ok  ",                     \
                   nsh_test_cases[i].name);                                   \
            failures += failed;                                               \
        }                                                                     \
        printf("  %d test(s), %d failed\n", nsh_test_count, failures);        \
        return failures == 0 ? 0 : 1;                                         \
    }
