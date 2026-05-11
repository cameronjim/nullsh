// Execution: a parsed Pipeline in, a process run and an exit status out.

#pragma once

#include "jobs.h"
#include "shell.h"
#include "parser.h"

#include "../util/error.h"

// Sets sh->last_status and borrows pl. A trailing & registers a job instead
// of waiting.
NshError exec_pipeline(Shell *sh, Pipeline *pl);

// Runs an existing job in the foreground: terminal handoff, wait, handoff back.
// The job ends up JOB_STOPPED or JOB_DONE. Returns the job's shell status.
int exec_wait_foreground(Shell *sh, Job *job);
