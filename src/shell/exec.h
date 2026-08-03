// Execution: a parsed Pipeline in, a process run and an exit status out.
// Builtins run in the shell itself; everything else is fork, PATH search,
// execve, wait.

#pragma once

#include "shell.h"
#include "parser.h"

#include "../util/error.h"

// Runs a parsed pipeline and sets sh->last_status. Borrows pl (caller frees).
// Phase 1 limits: exactly one command, no redirects, no background. Anything
// beyond that prints one clear stderr line naming the phase that adds it and
// sets status 1 (this is NOT an NshError; the line was valid shell syntax).
NshError exec_pipeline(Shell *sh, Pipeline *pl);
