// Tests for the job table: ids, lookups, state transitions, and reaping.

#define _POSIX_C_SOURCE 200809L

#include "jobs.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "../alloc/alloc.h"
#include "../../tests/harness.h"

// Wait statuses cannot be built portably, so these use the Linux layout:
// exit code<<8, stop 0x7f|sig<<8, continue 0xffff, killed by signal sig.
static int st_exit(int code) {
    return code << 8;
}

static int st_stop(int sig) {
    return 0x7f | (sig << 8);
}

static int st_cont(void) {
    return 0xffff;
}

static int st_kill(int sig) {
    return sig;
}

// Each recipe is checked against its macro before any test leans on it.
static int recipes_ok(void) {
    if (!WIFEXITED(st_exit(7)) || WEXITSTATUS(st_exit(7)) != 7) {
        printf("    SKIP: exit recipe does not match WIFEXITED\n");
        return 0;
    }
    if (!WIFSTOPPED(st_stop(SIGTSTP)) ||
        WSTOPSIG(st_stop(SIGTSTP)) != SIGTSTP) {
        printf("    SKIP: stop recipe does not match WIFSTOPPED\n");
        return 0;
    }
    if (!WIFCONTINUED(st_cont())) {
        printf("    SKIP: continue recipe does not match WIFCONTINUED\n");
        return 0;
    }
    if (!WIFSIGNALED(st_kill(SIGTERM)) ||
        WTERMSIG(st_kill(SIGTERM)) != SIGTERM) {
        printf("    SKIP: kill recipe does not match WIFSIGNALED\n");
        return 0;
    }
    return 1;
}

// The returned buffer comes from libc, so it takes free() and not nsh_free.
static char *reap_to_string(void) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (f == NULL) {
        return NULL;
    }
    jobs_reap_notify(f);
    fclose(f);
    return buf;
}

TEST(add_assigns_ids_and_fills_the_job) {
    jobs_init();
    pid_t one[] = {100};
    pid_t two[] = {200, 201};
    Job *j1 = jobs_add(100, one, 1, "sleep 1 &");
    Job *j2 = jobs_add(200, two, 2, "seq 3 | cat &");
    ASSERT_EQ(j1->id, 1);
    ASSERT_EQ(j2->id, 2);
    ASSERT_EQ(j1->pgid, 100);
    ASSERT_EQ(j2->state, JOB_RUNNING);
    ASSERT_EQ(j2->nproc, 2);
    ASSERT_EQ(j2->nleft, 2);
    ASSERT_EQ(j2->last_status, 0);
    ASSERT_STR_EQ(j1->cmdline, "sleep 1 &");
    ASSERT_EQ(jobs_count(), 2);
    jobs_free_all();
}

TEST(lookup_by_id_and_pgid) {
    jobs_init();
    pid_t a[] = {10};
    pid_t b[] = {20};
    Job *j1 = jobs_add(10, a, 1, "one");
    Job *j2 = jobs_add(20, b, 1, "two");
    ASSERT_TRUE(jobs_by_id(1) == j1);
    ASSERT_TRUE(jobs_by_id(2) == j2);
    ASSERT_TRUE(jobs_by_id(0) == NULL);
    ASSERT_TRUE(jobs_by_id(3) == NULL);
    ASSERT_TRUE(jobs_by_pgid(10) == j1);
    ASSERT_TRUE(jobs_by_pgid(20) == j2);
    ASSERT_TRUE(jobs_by_pgid(999) == NULL);
    jobs_free_all();
}

TEST(ids_count_up_then_reuse_after_pruning) {
    if (!recipes_ok()) {
        return;
    }
    jobs_init();
    pid_t a[] = {1}, b[] = {2}, c[] = {3};
    ASSERT_EQ(jobs_add(1, a, 1, "one")->id, 1);
    ASSERT_EQ(jobs_add(2, b, 1, "two")->id, 2);
    ASSERT_EQ(jobs_add(3, c, 1, "three")->id, 3);

    jobs_update(1, st_exit(0));
    char *out = reap_to_string();
    ASSERT_TRUE(out != NULL);
    ASSERT_STR_EQ(out, "[1]  Done  one\n");
    free(out);

    pid_t d[] = {4};
    ASSERT_EQ(jobs_add(4, d, 1, "four")->id, 1);
    pid_t e[] = {5};
    ASSERT_EQ(jobs_add(5, e, 1, "five")->id, 4);
    jobs_free_all();
}

