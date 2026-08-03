// The heap builtin: the allocator's internals exposed at the prompt.

#pragma once

#include "../shell/shell.h"

int heap_builtin(Shell *sh, int argc, char **argv);
