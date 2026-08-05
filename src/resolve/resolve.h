// The resolve builtin: one DNS question in, printed answers out.

#pragma once

#include "../shell/shell.h"

int resolve_builtin(Shell *sh, int argc, char **argv);
