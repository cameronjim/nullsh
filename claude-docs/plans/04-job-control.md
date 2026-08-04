# Phase 4: Job control

## Goal

`sleep 30 &` returns the prompt immediately. Ctrl-C kills the foreground command, not the shell. Ctrl-Z stops it and `bg`/`fg` resume it. `jobs` lists everything. Finished background jobs announce themselves before the next prompt. The last phase-1 refusal disappears.

## Concepts this phase teaches

### The kernel schedules processes; the shell manages the terminal

Job control is not about CPU time. Every process already runs whenever the kernel says so. Job control decides one thing: WHICH group of processes owns the terminal right now, meaning who receives keyboard input and keyboard signals.

### Process groups

Every process belongs to a process group (pgid). A pipeline becomes ONE group: the first child's pid becomes the pgid and every stage joins it via `setpgid`. Signals can then target the whole pipeline at once with `kill(-pgid, sig)`.

### The foreground group and why Ctrl-C spares the shell

A terminal has exactly one foreground process group, set with `tcsetpgrp`. When you press Ctrl-C the kernel sends SIGINT to every process in THAT group and nobody else. The shell puts the pipeline in the foreground and stays in its own separate group, so the keyboard kills the command while the shell survives. Ctrl-Z is the same story with SIGTSTP, which stops (suspends) instead of killing.

### The handoff dance

Run a foreground job: shell tcsetpgrps the job's group onto the terminal, waits, then takes the terminal back (`tcsetpgrp` to its own pgid). Taking it back happens even when the job was stopped rather than exited. A background process that tries to READ the terminal gets SIGTTIN and stops; one that writes may get SIGTTOU depending on terminal settings, which is why the shell itself must ignore SIGTTOU while juggling tcsetpgrp.

### SIGCHLD and async-signal-safety

The kernel raises SIGCHLD in the shell whenever a child exits, stops, or resumes. Almost nothing is legal inside a signal handler (no printf, no malloc, no data structure walks), so the handler does exactly one thing: set a `volatile sig_atomic_t` flag. Before each prompt the REPL checks the flag and reaps with `waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED)` in a loop: WNOHANG never blocks, WUNTRACED reports stops, WCONTINUED reports resumes. Reaping updates the job table and prints "[1]  Done  sleep 30" lines. Unreaped children become zombies; the reap loop is the garbage collector.

### The setpgid race

After fork, parent and child race. If the parent tcsetpgrps the group before the child setpgids into it, signals go astray; if the child execs first, setpgid can fail. Standard fix: BOTH parent and child call `setpgid(pid, pgid)`; one wins harmlessly (EACCES after exec is expected for the parent's late call and is ignored).

### fg and bg are tiny once the model is right

`fg %1`: tcsetpgrp the job's group, `kill(-pgid, SIGCONT)`, blocking wait as if just launched. `bg %1`: SIGCONT without the terminal handoff. That is all they are.

### Interactive vs not

All of this only makes sense on a terminal. When stdin is not a tty (scripts, tests, pipes) the shell skips tcsetpgrp and signal juggling and behaves as before. `interactive` is decided once at startup with `isatty(0)`, and job control still tracks background jobs either way (& works in scripts; only terminal handoff is tty-gated).

## Contracts (written by Fable, committed with this plan)

- `shell.h` grows: `bool interactive; int tty_fd; pid_t shell_pgid;`
- `src/shell/jobs.h`: job table API (the one sanctioned global). JobState RUNNING/STOPPED/DONE; Job carries id, pgid, state, nsh-allocated cmdline, pid array, per-job status of the last pid to report. Functions: jobs_init/jobs_free_all, jobs_add(pgid, pids, nproc, cmdline), jobs_by_id, jobs_by_pgid, jobs_current (most recent stopped, else most recent running), jobs_update(pid, wstatus) mapping a waitpid result onto the owning job, jobs_reap_notify(FILE*) printing Done lines and pruning, jobs_count.
- `src/shell/signals.h`: signals_install_shell (ignore SIGINT/SIGQUIT/SIGTSTP/SIGTTOU, SIGCHLD handler sets the flag), signals_reset_child (defaults restored after fork), signals_chld_take (read-and-clear flag).

## Execution waves

- Wave 1, parallel clones: jobs.c + jobs_test.c (pure data structure, fully unit-testable); signals.c + signals_test.c (install, raise, flag observed; reset-in-child via fork).
- Wave 2, single agent in main repo: exec.c places every pipeline in its own process group (single foreground commands too), foreground wait via tcsetpgrp handoff, `&` path registers the job and skips the wait; builtins fg/bg/jobs; main.c installs shell signals, records interactive/tty_fd/shell_pgid, checks the SIGCHLD flag before each prompt; the "job control arrives in phase 4" refusal dies. Integration tests.

## Testing strategy

Unit: the job table exhaustively (add/lookup/current-selection/update transitions/notify pruning); signals module via raise() and fork.
Integration without a tty (always runs): `sleep 0.2 &` returns immediately (time the prompt round trip), `jobs` shows it running, the Done line appears after it finishes, `fg` on a running background job waits for it, `bg`/`fg` on bogus ids error cleanly, & jobs from scripts get reaped (no zombies: check /proc for defunct children of the shell).
Integration with a pty (best effort): drive the shell under `script -qec`, deliver SIGTSTP/SIGINT to the foreground child's group via kill from outside, assert stopped-then-fg-resumes and that Ctrl-C kills the child but not the shell. If pty scripting proves too flaky in CI, keep these as a separate best-effort script that reports SKIP rather than failing the suite, and the behavior is verified manually by Fable at phase close.

## Exit criteria

Suite green under both strategies with no hangs; manual tty session verified by Fable (Ctrl-C, Ctrl-Z, fg, bg, jobs, background netcat-free equivalents); architecture.md and notes.md updated; lowercase tldr commit under CJ's name only.
