// Unit tests for the NshError to string mapping.

#include "error.h"

#include "../../tests/harness.h"

static const NshError ALL_ERRORS[] = {
    NSH_OK,        NSH_ERR_ALLOC,     NSH_ERR_SYNTAX,
    NSH_ERR_IO,    NSH_ERR_NOT_FOUND, NSH_ERR_INVALID,
};

static const int ALL_ERRORS_COUNT =
    (int)(sizeof(ALL_ERRORS) / sizeof(ALL_ERRORS[0]));

TEST(every_code_has_a_non_empty_string) {
    for (int i = 0; i < ALL_ERRORS_COUNT; i++) {
        const char *s = nsh_error_str(ALL_ERRORS[i]);
        ASSERT_TRUE(s != NULL);
        ASSERT_TRUE(s[0] != '\0');
    }
}

TEST(strings_are_pairwise_distinct) {
    for (int i = 0; i < ALL_ERRORS_COUNT; i++) {
        for (int j = i + 1; j < ALL_ERRORS_COUNT; j++) {
            const char *a = nsh_error_str(ALL_ERRORS[i]);
            const char *b = nsh_error_str(ALL_ERRORS[j]);
            ASSERT_TRUE(strcmp(a, b) != 0);
        }
    }
}

TEST(ok_is_zero_and_reads_as_ok) {
    ASSERT_EQ((int)NSH_OK, 0);
    ASSERT_STR_EQ(nsh_error_str(NSH_OK), "ok");
}

TEST_MAIN()
