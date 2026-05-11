// Execution: a parsed Pipeline in, a process run and an exit status out.

#pragma once

#include "shell.h"
#include "parser.h"

#include "../util/error.h"

// Sets sh->last_status and borrows pl. Background jobs wait for phase 4.
NshError exec_pipeline(Shell *sh, Pipeline *pl);
