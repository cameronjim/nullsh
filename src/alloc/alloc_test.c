// Tests for the public allocator: contract, routing, guards, and a randomized soak.

#define _POSIX_C_SOURCE 200809L

#include "alloc.h"

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

TEST(realloc_to_zero_returns_a_live_block) {
    unsigned char *p = nsh_malloc(32);
    p = nsh_realloc(p, 0);
    ASSERT_TRUE(p != NULL);
    p[0] = 7;
    ASSERT_EQ(p[0], 7);
    nsh_free(p);
}

static int pattern_ok(const unsigned char *p, size_t n, unsigned char fill) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] != fill) {
            return 0;
        }
    }
    return 1;
}

TEST(free_routes_by_address_across_a_live_switch) {
    const char *boot = alloc_strategy_name();
    ASSERT_EQ(alloc_set_strategy("firstfit"), NSH_OK);
    unsigned char *a = nsh_malloc(128);
    memset(a, 0xA1, 128);
    ASSERT_EQ(alloc_set_strategy("buddy"), NSH_OK);
    unsigned char *b = nsh_malloc(128);
    memset(b, 0xB2, 128);
    ASSERT_TRUE(a != b);
    ASSERT_TRUE(pattern_ok(a, 128, 0xA1));
    ASSERT_TRUE(pattern_ok(b, 128, 0xB2));
    // A firstfit block freed while buddy is active must still find its own arena.
    nsh_free(a);
    ASSERT_TRUE(pattern_ok(b, 128, 0xB2));
    nsh_free(b);
    ASSERT_EQ(alloc_set_strategy(boot), NSH_OK);
}

TEST(realloc_moves_across_strategies_and_keeps_content) {
    const char *boot = alloc_strategy_name();
    ASSERT_EQ(alloc_set_strategy("firstfit"), NSH_OK);
    char *p = nsh_malloc(32);
    memcpy(p, "cross-strategy realloc payload", 31);
    ASSERT_EQ(alloc_set_strategy("buddy"), NSH_OK);
    char *q = nsh_realloc(p, 4096);
    ASSERT_TRUE(q != p);
    ASSERT_STR_EQ(q, "cross-strategy realloc payload");
    memset(q + 31, 0xC3, 4096 - 31);
    ASSERT_TRUE(pattern_ok((unsigned char *)q + 31, 4096 - 31, 0xC3));
    nsh_free(q);
    ASSERT_EQ(alloc_set_strategy(boot), NSH_OK);
}

TEST(realloc_grows_in_place_inside_one_size_class) {
    const char *boot = alloc_strategy_name();
    ASSERT_EQ(alloc_set_strategy("buddy"), NSH_OK);
    unsigned char *p = nsh_malloc(16);
    memset(p, 0x5A, 16);
    unsigned char *q = nsh_realloc(p, 24);
    ASSERT_TRUE(q == p);
    ASSERT_TRUE(pattern_ok(q, 16, 0x5A));
    q[23] = 0x5A;
    unsigned char *r = nsh_realloc(q, 8);
    ASSERT_TRUE(r == q);
    ASSERT_TRUE(pattern_ok(r, 8, 0x5A));
    nsh_free(r);
    ASSERT_EQ(alloc_set_strategy(boot), NSH_OK);
}

TEST(stats_track_counters_usage_and_strategy) {
    const char *boot = alloc_strategy_name();
    AllocStats before, mid, after;
    ASSERT_EQ(alloc_get_stats(&before), NSH_OK);
    ASSERT_STR_EQ(before.strategy, boot);
    ASSERT_TRUE(before.arena_size > 0);
    void *p = nsh_malloc(50000);
    ASSERT_EQ(alloc_get_stats(&mid), NSH_OK);
    ASSERT_TRUE(mid.total_mallocs > before.total_mallocs);
    ASSERT_TRUE(mid.used_bytes > before.used_bytes);
    ASSERT_TRUE(mid.live_blocks > before.live_blocks);
    nsh_free(p);
    ASSERT_EQ(alloc_get_stats(&after), NSH_OK);
    ASSERT_TRUE(after.total_frees > mid.total_frees);
    ASSERT_EQ(after.used_bytes, before.used_bytes);
    ASSERT_EQ(after.live_blocks, before.live_blocks);
    ASSERT_EQ(alloc_set_strategy("buddy"), NSH_OK);
    ASSERT_EQ(alloc_get_stats(&after), NSH_OK);
    ASSERT_STR_EQ(after.strategy, "buddy");
    ASSERT_STR_EQ(alloc_strategy_name(), "buddy");
    ASSERT_EQ(alloc_set_strategy("firstfit"), NSH_OK);
    ASSERT_EQ(alloc_get_stats(&after), NSH_OK);
    ASSERT_STR_EQ(after.strategy, "firstfit");
    ASSERT_EQ(alloc_set_strategy(boot), NSH_OK);
}