TEST(cmdline_and_pids_are_copies) {
    jobs_init();
    char line[] = "sleep 5 &";
    pid_t pids[] = {7, 8, 9};
    Job *j = jobs_add(7, pids, 3, line);
    ASSERT_TRUE(j->cmdline != line);
    ASSERT_TRUE(j->pids != pids);
    line[0] = 'X';
    pids[0] = -1;
    pids[1] = -1;
    pids[2] = -1;
    ASSERT_STR_EQ(j->cmdline, "sleep 5 &");
    ASSERT_EQ(j->pids[0], 7);
    ASSERT_EQ(j->pids[1], 8);
    ASSERT_EQ(j->pids[2], 9);
    jobs_free_all();
}

TEST(empty_cmdline_and_no_pids_are_allowed) {
    jobs_init();
    Job *j = jobs_add(42, NULL, 0, "");
    ASSERT_EQ(j->nproc, 0);
    ASSERT_EQ(j->nleft, 0);
    ASSERT_STR_EQ(j->cmdline, "");
    ASSERT_EQ(jobs_count(), 1);
    jobs_free_all();
}

TEST(every_pid_reports_before_the_job_is_done) {
    if (!recipes_ok()) {
        return;
    }
    jobs_init();
    pid_t pids[] = {40, 41, 42};
    Job *j = jobs_add(40, pids, 3, "a | b | c");
    ASSERT_TRUE(jobs_update(40, st_exit(0)) == j);
    ASSERT_EQ(j->nleft, 2);
    ASSERT_EQ(j->state, JOB_RUNNING);
    ASSERT_TRUE(jobs_update(41, st_kill(SIGTERM)) == j);
    ASSERT_EQ(j->nleft, 1);
    ASSERT_EQ(j->state, JOB_RUNNING);
    ASSERT_TRUE(WIFSIGNALED(j->last_status));
    ASSERT_EQ(WTERMSIG(j->last_status), SIGTERM);
    ASSERT_TRUE(jobs_update(42, st_exit(3)) == j);
    ASSERT_EQ(j->nleft, 0);
    ASSERT_EQ(j->state, JOB_DONE);
    ASSERT_TRUE(WIFEXITED(j->last_status));
    ASSERT_EQ(WEXITSTATUS(j->last_status), 3);
    ASSERT_EQ(j->nproc, 3);
    jobs_free_all();
}

TEST(stop_then_continue_flips_state_only) {
    if (!recipes_ok()) {
        return;
    }
    jobs_init();
    pid_t pids[] = {50, 51};
    Job *j = jobs_add(50, pids, 2, "vim | cat");
    ASSERT_TRUE(jobs_update(50, st_stop(SIGTSTP)) == j);
    ASSERT_EQ(j->state, JOB_STOPPED);
    ASSERT_EQ(j->nleft, 2);
    ASSERT_EQ(j->last_status, 0);
    ASSERT_TRUE(jobs_update(51, st_cont()) == j);
    ASSERT_EQ(j->state, JOB_RUNNING);
    ASSERT_EQ(j->nleft, 2);
    ASSERT_TRUE(jobs_update(51, st_stop(SIGSTOP)) == j);
    ASSERT_EQ(j->state, JOB_STOPPED);
    jobs_update(50, st_exit(0));
    ASSERT_EQ(j->nleft, 1);
    ASSERT_EQ(j->state, JOB_STOPPED);
    jobs_update(51, st_exit(0));
    ASSERT_EQ(j->state, JOB_DONE);
    jobs_free_all();
}

TEST(update_with_unknown_pid_changes_nothing) {
    if (!recipes_ok()) {
        return;
    }
    jobs_init();
    pid_t pids[] = {60};
    Job *j = jobs_add(60, pids, 1, "one");
    ASSERT_TRUE(jobs_update(61, st_exit(0)) == NULL);
    ASSERT_TRUE(jobs_update(0, st_stop(SIGTSTP)) == NULL);
    ASSERT_TRUE(jobs_update(-1, st_cont()) == NULL);
    ASSERT_EQ(j->state, JOB_RUNNING);
    ASSERT_EQ(j->nleft, 1);
    ASSERT_EQ(j->last_status, 0);
    ASSERT_EQ(jobs_count(), 1);
    jobs_free_all();
}

