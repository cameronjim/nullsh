// Builtin command dispatch. Commands that must mutate the shell process
// itself, cwd, environment, history and exit, run through this table instead
// of fork and exec.

#pragma once

#include "shell.h"

// A builtin receives argv exactly like a program's main and returns its exit
// status. argv[0] is the builtin name; argv[argc] is NULL.
typedef int (*BuiltinFn)(Shell *sh, int argc, char **argv);

// Returns the builtin with this name, or NULL if name is not a builtin.
BuiltinFn builtin_lookup(const char *name);
