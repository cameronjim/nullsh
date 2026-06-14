// Read-eval drivers: the interactive REPL and non-interactive streams.

#pragma once

#include <stdio.h>

#include "shell.h"

#include "../util/error.h"

// Prompts, line editing, history and PS2 continuation. Returns at EOF or exit.
NshError run_interactive(Shell *sh);

// Piped stdin or an opened script file: no prompts, no history, no editing.
NshError run_stream(Shell *sh, FILE *fp);