TEST(current_prefers_recent_stopped_then_recent_running) {
    if (!recipes_ok()) {
        return;
    }
    jobs_init();
    ASSERT_TRUE(jobs_current() == NULL);
    pid_t a[] = {70}, b[] = {71}, c[] = {72};
    Job *j1 = jobs_add(70, a, 1, "one");
    ASSERT_TRUE(jobs_current() == j1);
    Job *j2 = jobs_add(71, b, 1, "two");
    ASSERT_TRUE(jobs_current() == j2);
    Job *j3 = jobs_add(72, c, 1, "three");
    ASSERT_TRUE(jobs_current() == j3);

    jobs_update(70, st_stop(SIGTSTP));
    ASSERT_TRUE(jobs_current() == j1);
    jobs_update(71, st_stop(SIGTSTP));
    ASSERT_TRUE(jobs_current() == j2);
    jobs_update(71, st_cont());
    ASSERT_TRUE(jobs_current() == j1);
    jobs_update(70, st_cont());
    ASSERT_TRUE(jobs_current() == j3);

    jobs_update(72, st_exit(0));
    ASSERT_EQ(j3->state, JOB_DONE);
    ASSERT_TRUE(jobs_current() == j2);
    jobs_update(71, st_exit(0));
    ASSERT_TRUE(jobs_current() == j1);
    jobs_update(70, st_exit(0));
    ASSERT_TRUE(jobs_current() == NULL);
    ASSERT_EQ(jobs_count(), 3);
    jobs_free_all();
}

TEST(reap_notify_prints_done_jobs_in_id_order_and_prunes) {
    if (!recipes_ok()) {
        return;
    }
    jobs_init();
    pid_t a[] = {80}, b[] = {81}, c[] = {82};
    jobs_add(80, a, 1, "first");
    jobs_add(81, b, 1, "second");
    jobs_add(82, c, 1, "third");
    jobs_update(82, st_exit(0));
    jobs_update(80, st_kill(SIGINT));
    jobs_update(81, st_stop(SIGTSTP));

    char *out = reap_to_string();
    ASSERT_TRUE(out != NULL);
    ASSERT_STR_EQ(out, "[1]  Done  first\n[3]  Done  third\n");
    free(out);

    ASSERT_EQ(jobs_count(), 1);
    ASSERT_TRUE(jobs_by_id(1) == NULL);
    ASSERT_TRUE(jobs_by_id(3) == NULL);
    ASSERT_TRUE(jobs_by_pgid(81) != NULL);
    ASSERT_EQ(jobs_by_id(2)->state, JOB_STOPPED);
    jobs_free_all();
}

TEST(reap_notify_orders_by_id_not_by_table_position) {
    if (!recipes_ok()) {
        return;
    }
    jobs_init();
    pid_t a[] = {90}, b[] = {91}, c[] = {92};
    jobs_add(90, a, 1, "alpha");
    jobs_add(91, b, 1, "beta");
    jobs_add(92, c, 1, "gamma");
    jobs_update(90, st_exit(0));
    char *first = reap_to_string();
    ASSERT_TRUE(first != NULL);
    free(first);

    // Reusing id 1 puts the lowest id last in the table.
    pid_t d[] = {93};
    Job *reused = jobs_add(93, d, 1, "delta");
    ASSERT_EQ(reused->id, 1);
    jobs_update(92, st_exit(0));
    jobs_update(93, st_exit(0));

    char *out = reap_to_string();
    ASSERT_TRUE(out != NULL);
    ASSERT_STR_EQ(out, "[1]  Done  delta\n[3]  Done  gamma\n");
    free(out);
    ASSERT_EQ(jobs_count(), 1);
    ASSERT_EQ(jobs_by_id(2)->pgid, 91);
    jobs_free_all();
}

