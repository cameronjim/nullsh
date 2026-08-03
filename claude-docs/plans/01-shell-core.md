# Phase 1: Shell core

## Goal

An interactive shell that reads a line, handles quotes and `$VAR` expansion, runs external programs found on PATH, and has the standard builtins. No pipes, no redirects, no job control yet, but the lexer already tokenizes their operators so Phases 3 and 4 extend rather than rewrite.

## Concepts this phase teaches

### The REPL

A shell is a loop: print prompt, read line, tokenize, parse, expand, execute, repeat. Every feature after this phase is a refinement of one of those steps.

### fork, exec, wait

Unix creates processes in two separate steps:
- `fork()` clones the calling process. Both copies continue from the same point; the return value (0 in the child, child pid in the parent) tells them apart.
- `execve(path, argv, envp)` replaces the calling process image with a new program. It never returns on success. The pid, open file descriptors, and environment survive; the code and memory are replaced.
- The parent calls `waitpid(pid, &status, 0)` to block until the child exits and to collect its exit status. Skipping this leaks zombie processes: dead children whose status nobody read.

This split is why shells are simple to write: fork, do per-child setup (later: redirects, process group), exec, and the parent just waits.

### Why builtins exist

`cd` cannot be an external program. If the shell forked a child that called `chdir()`, the child's working directory would change and then the child would exit. The shell's own cwd would be untouched. Anything that must mutate the shell process itself (cwd, environment, history, exit) has to run inside the shell. Hence the builtin dispatch check before forking.

### PATH resolution

`ls` means "search each directory in `$PATH`, in order, for an executable file named ls". Implementation: split PATH on `:`, build `dir/name`, try `execve` on each (in the already forked child), treating ENOENT as "keep looking". If a candidate exists but is not executable, remember EACCES so the final error is "permission denied" (exit 126) rather than "not found" (exit 127). A name containing `/` skips the search entirely.

### Exit status

`$?` holds the last command's status. Conventions: 0 success, 1-125 program-defined failure, 126 found but not executable, 127 not found, 128+N killed by signal N. Extracted with `WIFEXITED`/`WEXITSTATUS` and `WIFSIGNALED`/`WTERMSIG`.

### Quoting

Three lexer modes:
- Bare word: whitespace splits, `$` expands, `\x` escapes x.
- Single quotes: everything literal until the closing quote. No expansion, no escapes.
- Double quotes: whitespace does not split and `$` still expands. `\` escapes only `$`, `` ` ``, `"`, `\`.
An unterminated quote is a parse error and a clear message, never a crash.

## Module breakdown

Each file ships with its `*_test.c`. Dependency order matters for the agent fan-out.

Wave 1 (independent, parallel):
- `util/str.c/h`: growable string (`Str`), push char, push cstr, take ownership. On `nsh_malloc`.
- `util/vec.c/h`: growable pointer array (`Vec`). Used for token lists and argv.
- `util/error.h`: `NshError` enum (NSH_OK, NSH_ERR_ALLOC, NSH_ERR_SYNTAX, NSH_ERR_IO, ...) plus `nsh_error_str()`.
- `shell/history.c/h`: fixed-capacity ring of lines, append, get by index, load/save `~/.nullsh_history`.

Wave 2 (needs util):
- `shell/lexer.c/h`: line to `TokenList`. Token kinds: WORD, PIPE, REDIR_IN, REDIR_OUT, REDIR_APPEND, REDIR_ERR, AMP, EOL. Words carry per-character "was quoted" info or are stored post-quote-processing with expansion markers, decided as follows: the lexer strips quotes and records for each resulting word segment whether `$` expansion is allowed. State machine over the input, one pass.
- `shell/expand.c/h`: takes lexed words, expands `$NAME`, `${NAME}`, `$?` in expandable segments using `getenv` and last exit status. Undefined variable expands to empty string.

Wave 3 (needs lexer/expand):
- `shell/parser.c/h`: `TokenList` to `Pipeline` struct: array of `Command` (argv vector, redirect filename slots kept but unused until Phase 3), background flag. Syntax errors (empty command between pipes, redirect without target) return NSH_ERR_SYNTAX with a message.

Wave 4 (needs parser):
- `shell/builtin.c/h`: dispatch table `{name, fn}`; cd (with `cd` alone meaning `$HOME`, `cd -` meaning `$OLDPWD`), exit, help, export (NAME=VALUE), unset, history.
- `shell/exec.c/h`: if single command and builtin, run in-process; else fork, child execs via PATH search, parent waits and records `$?`.
- `main.c`: REPL loop, prompt showing cwd, EOF (Ctrl-D) exits cleanly, empty line loops.

## Tests

- str/vec: growth across many appends, ownership, zero-length.
- lexer: `echo hi`, `echo "a b"`, `echo 'a $HOME'`, `echo a\ b`, `echo "unclosed`, mixed `"a"'b'c`, operators `a|b>c`, empty line.
- expand: `$HOME`, `${HOME}`, `$?` after success and failure, undefined var, `$` at end of line, no expansion in single quotes.
- parser: single command, redirect slot capture, `|` splitting, syntax errors.
- builtins: cd changes cwd and sets OLDPWD, export then getenv, unset, history returns appended lines.
- integration: `echo hi` prints hi; `nosuchcmd` prints error and `$?` is 127; `export X=1` then `echo $X` prints 1; quoted args reach `/bin/echo` unsplit.

## Exit criteria

Interactive prompt runs real programs with quoting and expansion, all six builtins work, history persists across runs, `make test` green under ASan and UBSan.
