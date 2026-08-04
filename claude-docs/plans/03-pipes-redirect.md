# Phase 3: Pipes and redirection

## Goal

`cmd1 | cmd2 | cmd3` chains of arbitrary length, plus `>`, `>>`, `<`, `2>`, working for external commands AND builtins. After this phase `history | grep echo` and `heap dump > map.txt` both work. The parser already produces the structures; this phase makes exec honor them.

## Concepts this phase teaches

### A pipe is a kernel buffer with two file descriptors

`pipe(fd)` gives back fd[0] (read end) and fd[1] (write end) of a byte queue living in the kernel. Whatever is written to fd[1] comes out of fd[0]. That is the entire mechanism; everything else is plumbing the right fds to the right processes.

### fd inheritance is the whole trick

A child made by fork() shares copies of every open fd. So the shell creates the pipe BEFORE forking, and each child rearranges its own fds with `dup2(oldfd, newfd)`: the writer dup2s the write end onto 1 (stdout), the reader dup2s the read end onto 0 (stdin), then both exec. The exec'd program never knows; it just reads 0 and writes 1 like always. This is why pipes compose with any program ever written.

### The close discipline, or why pipelines hang

A reader sees EOF only when EVERY copy of the write end in EVERY process is closed. After fork, the pipe ends exist in the parent and all children. So: each child closes every pipe fd it does not use (after dup2ing the ones it does), and the parent closes both ends of every pipe once all children are forked. One forgotten fd and `grep` waits forever for an EOF that cannot come. This is the classic pipeline bug and the tests target it with a many-stage pipeline.

### SIGPIPE, the other direction

If the reader exits early (think `head -1`), the writer's next write gets SIGPIPE and dies. That is normal Unix flow control: `seq 100000 | head -1` kills seq the moment head leaves. The shell just reports statuses; children keep the default SIGPIPE disposition.

### Redirection is open + dup2

`> f` is `open(f, O_WRONLY|O_CREAT|O_TRUNC, 0666)` then `dup2(fd, 1)`. `>>` swaps O_TRUNC for O_APPEND. `<` is O_RDONLY onto 0. `2>` is the `>` recipe onto 2. Done in the child, so the parent shell's own fds never move. File redirects apply AFTER pipe wiring, so an explicit `> f` in the middle of a pipeline beats the pipe, same as bash.

### Builtins need two paths

- A builtin alone in a pipeline with redirects (`history > f`) must run IN the parent (its side effects, like cd changing the cwd, must land in the shell). So the parent saves its own 0/1/2 with dup, applies the redirects, runs the builtin, restores. `cd /tmp > log` still changes the shell's directory; the tests prove it.
- A builtin inside a multi-command pipeline (`history | grep x`) runs in a forked child like any other stage; its side effects die with the child, which matches bash. The child runs the builtin function and _exits with its status.

### Pipeline status

`$?` after a pipeline is the LAST command's exit status (bash default, no pipefail). `false | true` is 0; `true | false` is 1.

## Structure

- New `src/shell/redirect.c/h`: expands the redirect target words (via expand_word) and applies them; a save/restore pair for the in-parent builtin path. Suggested shape: `redirect_apply(const Command*, int last_status, RedirSave*)` where a NULL save means child mode (print error, return code), and `redirect_restore(RedirSave*)`.
- `src/shell/exec.c` grows the pipeline walk: N-1 pipe() calls, N forks; child i wires read end i-1 onto 0 and write end i onto 1, closes all pipe fds, applies file redirects, then runs (execve or builtin+_exit). Parent closes everything and waitpids all children, keeping the last one's status. Phase 1's "pipes arrive in phase 3" refusal disappears; only the background refusal remains (phase 4).
- Open failures on a redirect target: message to stderr, status 1, command not run (bash behavior), shell continues.

## Tests

exec unit tests through the real lexer/parser: two- and three-stage pipeline statuses, last-command-wins (`false | true`, `true | false`), redirect target from a variable, open-failure path leaves status 1 and shell state intact.

Integration (new 03_pipes.sh): echo through cat; sort a printf; 3-stage pipeline; `2>` captures stderr and stdout stays clean; `>` truncates where `>>` appends; `<` feeds a file in; `history > f` runs in the parent (file lands AND a following builtin still sees shell state); `cd /tmp > /dev/null` still changes the cwd; `history | grep` (builtin as pipeline stage); unknown command mid-pipeline (shell survives, last status from last stage); `seq 100000 | head -1` (early reader exit, no hang, output correct); a 6-stage pipeline (close-discipline stress); redirect into an unwritable path errors without running the command.

## Exit criteria

All of the above green under both allocator strategies, ASan/UBSan clean, no hangs (the suite itself is the timeout canary), architecture.md updated, notes.md gains a phase 3 section.
