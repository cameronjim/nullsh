// The Shell struct: state that must live in the shell process itself.

#pragma once

#include <stdbool.h>

#include "history.h"

typedef struct {
    History history;
    int last_status;  // feeds $? in expansion
    bool want_exit;   // set by the exit builtin, checked by the REPL
    int exit_code;    // valid once want_exit is true
} Shell;
