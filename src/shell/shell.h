// The one Shell struct: state that must live in the shell process itself.
// Builtins mutate it; the REPL owns it; nothing here is a global.

#pragma once

#include <stdbool.h>

#include "history.h"

typedef struct {
    History history;
    int last_status;  // feeds $? in expansion
    bool want_exit;   // set by the exit builtin; the REPL checks it each loop
    int exit_code;    // valid once want_exit is true
} Shell;