TEST(unknown_strategy_name_is_rejected) {
    const char *boot = alloc_strategy_name();
    ASSERT_EQ(alloc_set_strategy("nope"), NSH_ERR_INVALID);
    ASSERT_EQ(alloc_set_strategy(NULL), NSH_ERR_INVALID);
    ASSERT_STR_EQ(alloc_strategy_name(), boot);
}

#define SOAK_SLOTS 256
#define SOAK_OPS 20000
#define SOAK_MAX 4096

typedef struct {
    unsigned char *p;
    size_t size;
    unsigned char fill;
} Shadow;

static Shadow g_soak[SOAK_SLOTS];

TEST(randomized_soak_keeps_every_block_intact) {
    AllocStats before, after;
    ASSERT_EQ(alloc_get_stats(&before), NSH_OK);
    memset(g_soak, 0, sizeof g_soak);
    srand(99);
    for (int op = 0; op < SOAK_OPS; op++) {
        Shadow *s = &g_soak[rand() % SOAK_SLOTS];
        int what = rand() % 4;
        size_t n = (size_t)(rand() % SOAK_MAX) + 1;
        if (s->p == NULL) {
            s->size = n;
            s->fill = (unsigned char)(rand() & 0xFF);
            s->p = (what == 1) ? nsh_calloc(n, 1) : nsh_malloc(n);
            ASSERT_TRUE(s->p != NULL);
            if (what == 1) {
                ASSERT_TRUE(pattern_ok(s->p, n, 0));
            }
            memset(s->p, s->fill, n);
            continue;
        }
        ASSERT_TRUE(pattern_ok(s->p, s->size, s->fill));
        if (what == 3) {
            size_t keep = n < s->size ? n : s->size;
            unsigned char *q = nsh_realloc(s->p, n);
            ASSERT_TRUE(q != NULL);
            ASSERT_TRUE(pattern_ok(q, keep, s->fill));
            s->p = q;
            s->size = n;
            memset(s->p, s->fill, n);
        } else {
            nsh_free(s->p);
            s->p = NULL;
            s->size = 0;
        }
    }
    for (int i = 0; i < SOAK_SLOTS; i++) {
        if (g_soak[i].p != NULL) {
            ASSERT_TRUE(pattern_ok(g_soak[i].p, g_soak[i].size, g_soak[i].fill));
            nsh_free(g_soak[i].p);
            g_soak[i].p = NULL;
        }
    }
    ASSERT_EQ(alloc_get_stats(&after), NSH_OK);
    ASSERT_EQ(after.used_bytes, before.used_bytes);
    ASSERT_EQ(after.live_blocks, before.live_blocks);
    ASSERT_TRUE(after.total_mallocs > before.total_mallocs + 1000);
    ASSERT_TRUE(after.total_frees > before.total_frees + 1000);
}

// Corrupts one guard byte in a child and reports how the child died.
static int run_corrupting_child(int overflow) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
        }
        unsigned char *p = nsh_malloc(64);
        memset(p, 0x11, 64);
        if (overflow) {
            p[64] = 0x00;
        } else {
            p[-1] = 0x00;
        }
        nsh_free(p);
        _exit(0);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        return -1;
    }
    return status;
}

TEST(overflow_past_the_payload_aborts_on_free) {
    int status = run_corrupting_child(1);
    ASSERT_TRUE(status >= 0);
    ASSERT_TRUE(!(WIFEXITED(status) && WEXITSTATUS(status) == 0));
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGABRT);
}

TEST(underflow_into_the_prefix_aborts_on_free) {
    int status = run_corrupting_child(0);
    ASSERT_TRUE(status >= 0);
    ASSERT_TRUE(!(WIFEXITED(status) && WEXITSTATUS(status) == 0));
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGABRT);
}

// Runs body in a forked child with stderr silenced and reports how the child died.
static int run_child(void (*body)(void)) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
        }
        body();
        _exit(0);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        return -1;
    }
    return status;
}

#define ASSERT_ABORTED(status)                                                \
    do {                                                                      \
        ASSERT_TRUE((status) >= 0);                                           \
        ASSERT_TRUE(!(WIFEXITED(status) && WEXITSTATUS(status) == 0));        \
        ASSERT_TRUE(WIFSIGNALED(status));                                     \
        ASSERT_EQ(WTERMSIG(status), SIGABRT);                                 \
    } while (0)

static const char *const BOTH_STRATEGIES[2] = {"firstfit", "buddy"};

static void child_double_free(void) {
    void *p = nsh_malloc(64);
    nsh_free(p);
    nsh_free(p);
}

TEST(double_free_aborts) {
    for (int i = 0; i < 2; i++) {
        ASSERT_EQ(alloc_set_strategy(BOTH_STRATEGIES[i]), NSH_OK);
        int status = run_child(child_double_free);
        ASSERT_ABORTED(status);
    }
    ASSERT_EQ(alloc_set_strategy("firstfit"), NSH_OK);
}

static void child_interior_free(void) {
    unsigned char *p = nsh_malloc(256);
    memset(p, 0x33, 256);
    nsh_free(p + 8);
}

