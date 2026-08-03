// Proves the harness itself works: registration runs, every assertion form
// accepts a true case, and the runner reports success. Failure paths would
// need a fork to observe, so they are left alone.

#include "harness.h"

TEST(assert_true_accepts_truth) {
    ASSERT_TRUE(1);
    ASSERT_TRUE("non null pointer" != NULL);
}

TEST(assert_eq_accepts_equal_numbers) {
    ASSERT_EQ(1 + 1, 2);
    ASSERT_EQ(-5, -5);
    ASSERT_EQ((char)'a', 97);
}

TEST(assert_str_eq_accepts_equal_strings) {
    char buf[8];
    buf[0] = 'h';
    buf[1] = 'i';
    buf[2] = '\0';
    ASSERT_STR_EQ(buf, "hi");
    ASSERT_STR_EQ("", "");
}

TEST(registration_saw_every_test) {
    // Four TEST() blocks live in this file, all registered before main ran.
    ASSERT_EQ(nsh_test_count, 4);
}

TEST_MAIN()
