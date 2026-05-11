// Process groups, the terminal handoff, and the foreground wait over a group.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "shell.h"

// Called by BOTH parent and child after fork; the loser's failure is expected.
void spawn_join_group(pid_t pid, pid_t pgid);

// Interactive only: gives the terminal to pgid, or hands it back to the shell.
void spawn_set_terminal(const Shell *sh, pid_t pgid);

// Waits for every pid with WUNTRACED, skipping ones already reaped elsewhere.
// Sets *stopped when any stage stopped. Returns the last stage's shell status.
int spawn_wait_pids(const pid_t *pids, size_t n, bool *stopped);