TEST(interior_pointer_free_aborts) {
    int status = run_child(child_interior_free);
    ASSERT_ABORTED(status);
}

static void child_unknown_free(void) {
    unsigned char on_stack[32];
    memset(on_stack, 0, sizeof on_stack);
    nsh_free(on_stack);
}

TEST(unknown_pointer_free_aborts) {
    int status = run_child(child_unknown_free);
    ASSERT_ABORTED(status);
}

static void child_oom(void) {
    unsigned char *p = nsh_malloc((size_t)64 << 20);
    p[0] = 1;
}

TEST(oom_aborts) {
    for (int i = 0; i < 2; i++) {
        ASSERT_EQ(alloc_set_strategy(BOTH_STRATEGIES[i]), NSH_OK);
        int status = run_child(child_oom);
        ASSERT_ABORTED(status);
    }
    ASSERT_EQ(alloc_set_strategy("firstfit"), NSH_OK);
}

TEST(freed_memory_is_poisoned) {
    for (int i = 0; i < 2; i++) {
        ASSERT_EQ(alloc_set_strategy(BOTH_STRATEGIES[i]), NSH_OK);
        unsigned char *p = nsh_malloc(512);
        memset(p, 0xAB, 512);
        nsh_free(p);
        // Deliberate use after free: the arena stays mapped, so reading the poison back is defined at the machine level.
        ASSERT_TRUE(pattern_ok(p + 16, 512 - 16, 0xDD));
    }
    ASSERT_EQ(alloc_set_strategy("firstfit"), NSH_OK);
}

#define SWITCH_SLOTS 128
#define SWITCH_OPS 10000
#define SWITCH_MAX 2048
#define SWITCH_EVERY 100

static Shadow g_switch[SWITCH_SLOTS];

TEST(soak_survives_live_strategy_switches) {
    AllocStats ff_before, bd_before, after;
    ASSERT_EQ(alloc_set_strategy("firstfit"), NSH_OK);
    ASSERT_EQ(alloc_get_stats(&ff_before), NSH_OK);
    ASSERT_EQ(alloc_set_strategy("buddy"), NSH_OK);
    ASSERT_EQ(alloc_get_stats(&bd_before), NSH_OK);
    memset(g_switch, 0, sizeof g_switch);
    srand(4242);
    for (int op = 0; op < SWITCH_OPS; op++) {
        if (op % SWITCH_EVERY == 0) {
            ASSERT_EQ(alloc_set_strategy(
                          BOTH_STRATEGIES[(op / SWITCH_EVERY) % 2]),
                      NSH_OK);
        }
        Shadow *s = &g_switch[rand() % SWITCH_SLOTS];
        int what = rand() % 4;
        size_t n = (size_t)(rand() % SWITCH_MAX) + 1;
        if (s->p == NULL) {
            s->size = n;
            s->fill = (unsigned char)(rand() & 0xFF);
            s->p = (what == 1) ? nsh_calloc(n, 1) : nsh_malloc(n);
            ASSERT_TRUE(s->p != NULL);
            memset(s->p, s->fill, n);
            continue;
        }
        ASSERT_TRUE(pattern_ok(s->p, s->size, s->fill));
        if (what == 3) {
            size_t keep = n < s->size ? n : s->size;
            unsigned char *q = nsh_realloc(s->p, n);
            ASSERT_TRUE(q != NULL);
            ASSERT_TRUE(pattern_ok(q, keep, s->fill));
            s->p = q;
            s->size = n;
            memset(s->p, s->fill, n);
        } else {
            nsh_free(s->p);
            s->p = NULL;
            s->size = 0;
        }
    }
    for (int i = 0; i < SWITCH_SLOTS; i++) {
        if (g_switch[i].p != NULL) {
            ASSERT_TRUE(pattern_ok(g_switch[i].p, g_switch[i].size,
                                   g_switch[i].fill));
            nsh_free(g_switch[i].p);
            g_switch[i].p = NULL;
            g_switch[i].size = 0;
        }
    }
    ASSERT_EQ(alloc_set_strategy("firstfit"), NSH_OK);
    ASSERT_EQ(alloc_get_stats(&after), NSH_OK);
    ASSERT_EQ(after.used_bytes, ff_before.used_bytes);
    ASSERT_EQ((long long)after.total_frees - (long long)after.total_mallocs,
              (long long)ff_before.total_frees -
                  (long long)ff_before.total_mallocs);
    ASSERT_EQ(alloc_set_strategy("buddy"), NSH_OK);
    ASSERT_EQ(alloc_get_stats(&after), NSH_OK);
    ASSERT_EQ(after.used_bytes, bd_before.used_bytes);
    ASSERT_EQ((long long)after.total_frees - (long long)after.total_mallocs,
              (long long)bd_before.total_frees -
                  (long long)bd_before.total_mallocs);
    ASSERT_EQ(alloc_set_strategy("firstfit"), NSH_OK);
}

TEST_MAIN()