TEST(reap_notify_prints_nothing_without_done_jobs) {
    if (!recipes_ok()) {
        return;
    }
    jobs_init();
    char *empty = reap_to_string();
    ASSERT_TRUE(empty != NULL);
    ASSERT_STR_EQ(empty, "");
    free(empty);

    pid_t a[] = {110, 111};
    jobs_add(110, a, 2, "half done");
    jobs_update(110, st_exit(0));
    char *out = reap_to_string();
    ASSERT_TRUE(out != NULL);
    ASSERT_STR_EQ(out, "");
    free(out);
    ASSERT_EQ(jobs_count(), 1);
    jobs_free_all();
}

TEST(count_holds_done_jobs_until_they_are_reaped) {
    if (!recipes_ok()) {
        return;
    }
    jobs_init();
    ASSERT_EQ(jobs_count(), 0);
    pid_t a[] = {120}, b[] = {121};
    jobs_add(120, a, 1, "one");
    jobs_add(121, b, 1, "two");
    ASSERT_EQ(jobs_count(), 2);
    jobs_update(120, st_exit(0));
    ASSERT_EQ(jobs_count(), 2);
    char *out = reap_to_string();
    ASSERT_TRUE(out != NULL);
    free(out);
    ASSERT_EQ(jobs_count(), 1);
    jobs_free_all();
    ASSERT_EQ(jobs_count(), 0);
}

TEST(free_all_twice_then_add_again) {
    jobs_init();
    pid_t a[] = {130}, b[] = {131};
    jobs_add(130, a, 1, "one");
    jobs_add(131, b, 1, "two");
    jobs_free_all();
    ASSERT_EQ(jobs_count(), 0);
    jobs_free_all();
    ASSERT_EQ(jobs_count(), 0);
    ASSERT_TRUE(jobs_current() == NULL);
    ASSERT_TRUE(jobs_by_id(1) == NULL);
    ASSERT_TRUE(jobs_by_pgid(130) == NULL);

    pid_t c[] = {132};
    Job *j = jobs_add(132, c, 1, "after");
    ASSERT_EQ(j->id, 1);
    ASSERT_EQ(jobs_count(), 1);
    ASSERT_TRUE(jobs_by_id(1) == j);
    jobs_free_all();
}

TEST(init_resets_a_populated_table) {
    jobs_init();
    pid_t a[] = {140};
    jobs_add(140, a, 1, "one");
    jobs_add(141, a, 1, "two");
    ASSERT_EQ(jobs_count(), 2);
    jobs_init();
    ASSERT_EQ(jobs_count(), 0);
    pid_t b[] = {142};
    ASSERT_EQ(jobs_add(142, b, 1, "fresh")->id, 1);
    jobs_free_all();
}

