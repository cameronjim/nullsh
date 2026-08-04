# Phase 8: Polish and ship

## Goal

The repo stops being a pile of working phases and becomes a portfolio piece someone can clone, build, and understand in ten minutes. Plus the two quality-of-life gaps the earlier phases deliberately deferred.

## Work items

### 1. Line editing (the biggest remaining rough edge)

Arrow keys currently emit escape sequences into the line. Add raw-mode line editing reusing the terminal machinery emu already proved: left/right, Home/End, backspace/delete, Up/Down walking history, Ctrl-A/E/K/U/W. Only when interactive; the non-interactive path keeps using line_read unchanged so every existing test stays valid. New module src/shell/edit.c/h; history integration through the existing History API. Terminal restoration on every path, including SIGTSTP mid-edit.

### 2. Tilde expansion

`cd ~/code` and `inspect ~/bin/thing` should work. A leading `~` in a bare (unquoted, expandable) word becomes $HOME; `~/` prefix only, no `~user` form. Lives in expand.c next to the $VAR logic, gated to the first character of a word.

### 3. README that earns the repo

Replace the stub with: what nullsh is in three sentences, a demo transcript that actually runs (every line verified by a script, not aspirational), build and test instructions, a feature list per subsystem with one-line explanations of the concept each teaches, the code rules, and the layout. No em dashes, no "utilize", direct prose. Include the honest limitations list (no && or ||, no globbing, no subshells, no scripting constructs, single-quote-free tilde only, 64-bit LE ELF only, IPv4 only).

### 4. Docs pass

architecture.md: final module map and the full decisions list, current. testing.md: document the dual-strategy runs, the sudo-gated make test-net, the pty tests, and the mutation-testing practice the agents used. code-style.md: unchanged if still accurate.

### 5. Repo hygiene

`make` from a clean clone must work with zero warnings; verify with a fresh `git clone` into /tmp and a build there. Confirm no file exceeds 500 non-test lines (a script in the report). Confirm no `/* */` comments anywhere. Confirm every source file opens with its one-line module comment. Fix anything that drifted.

### 6. GitHub

Requires CJ to authenticate once (`gh auth login` or an SSH key). Then: create the public repo `nullsh`, push main, confirm the description and topics. This is the one step the loop cannot do unattended; the phase closes with it queued and clearly flagged rather than blocked on it.

## Testing

Line editing: unit-test the edit buffer logic (insert/delete/word-kill/history cursor) as a pure module with no terminal; the key-decoding table (escape sequences to actions) as pure input-to-action mapping; a pty integration script driving real arrow keys and asserting the resulting line. Tilde: expand unit tests plus an integration case (`cd ~` then `pwd`). README demo: a script that executes every command in the transcript and diffs the output, so the README cannot rot.

## Exit criteria

Dual-pass suite green including the new tests, fresh-clone build clean, README demo script passes, docs current, lowercase tldr commits, GitHub push either done or queued with instructions for CJ.
