// Builtin command dispatch, for commands that mutate the shell process itself.

#pragma once

#include "shell.h"

// argv is main-style: argv[0] is the builtin name and argv[argc] is NULL.
typedef int (*BuiltinFn)(Shell *sh, int argc, char **argv);

// NULL when name is not a builtin.
BuiltinFn builtin_lookup(const char *name);
