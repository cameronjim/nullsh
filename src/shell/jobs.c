// The job table: pure bookkeeping over background and stopped pipelines.

// WIFCONTINUED is XOPEN2K8, not plain C17.
#define _POSIX_C_SOURCE 200809L

#include "jobs.h"

#include <string.h>
#include <sys/wait.h>

#include "../alloc/alloc.h"

static Job **table;
static size_t table_len;
static size_t table_cap;

// strdup would use libc malloc, which is off limits outside src/alloc.
static char *dup_cstr(const char *s) {
    size_t len = s == NULL ? 0 : strlen(s);
    char *copy = nsh_malloc(len + 1);
    if (len > 0) {
        memcpy(copy, s, len);
    }
    copy[len] = '\0';
    return copy;
}

static void job_free(Job *j) {
    nsh_free(j->cmdline);
    nsh_free(j->pids);
    nsh_free(j);
}

static void table_push(Job *j) {
    if (table_len == table_cap) {
        table_cap = table_cap == 0 ? 8 : table_cap * 2;
        table = nsh_realloc(table, table_cap * sizeof(*table));
    }
    table[table_len++] = j;
}

// Keeps the remaining jobs in insertion order, which jobs_current depends on.
static void table_remove(size_t i) {
    for (size_t k = i + 1; k < table_len; k++) {
        table[k - 1] = table[k];
    }
    table_len--;
}

static int next_id(void) {
    for (int id = 1;; id++) {
        if (jobs_by_id(id) == NULL) {
            return id;
        }
    }
}

static Job *job_of_pid(pid_t pid) {
    for (size_t i = 0; i < table_len; i++) {
        for (size_t k = 0; k < table[i]->nproc; k++) {
            if (table[i]->pids[k] == pid) {
                return table[i];
            }
        }
    }
    return NULL;
}

void jobs_init(void) {
    jobs_free_all();
}

void jobs_free_all(void) {
    for (size_t i = 0; i < table_len; i++) {
        job_free(table[i]);
    }
    nsh_free(table);
    table = NULL;
    table_len = 0;
    table_cap = 0;
}

Job *jobs_add(pid_t pgid, const pid_t *pids, size_t nproc, const char *cmdline) {
    if (pids == NULL) {
        nproc = 0;
    }
    Job *j = nsh_malloc(sizeof(*j));
    j->id = next_id();
    j->pgid = pgid;
    j->state = JOB_RUNNING;
    j->cmdline = dup_cstr(cmdline);
    j->pids = nsh_calloc(nproc, sizeof(*j->pids));
    if (nproc > 0) {
        memcpy(j->pids, pids, nproc * sizeof(*j->pids));
    }
    j->nproc = nproc;
    j->nleft = nproc;
    j->last_status = 0;
    table_push(j);
    return j;
}

Job *jobs_by_id(int id) {
    for (size_t i = 0; i < table_len; i++) {
        if (table[i]->id == id) {
            return table[i];
        }
    }
    return NULL;
}

Job *jobs_by_pgid(pid_t pgid) {
    for (size_t i = 0; i < table_len; i++) {
        if (table[i]->pgid == pgid) {
            return table[i];
        }
    }
    return NULL;
}

Job *jobs_current(void) {
    for (size_t i = table_len; i > 0; i--) {
        if (table[i - 1]->state == JOB_STOPPED) {
            return table[i - 1];
        }
    }
    for (size_t i = table_len; i > 0; i--) {
        if (table[i - 1]->state == JOB_RUNNING) {
            return table[i - 1];
        }
    }
    return NULL;
}

Job *jobs_update(pid_t pid, int wstatus) {
    Job *j = job_of_pid(pid);
    if (j == NULL) {
        return NULL;
    }
    // Only a terminal status counts as a report, so only it moves nleft.
    if (WIFEXITED(wstatus) || WIFSIGNALED(wstatus)) {
        j->last_status = wstatus;
        if (j->nleft > 0 && --j->nleft == 0) {
            j->state = JOB_DONE;
        }
    } else if (WIFSTOPPED(wstatus)) {
        j->state = JOB_STOPPED;
    } else if (WIFCONTINUED(wstatus)) {
        j->state = JOB_RUNNING;
    }
    return j;
}

void jobs_reap_notify(FILE *out) {
    if (out == NULL) {
        return;
    }
    // Lowest id first, which is not table order once an id has been reused.
    for (;;) {
        size_t pick = table_len;
        for (size_t i = 0; i < table_len; i++) {
            if (table[i]->state == JOB_DONE &&
                (pick == table_len || table[i]->id < table[pick]->id)) {
                pick = i;
            }
        }
        if (pick == table_len) {
            return;
        }
        fprintf(out, "[%d]  Done  %s\n", table[pick]->id, table[pick]->cmdline);
        job_free(table[pick]);
        table_remove(pick);
    }
}

// Every job present, finished ones included until jobs_reap_notify prunes them.
size_t jobs_count(void) {
    return table_len;
}
