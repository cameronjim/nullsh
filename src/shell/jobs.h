// The job table: background and stopped pipelines. The one sanctioned global besides signal flags.

#pragma once

#include <stdio.h>
#include <sys/types.h>

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } JobState;

typedef struct {
    int id;          // the [N] the user sees, 1-based, reused after pruning
    pid_t pgid;
    JobState state;
    char *cmdline;   // nsh-allocated copy of the typed line
    pid_t *pids;     // nsh-allocated, live processes in the group
    size_t nproc;    // entries in pids
    size_t nleft;    // pids not yet reported done
    int last_status; // wait status of the most recent pid to report
} Job;

void jobs_init(void);
// Frees every job; safe to call twice.
void jobs_free_all(void);
// Copies cmdline and pids; returns the new job, never NULL (nsh_* aborts on OOM).
Job *jobs_add(pid_t pgid, const pid_t *pids, size_t nproc, const char *cmdline);
Job *jobs_by_id(int id);
Job *jobs_by_pgid(pid_t pgid);
// Most recently added stopped job, else most recently added running job, else NULL.
Job *jobs_current(void);
// Maps one waitpid result onto its job: exit/signal marks the pid done, stop and
// continue flip the whole job's state. Returns the job, NULL for an unknown pid.
Job *jobs_update(pid_t pid, int wstatus);
// Prints "[id]  Done  cmdline" for every finished job, then prunes them.
void jobs_reap_notify(FILE *out);
size_t jobs_count(void);
