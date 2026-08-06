# nullsh manual

Everything nullsh understands, in tables. Each row is a piece of syntax, what
it does, and a short example. Nothing here is aspirational: if a feature is not
in this manual, nullsh does not have it, and the last section lists the
omissions on purpose.

For what nullsh is and why each subsystem exists, read `README.md`. For how the
code is put together, read `claude-docs/architecture.md`.

## Contents

1. [Invocation](#invocation)
2. [Command line editing](#command-line-editing)
3. [Basic syntax](#basic-syntax)
4. [Expansion](#expansion)
5. [Pipes and redirection](#pipes-and-redirection)
6. [The interpreter](#the-interpreter)
7. [Builtins](#builtins)
8. [Job control](#job-control)
9. [Limitations](#limitations)

## Invocation

nullsh reads commands from one of three places. Which one it uses is decided
once at startup, by `isatty(0)` and by whether an argument was given.

| Form | What it does |
|---|---|
| `nullsh` on a terminal | interactive: prompt, line editing, history file, job control handoff |
| `nullsh < file`, `cmd \| nullsh` | reads stdin to the end: no prompts, no editing; lines still land in the history file |
| `nullsh PATH` | runs the script at PATH |
| `nullsh PATH ARG...` | runs the script with `$1`..`$9` set from ARG... |

The interactive prompt is the working directory with `$HOME` collapsed to a
tilde, then `$ `:

```
nullsh:~$
nullsh:~/code/nullsh$
nullsh:/etc$
```

Script mode details:

| Detail | Behavior |
|---|---|
| `$0` | the PATH as it was written on the command line |
| `$1`..`$9`, `$#` | the arguments after PATH, and how many there are |
| `#!/usr/bin/env nullsh` on line 1 | nothing special: it is an ordinary comment |
| PATH cannot be opened | `nullsh: PATH: No such file or directory` on stderr, exit 127 |
| exit status | the status of the last command the script ran |
| prompts, line editing, history | none of them: a script never touches the history file |

Exit statuses nullsh itself produces:

| Status | Meaning |
|---|---|
| 0 | the last command succeeded |
| 2 | syntax error, bad expansion, or `exit` with a non-numeric argument |
| 126 | the command was found but could not be run (permission denied, or not an executable format) |
| 127 | command not found, or the script file could not be opened |
| 128+N | the command was killed by signal N |
| 130 | interrupted, which is 128 plus SIGINT |

`exit [N]` sets the shell's own status. `exit` with no argument leaves with the
last command's status, and so does end of input (Ctrl-D). A status is one byte
on the wire, so `exit 256` leaves with 0 and `exit -1` leaves with 255.

## Command line editing

Editing is active only when stdin is a terminal. The line is repainted on every
keystroke, so the display can never drift out of sync with the buffer.

| Key | Action |
|---|---|
| printable character | insert at the cursor |
| Left, Right | move the cursor one character |
| Home, Ctrl-A | move to the start of the line |
| End, Ctrl-E | move to the end of the line |
| Backspace | delete the character before the cursor |
| Delete | delete the character under the cursor |
| Ctrl-K | delete from the cursor to the end of the line |
| Ctrl-U | delete from the start of the line to the cursor |
| Ctrl-W | delete the word before the cursor (blanks, then non-blanks) |
| Up | recall the previous history entry |
| Down | recall the next one, ending at the line you were typing |
| Enter | submit the line |
| Ctrl-C | throw the line away, print a fresh prompt, run nothing |
| Ctrl-D on an empty line | end of input: the shell exits |
| Ctrl-D on a line with text | delete the character under the cursor |

The first Up stashes whatever you were typing and Down walks back to it. Edits
made to a recalled entry are dropped by the next recall, and the history entry
itself is never rewritten.

History file behavior:

| Detail | Behavior |
|---|---|
| File | `~/.nullsh_history`, one command per line, oldest first |
| Load | at startup; a missing file is normal, not an error |
| Save | on exit, including exit by Ctrl-D |
| Capacity | 1000 entries, oldest dropped first |
| What is recorded | the raw line, before lexing, so a line that failed to parse is still recallable |
| Duplicates | an immediate repeat of the newest entry is dropped; the same line later is a new entry |
| Multi-line input | every physical line is recorded separately, continuation lines included |
| `HOME` unset | no file is loaded or saved; the in-memory ring still works |

## Basic syntax

A command is a list of words separated by blanks (space and tab). Quoting
decides which characters inside a word stay literal.

| Syntax | Meaning | Example |
|---|---|---|
| `word` | a bare word; blanks end it | `echo a b` passes two arguments |
| `'text'` | literal: nothing inside is special, not even a backslash | `echo 'a$b\c'` prints `a$b\c` |
| `"text"` | expansion still happens; four escapes are recognised | `echo "$HOME/bin"` |
| `\c` | outside quotes, the next character is literal | `echo a\ b` passes one argument |
| `""` or `''` | an empty word, which is still an argument | `grep "" file` |
| `"a"'b'c` | one word made of three quoting runs | expansion applies per run |
| `#` | starts a comment when it opens a word; runs to the newline or end of input | `echo hi # a note` |
| `a#b` | not a comment: the `#` is inside a word | one word, `a#b` |

The four escapes recognised inside double quotes:

| Escape | Produces |
|---|---|
| `\$` | a literal `$` |
| `\"` | a literal `"` |
| `\\` | a literal `\` |
| ``\` `` | a literal backtick |

Any other backslash inside double quotes is two literal characters: `"a\nb"` is
`a\nb`, not a newline.

A backslash at the very end of the input is a literal backslash. nullsh has no
line continuation.

An unterminated `'` or `"` is not an error. Interactively the shell prompts for
more input and the quote spans lines with a real newline inside it. See
[multi-line input](#multi-line-input).

## Expansion

Expansion happens when a command runs, not when it is parsed, and only inside
bare words and double-quoted runs.

| Syntax | Expands to | Example |
|---|---|---|
| `$NAME` | the environment variable NAME, empty if unset | `echo $HOME` |
| `${NAME}` | the same, with an explicit end so text can follow | `echo ${HOME}bar` |
| `$?` | the exit status of the last command | `false; echo $?` prints `1` |
| `$0` | the script path in script mode, otherwise how nullsh was invoked | `echo $0` |
| `$1` .. `$9` | positional parameters; out of range is empty | `echo $1` |
| `${0}` .. `${9}` | the same, braced | `echo ${1}x` |
| `$#` | how many positional parameters there are, never negative | `echo $#` |
| `~` | `$HOME`, only as the whole first component of a word | `cd ~` |
| `~/path` | `$HOME/path` | `inspect ~/bin/thing` |

Rules the table cannot show:

- A name is `[A-Za-z_][A-Za-z0-9_]*` and the longest run wins. `$FOObar` reads
  the variable `FOObar`, so use `${FOO}bar` when you mean concatenation.
- Only one digit is a positional parameter. `$10` is `$1` followed by a literal
  `0`, and `${10}` is a bad substitution like any other multi-character `${N}`.
- A `$` that starts none of the forms above is an ordinary character. `echo $`
  and `echo 100$` both print what you typed.
- `${` with no name or no closing `}` is a bad substitution: `nullsh: bad
  substitution` on stderr, status 2, and the command does not run.
- The tilde is recognised only in a word's first quoting run, and only when the
  next character is `/` or the word ends there. `~x` and `x~` stay literal, and
  so does `~` when `HOME` is unset or empty. Single quotes suppress it, so
  `'~'` is a tilde, but double quotes do not, so `"~"` is your home directory.
- Expansion output is never rescanned. A variable holding `$OTHER` expands
  once, to the text `$OTHER`.

**No word splitting.** The result of an expansion is one argv entry no matter
what is in it. If `FILES` holds `a b c`, then `wc $FILES` passes one argument,
the three-word string, not three arguments. There is also no globbing, so `echo
*.txt` prints `*.txt`. Both are deliberate: see [Limitations](#limitations).

## Pipes and redirection

| Syntax | Meaning | Example |
|---|---|---|
| `a \| b` | a's stdout becomes b's stdin; chains of any length work | `history \| grep echo` |
| `< file` | file becomes stdin | `wc -l < notes.txt` |
| `> file` | stdout to file, created or truncated | `heap dump > map.txt` |
| `>> file` | stdout appended to file | `echo two >> notes.txt` |
| `2> file` | stderr to file | `inspect bad 2> errors.txt` |

Details:

| Detail | Behavior |
|---|---|
| Target words | expanded like any other word, so `> $OUT` works |
| Order | file redirects are applied after pipe wiring, so an explicit `>` in the middle of a pipeline beats the pipe |
| Open failure | message on stderr, status 1, and the command does not run |
| Pipeline status | the last stage's status, with no `pipefail`: `false \| true` is 0, `true \| false` is 1 |
| Early reader exit | normal Unix flow control; `seq 100000 \| head -1` kills the writer with SIGPIPE |
| `2>` recognition | only when `2>` opens a word; `a2>` is the word `a2` then `>` |

Builtins in pipelines:

| Shape | Where the builtin runs | Consequence |
|---|---|---|
| `cd /tmp` | in the shell | the shell's directory changes |
| `cd /tmp > log` | in the shell, with fds saved and restored around it | the directory still changes and the output still lands in `log` |
| `history \| grep x` | in a forked child, like any other stage | side effects die with the child, which is what bash does |

## The interpreter

This is the part that makes nullsh a small language rather than a command
runner. Everything in this section arrived in phase 9.

### Operators

| Syntax | Meaning | Example |
|---|---|---|
| `a ; b` | run a, then b, whatever a did | `cd /tmp ; pwd` |
| newline | the same as `;` | two lines, two commands |
| `a && b` | run b only if a exited 0 | `mkdir d && cd d` |
| `a \|\| b` | run b only if a exited nonzero | `cd d \|\| echo no such dir` |
| `! p` | run p and invert its status | `! grep -q x file && echo absent` |
| `p &` | run the pipeline p in the background | `sleep 3 &` |

`&&` and `||` chain left to right with equal precedence, exactly like bash:
`a && b || c` runs `c` when either `a` or `b` fails. Both connect pipelines
only, never compound commands.

`!` applies to one pipeline and must come first. `! !` is a syntax error.

`&` backgrounds a plain pipeline and nothing else. `a && b &` and
`if ...; fi &` are syntax errors, because bash runs those in a subshell and
nullsh has no subshells.

### Compound commands

| Syntax | Meaning |
|---|---|
| `if LIST; then LIST; fi` | run the body when the condition's last command exits 0 |
| `if LIST; then LIST; elif LIST; then LIST; fi` | any number of `elif` clauses, tested in order |
| `if LIST; then LIST; else LIST; fi` | one optional `else` clause last |
| `while LIST; do LIST; done` | run the body while the condition's last command exits 0 |
| `for NAME in WORD...; do LIST; done` | run the body once per expanded word, with NAME set to it |
| `NAME () { LIST; }` | define a function |
| `NAME ARG...` | call it, with the arguments as `$1`..`$9` |
| `break` | leave the innermost loop |
| `continue` | start the innermost loop's next iteration |
| `return [N]` | leave the current function |

Written out:

```
if grep -q root /etc/passwd; then
    echo found
elif test -r /etc/passwd; then
    echo readable but no root
else
    echo unreadable
fi

while test -e /tmp/lock; do
    sleep 1
done

for f in a b c; do
    echo $f
done

greet() {
    echo hello $1
    return 0
}
greet world
```

Rules the syntax table cannot show:

- Keywords (`if then elif else fi while do done for in { } !`) are only special
  as a bare one-segment word where the grammar expects them. `echo if` prints
  `if`, and a file called `done` is an ordinary argument.
- `then`, `do`, `fi`, `done` and `}` must sit in command position, which means
  after a `;` or a newline. `if true then echo x fi` is one command with four
  arguments, not an `if`.
- A `for` always has an `in` list, and that list ends at a `;` or a newline,
  after which `do` follows. There is no `for x; do` shorthand.
- The loop variable is set with `setenv`, because nullsh has no separate shell
  variable table. It is an environment variable, it is visible to children, and
  it keeps its last value after the loop ends.
- NAME, for both `for` and function definitions, matches
  `[A-Za-z_][A-Za-z0-9_]*`.
- Every body must be non-empty. `if true; then fi` is a syntax error.
- Compound commands stand alone. They cannot be piped into or out of, they take
  no redirections, and they do not chain with `&&` or `||`.
  `if ...; fi | grep x`, `done > out` and `while ...; done && echo` are all
  syntax errors. Sequencing with `;` and newlines works, and compounds nest
  inside each other freely.

### Exit statuses

| Construct | Status rule |
|---|---|
| `a && b` | run b only if a exited 0; `$?` is the last command actually run |
| `a \|\| b` | run b only if a exited nonzero; `$?` is the last command actually run |
| `! p` | 1 if p exited 0, otherwise 0 |
| `a ; b` | run both; `$?` is b's |
| `if` | the first true condition's body decides `$?`; if no branch was taken, 0 |
| `while` | the last body iteration's status; 0 if the body never ran |
| `for` | the last body iteration's status; 0 if there were no words |
| `break` | leaves the loop with status 0 |
| `continue` | starts the next iteration |
| `break` or `continue` outside a loop | message on stderr, status 1 |
| `return N` | the function's status is N, taken as 0..255 |
| `return` | the function's status is `$?` as it stood where `return` ran |
| `return` outside a function | message on stderr, status 1 |
| `f() { ... }` | defines the function or silently replaces it; status 0 |
| a function call | the status of the last command in the body, or of `return` |
| a function call over the depth cap | message on stderr, status 1 |

`break` and `continue` affect the innermost loop only. There is no
`break N`.

### Functions

| Detail | Behavior |
|---|---|
| Dispatch order for a command word | builtin, then function, then PATH search |
| Redefinition | silent, and the new body wins |
| Arguments | `$1`..`$9` inside the body; `$#` is how many were passed |
| `$0` inside the body | unchanged: still the script path or shell name, never the function name |
| After the call | the caller's positional parameters are back, unchanged |
| Where the body runs | in the shell when the call is a lone command, in the forked child when it is a pipeline stage |
| Variables | shared with the environment: there is no `local` |
| Recursion | capped at 64 nested calls, then a message on stderr and status 1 |

Because dispatch checks builtins first, a function cannot shadow `cd`, `exit`
or any other builtin. That is a deliberate divergence from bash, chosen so the
shell can never be locked out of its own controls.

### Loops and Ctrl-C

The shell itself ignores SIGINT, which is why Ctrl-C kills your command and
spares the prompt. A loop made only of builtins forks nothing, so nothing would
receive the signal and the loop would be unkillable. The evaluator therefore
watches SIGINT while it is inside any loop and checks between iterations. A
foreground pipeline that came back with status 130 counts as the same request.
Either way the loop stops and its status is 130.

### Multi-line input

An unfinished construct is not an error. The shell prints `> ` and reads
another line, joins it to what came before with a newline, and re-parses the
whole buffer.

| Input ends after | What happens |
|---|---|
| `if`, `while`, `for` or `{` with the construct unclosed | continuation prompt |
| `&&`, `\|\|`, `\|` or `!` | continuation prompt |
| an unterminated `'` or `"` | continuation prompt; the quote gains a real newline |
| `>` or another redirect with no target yet | continuation prompt |
| `fi` with no `if`, `>` followed by an operator, or any other dead end | syntax error at once, status 2 |

Session shape:

```
nullsh:~$ for f in a b c; do
> echo $f
> done
a
b
c
```

| Event | Result |
|---|---|
| Ctrl-C at the `> ` prompt | the whole buffer is thrown away, back to the normal prompt |
| Ctrl-D at the `> ` prompt | `nullsh: syntax error: unexpected end of file`, status 2, the shell stays alive |
| EOF mid-construct on piped stdin or in a script | the same message and status 2, then the shell exits |

The accumulator re-lexes the whole buffer on every new line. That is quadratic
and entirely fine: interactive constructs are short.

### Script files

```
nullsh:~$ cat count.nsh
#!/usr/bin/env nullsh
# prints its arguments, one per line
for a in $1 $2 $3; do
    echo $a
done
echo "got $# args, script was $0"
nullsh:~$ nullsh count.nsh x y z
x
y
z
got 3 args, script was count.nsh
```

Scripts run through the same accumulator as piped stdin, so multi-line
constructs work the same way. Nothing about a script is interactive: no prompt
is printed, no line editing happens, and nothing is added to the history file.

## Builtins

A builtin runs inside the shell process when it is a command on its own, and in
a forked child when it is a stage of a pipeline. `help` prints the list.

| Syntax | Purpose |
|---|---|
| `bg [%N]` | resume a stopped job in the background |
| `break` | leave the innermost loop |
| `cd [DIR]` | change directory; no argument means `$HOME`, a single `-` means `$OLDPWD` and prints the new directory |
| `continue` | start the innermost loop's next iteration |
| `emu ROMFILE` | run a CHIP-8 rom |
| `exit [N]` | leave the shell, defaulting to the last status |
| `export NAME=VALUE` | set a variable for the shell and everything it starts |
| `fg [%N]` | bring a job to the foreground and wait for it |
| `heap [SUBCOMMAND]` | allocator statistics, strategy and block map |
| `help` | print the builtin list |
| `history` | print the command history, oldest first, numbered |
| `inspect [FLAGS] FILE` | show the structure of an ELF file |
| `jobs` | list the background and stopped jobs |
| `netmon IFACE [FLAGS]` | decode packets off an interface |
| `resolve NAME [FLAGS]` | look a name up over DNS and print the answer records |
| `return [N]` | leave the current function |
| `unset NAME` | remove a variable from the environment |

`cd` and `export` accept and reject the same shapes bash does: `cd` with more
than one argument is an error, `export NAME=VALUE` writes straight to the
environment, and a bare `export NAME` does nothing because nullsh has no
separate shell variable table. `unset` on a name that was never set is success.

### heap

| Syntax | What it prints |
|---|---|
| `heap` or `heap stats` | strategy, arena size, used and free bytes, live and free block counts, largest free block, total mallocs and frees |
| `heap strategy` | the strategy new allocations currently come from |
| `heap strategy firstfit` | switch new allocations to the first-fit arena |
| `heap strategy buddy` | switch new allocations to the buddy arena |
| `heap dump` | the block map of the arena |

Switching strategies never moves memory. Each arena keeps its own blocks and a
free routes by address, so every live pointer stays valid across a switch.

### inspect

| Flag | What it prints |
|---|---|
| (none) | the ELF header |
| `--sections` | the section table, which is the linker's view |
| `--segments` | the program headers, which is the loader's view |
| `--symbols` | `.symtab`, falling back to `.dynsym` on a stripped binary |
| `--all` | all four, separated by blank lines |

Flags combine, and exactly one FILE is required. An unreadable file gives
`nullsh: inspect: PATH: cannot open`; a file that is not a supported ELF gives
`nullsh: inspect: PATH: not a supported elf file`. 64-bit little-endian only.

### emu

| Thing | Behavior |
|---|---|
| `emu ROMFILE` | takes over the terminal, runs at about 700 instructions per second, Esc quits |
| Keypad | the 16 hex keys sit on `1234`, `qwer`, `asdf`, `zxcv` |
| `NSH_EMU_HEADLESS=N` | run exactly N cycles with no terminal, no timers and no sleeping, then dump the 64 by 32 framebuffer as ASCII |

The headless mode is what makes a rom paint the same frame every time, which is
why the README demo uses it.

### netmon

| Flag | Meaning |
|---|---|
| `IFACE` | the interface to capture on, required and first |
| `--filter tcp` | show TCP only |
| `--filter udp` | show UDP only |
| `--port N` | show only packets with N as source or destination port |

Capture opens a raw `AF_PACKET` socket, which needs `CAP_NET_RAW` and therefore
root:

```
nullsh:~$ netmon eth0
nullsh: netmon: eth0: needs root, try sudo nullsh
```

Ctrl-C stops a capture, and the counts of packets seen, shown and malformed go
to stderr on the way out. netmon installs its own SIGINT
handler for the duration and puts the previous one back, because in the
foreground it is running inside a shell that ignores SIGINT.

### resolve

```
resolve NAME [--server IP] [--port N] [--timeout MS] [--tries N]
```

Where netmon decodes packets other programs made, `resolve` makes one: it
builds a DNS query byte by byte from RFC 1035, sends it over UDP, and parses
the reply, name compression and all.

| Flag | Default | Meaning |
|---|---|---|
| `NAME` | required, and first | the name to look up; a trailing dot is accepted. The query type is always A |
| `--server IP` | the first IPv4 `nameserver` line of `/etc/resolv.conf` | the dotted IPv4 address of the server to ask |
| `--port N` | 53 | the UDP port on that server |
| `--timeout MS` | 2000 | milliseconds to wait for a reply, per try |
| `--tries N` | 2 | how many times the query is sent before giving up |
| `NSH_RESOLV_CONF=PATH` | `/etc/resolv.conf` | read the nameserver list from PATH instead, which is what the tests use |

An IPv6 `nameserver` line is skipped rather than tried. If the file names no
IPv4 nameserver and no `--server` was given, that is an error asking you to
pass one.

What a lookup prints:

```
;; id 4242 flags qr rd ra rcode NOERROR answers 2
example.com. 300 IN CNAME edge.example.net.
edge.example.net. 60 IN A 93.184.216.34
```

| Status | When |
|---|---|
| 0 | a reply came back with rcode NOERROR and parsed, even when it carried zero answers |
| 1 | everything else: usage errors, no nameserver, socket failure, silence after every try, NXDOMAIN and the other rcodes, a reply id mismatch, a malformed reply |
| 130 | Ctrl-C interrupted the wait |

Errors print as `nullsh: resolve: <reason>` in the netmon style, and a bad
argument gets a usage line.

Reading the output. The `;;` line always comes first and reports the header:
the query id, then the flags that are actually set, of `qr aa tc rd ra` and in
that order, then `rcode NAME` and how many answer records followed. After it
comes one line per answer, each with the record's name, its ttl in seconds, its
class (`IN`, or `CLASS%u` for a class nullsh has no name for), its type, and
then the address for an A record, the target for a CNAME, or `TYPE%u (%u
bytes)` for a type nullsh does not decode. Every name carries a trailing dot
because that is what the wire carries: a name ends in the empty root label, and
`example.com.` is the whole name while `example.com` would be a relative one. A
CNAME chain is several lines read top to bottom, the name you asked for
pointing at its alias and the alias further down carrying the address. Two more
`;;` lines can appear before the records: `;; truncated reply` when the server
had to cut the reply to fit 512 bytes, after which what you see is only what
arrived, and `;; recursion not available` when the server will not chase the
answer on your behalf.

Watching your own query. `resolve` is the other half of `netmon`, so run the
capture as a background job and the lookup in the foreground and the packets on
screen are yours:

```
nullsh:~$ netmon eth0 --filter udp --port 53 &
[1] 812
nullsh:~$ resolve example.com
```

netmon needs root for its raw socket, so that pairing happens in a
`sudo nullsh` session. The query leaving and the reply arriving are two UDP
packets on port 53, and the `;;` line's id is the same id netmon just watched
cross the wire.

One note on those ids. nullsh seeds the query id from the monotonic clock xor
the pid, which is fine for a tool you read but is guessable. An attacker who
can guess the id and the source port can answer before the real server does,
which is cache poisoning, and it is why real resolvers randomise both from a
cryptographic source.

## Job control

| Syntax or key | Effect |
|---|---|
| `cmd &` | run the pipeline in the background; `[N] PGID` goes to stderr; status 0 |
| Ctrl-Z | stop the foreground job; `[N]  Stopped  CMD` goes to stderr |
| Ctrl-C | SIGINT to the foreground job's process group only; the shell survives |
| `jobs` | list every live job |
| `fg` / `fg %N` | echo the command line, hand the terminal to that group, wait |
| `bg` / `bg %N` | SIGCONT a stopped job without handing over the terminal |

What the notifications look like:

```
nullsh:~$ sleep 3 &
[1] 426
nullsh:~$ jobs
[1]  Running  sleep 3 &
nullsh:~$ fg
sleep 3 &
[1]  Done  sleep 3 &
```

| Detail | Behavior |
|---|---|
| Job names | `%1` and a bare `1` both work |
| No argument | the current job: the most recent stopped one, else the most recent running one |
| Done notices | printed on stdout before the next prompt, lowest job id first |
| Stopped notices | printed on stderr the moment the stop is noticed |
| Job ids | the lowest free number is reused once a job is reported and pruned |
| Job command lines | rebuilt from the expanded words, so quoting and the original spacing are lost |
| Non-interactive | `&`, the job table and reaping all still work; only the terminal handoff is skipped |

Every pipeline gets its own process group, which is what makes `kill(-pgid, ...)`
and the Ctrl-C behavior work. The SIGCHLD handler only sets a flag; the real
`waitpid` work happens before the next prompt.

## Limitations

Every one of these is deliberate. nullsh is a shell for learning how a shell
works, not a replacement for bash.

| Missing | What happens instead |
|---|---|
| `$@` and `$*` | not recognised; use `$1`..`$9`, since nullsh does no word splitting and `$@` would only mislead |
| Positional parameters past `$9` | `$#` counts them, but only one digit is read, so `$10` is `$1` then `0` |
| Arithmetic `$(( ))` | not recognised; the text stays literal |
| Command substitution `$(...)` and backticks | not recognised; the text stays literal |
| Subshells `( ... )` | syntax error: parentheses are only the function definition marker |
| `case`, `until`, `select` | not keywords; they are ordinary words |
| Pipes or redirections on a compound command | syntax error: operators connect pipelines only |
| `&&` or `\|\|` on a compound command | syntax error, same reason |
| Backgrounding a list | syntax error; `&` takes a plain pipeline only |
| `local` | function variables share the process environment |
| Line continuation `\` at end of line | a literal backslash; use an unfinished construct or an open quote to span lines |
| Quoted keywords | the token model does not record how a word was quoted, so `"if"` in command position is still the keyword |
| Functions shadowing builtins | dispatch checks builtins first, so a function named `cd` is never called |
| `break N` | only the innermost loop, always |
| Globbing | `echo *.txt` prints `*.txt` |
| Word splitting after expansion | a variable holding spaces stays one argv entry |
| A shell variable table | `export NAME=VALUE` writes the environment; a bare `export NAME` does nothing |
| `~user` | stays literal; only `~` and `~/path` expand |
| `2>&1` and other fd duplication | syntax error; the redirects are `<`, `>`, `>>` and `2>` |
| Completion | there is no Tab completion, and editing is not readline |
| Lines wider than the terminal | the repaint wraps and the cursor jump lands on the wrong row |
| `inspect` on 32-bit or big-endian ELF | rejected with a message, never parsed halfway |
| `netmon` beyond IPv4 | Ethernet and IPv4 only, TCP and UDP only, no IPv6, no promiscuous mode, no kernel BPF; filtering happens after decoding |
| `resolve` beyond A records | the query type is always A and a query carries exactly one question: no AAAA, no MX, no TXT |
| TCP fallback on a truncated reply | `;; truncated reply` and the records that fit; a real client would ask again over TCP |
| EDNS(0) | no OPT record is sent, so a reply is capped at the classic 512 bytes |
| IPv6 nameservers | `--server` takes a dotted IPv4 address, and an IPv6 `nameserver` line in the config is skipped |
| Anything in `resolv.conf` but `nameserver` | that one file is the only config read, and only its `nameserver` lines: no `search`, no `domain`, no `ndots`, so NAME goes out exactly as you typed it |
| Unpredictable query ids | the id is the monotonic clock xor the pid, which is guessable; real resolvers randomise the id and the source port against cache poisoning |
| Re-listening after a reply id mismatch | `nullsh: resolve: reply id mismatch` and status 1; nullsh does not keep waiting for the right one |
| Portability | Linux only: `mmap`, `AF_PACKET`, `tcsetpgrp`, `setpgid` and `/proc` are called directly |
| Out of memory | `nsh_malloc` never returns NULL, so a failed allocation prints and aborts |
