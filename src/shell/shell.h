// The Shell struct: state that must live in the shell process itself.

#pragma once

#include <stdbool.h>
#include <sys/types.h>

#include "history.h"

// How break, continue and return unwind the evaluator's recursion.
typedef enum { FLOW_NONE, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN } FlowState;

typedef struct {
    History history;
    int last_status;   // feeds $? in expansion
    bool want_exit;    // set by the exit builtin, checked by the REPL
    int exit_code;     // valid once want_exit is true
    bool interactive;  // isatty(0) at startup; gates terminal handoff
    int tty_fd;        // the controlling terminal when interactive
    pid_t shell_pgid;  // the shell's own process group
    FlowState flow;    // set by the flow builtins, consumed by eval
    int flow_status;   // return's argument, applied when FLOW_RETURN lands
    int loop_depth;    // maintained by eval; gates break and continue
    int func_depth;    // maintained by eval; gates return, caps recursion
    int argc;          // positional parameters; argv[0] feeds $0
    char **argv;       // borrowed from main or the active function call
} Shell;
