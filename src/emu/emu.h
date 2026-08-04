// The emu builtin: load a CHIP-8 rom and run it, on the terminal or headless.

#pragma once

#include "../shell/shell.h"

int emu_builtin(Shell *sh, int argc, char **argv);