// Deterministic xorshift so a soak failure reproduces exactly.
static unsigned soak_rand(unsigned *state) {
    unsigned x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

#define SOAK_JOBS 100

typedef struct {
    pid_t pgid;
    size_t nproc;
    size_t nleft;
    size_t reported;
    JobState state;
    int live;
} Shadow;

// Void so the ASSERT_* macros can bail out; the caller checks *nsh_failed.
static void soak_verify(Shadow *sh, int *nsh_failed) {
    size_t live = 0;
    for (size_t i = 0; i < SOAK_JOBS; i++) {
        if (!sh[i].live) {
            ASSERT_TRUE(jobs_by_pgid(sh[i].pgid) == NULL);
            continue;
        }
        live++;
        Job *j = jobs_by_pgid(sh[i].pgid);
        ASSERT_TRUE(j != NULL);
        ASSERT_EQ(j->state, sh[i].state);
        ASSERT_EQ(j->nleft, sh[i].nleft);
        ASSERT_EQ(j->nproc, sh[i].nproc);
        ASSERT_TRUE(j->id > 0);
        ASSERT_TRUE(jobs_by_id(j->id) == j);
    }
    ASSERT_EQ(jobs_count(), live);

    Job *want = NULL;
    for (size_t i = SOAK_JOBS; i > 0 && want == NULL; i--) {
        if (sh[i - 1].live && sh[i - 1].state == JOB_STOPPED) {
            want = jobs_by_pgid(sh[i - 1].pgid);
        }
    }
    for (size_t i = SOAK_JOBS; i > 0 && want == NULL; i--) {
        if (sh[i - 1].live && sh[i - 1].state == JOB_RUNNING) {
            want = jobs_by_pgid(sh[i - 1].pgid);
        }
    }
    ASSERT_TRUE(jobs_current() == want);
}

TEST(soak_100_jobs_with_random_transitions) {
    if (!recipes_ok()) {
        return;
    }
    jobs_init();
    Shadow *sh = nsh_calloc(SOAK_JOBS, sizeof(*sh));
    unsigned rng = 0x9e3779b9u;

    for (size_t i = 0; i < SOAK_JOBS; i++) {
        size_t nproc = 1 + soak_rand(&rng) % 4;
        pid_t pids[4];
        pid_t base = (pid_t)(1000 + i * 8);
        for (size_t k = 0; k < nproc; k++) {
            pids[k] = base + (pid_t)k;
        }
        char line[32];
        snprintf(line, sizeof(line), "soak job %zu", i);
        Job *j = jobs_add(base, pids, nproc, line);
        ASSERT_STR_EQ(j->cmdline, line);
        sh[i].pgid = base;
        sh[i].nproc = nproc;
        sh[i].nleft = nproc;
        sh[i].reported = 0;
        sh[i].state = JOB_RUNNING;
        sh[i].live = 1;
    }
    soak_verify(sh, nsh_failed);
    if (*nsh_failed) {
        nsh_free(sh);
        return;
    }

    for (int step = 0; step < 400; step++) {
        size_t i = soak_rand(&rng) % SOAK_JOBS;
        unsigned action = soak_rand(&rng) % 8;

        if (action == 7) {
            size_t expect_lines = 0;
            for (size_t k = 0; k < SOAK_JOBS; k++) {
                if (sh[k].live && sh[k].state == JOB_DONE) {
                    expect_lines++;
                    sh[k].live = 0;
                }
            }
            char *out = reap_to_string();
            ASSERT_TRUE(out != NULL);
            size_t lines = 0;
            for (const char *p = out; *p != '\0'; p++) {
                if (*p == '\n') {
                    lines++;
                }
            }
            ASSERT_EQ(lines, expect_lines);
            free(out);
        } else if (!sh[i].live || sh[i].state == JOB_DONE) {
            continue;
        } else if (action < 2) {
            pid_t pid = sh[i].pgid + (pid_t)(soak_rand(&rng) % sh[i].nproc);
            ASSERT_TRUE(jobs_update(pid, st_stop(SIGTSTP)) != NULL);
            sh[i].state = JOB_STOPPED;
        } else if (action < 4) {
            pid_t pid = sh[i].pgid + (pid_t)(soak_rand(&rng) % sh[i].nproc);
            ASSERT_TRUE(jobs_update(pid, st_cont()) != NULL);
            sh[i].state = JOB_RUNNING;
        } else {
            pid_t pid = sh[i].pgid + (pid_t)sh[i].reported;
            int status = (soak_rand(&rng) % 2) == 0 ? st_exit(step % 128)
                                                    : st_kill(SIGKILL);
            ASSERT_TRUE(jobs_update(pid, status) != NULL);
            sh[i].reported++;
            sh[i].nleft--;
            if (sh[i].nleft == 0) {
                sh[i].state = JOB_DONE;
            }
        }

        if (step % 40 == 0) {
            soak_verify(sh, nsh_failed);
            if (*nsh_failed) {
                nsh_free(sh);
                return;
            }
        }
    }

    soak_verify(sh, nsh_failed);
    if (*nsh_failed) {
        nsh_free(sh);
        return;
    }
    char *out = reap_to_string();
    ASSERT_TRUE(out != NULL);
    free(out);
    for (size_t i = 0; i < SOAK_JOBS; i++) {
        if (sh[i].state == JOB_DONE) {
            sh[i].live = 0;
        }
    }
    soak_verify(sh, nsh_failed);
    if (*nsh_failed) {
        nsh_free(sh);
        return;
    }

    nsh_free(sh);
    jobs_free_all();
    ASSERT_EQ(jobs_count(), 0);
}

TEST_MAIN()
