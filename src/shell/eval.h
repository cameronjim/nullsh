// Evaluator: walks the AST, runs pipelines through exec, owns control flow.

#pragma once

#include <stdbool.h>

#include "ast.h"
#include "shell.h"

#include "../util/error.h"

// Runs one parsed program; borrows n and sets sh->last_status.
NshError eval_run(Shell *sh, Node *n);

// Called by exec for a command word that may name a function. Returns false
// when argv[0] is no function; otherwise runs the body and writes its status.
bool eval_maybe_call_function(Shell *sh, int argc, char **argv, int *status);
