// The netmon builtin: capture an interface and print one line per packet.

#pragma once

#include "../shell/shell.h"

int netmon_builtin(Shell *sh, int argc, char **argv);
