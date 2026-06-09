// The inspect builtin: open an ELF file and print the view the flags asked for.

#pragma once

#include "../shell/shell.h"

int inspect_builtin(Shell *sh, int argc, char **argv);
