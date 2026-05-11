// Signal setup for the shell and its children, plus the SIGCHLD flag.

#pragma once

// Shell process: ignore SIGINT, SIGQUIT, SIGTSTP, SIGTTOU; SIGCHLD handler sets the flag.
void signals_install_shell(void);
// Child after fork: every disposition above back to SIG_DFL.
void signals_reset_child(void);
// Returns nonzero if SIGCHLD fired since the last call, clearing the flag.
int signals_chld_take(void);
