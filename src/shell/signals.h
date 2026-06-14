// Signal setup for the shell and its children, plus the SIGCHLD flag.

#pragma once

// Shell process: ignore SIGINT, SIGQUIT, SIGTSTP, SIGTTOU; SIGCHLD handler sets the flag.
void signals_install_shell(void);
// Child after fork: every disposition above back to SIG_DFL.
void signals_reset_child(void);
// Returns nonzero if SIGCHLD fired since the last call, clearing the flag.
int signals_chld_take(void);
// While watching (on nonzero), SIGINT sets a flag instead of being ignored;
// off restores SIG_IGN. eval turns this on around interpreter loops.
void signals_int_watch(int on);
// Returns nonzero if SIGINT fired since the last call, clearing the flag.
int signals_int_take(void);
