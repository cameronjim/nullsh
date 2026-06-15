# phase 9: the interpreter

The shell learns to make decisions. Until now nullsh executes one flat pipeline
per line and forgets it. This phase replaces the flat grammar with a real
abstract syntax tree and an evaluator that walks it, which turns nullsh from a
command runner into a small programming language: `&&` and `||`, `;`, `!`,
`if`/`elif`/`else`, `while`, `for`, functions with arguments, `break`,
`continue`, `return`, comments, multi-line input with a continuation prompt,
and script files with positional parameters.

## Concepts

**Why a tree.** `a | b` is a sequence: a vector of commands represents it
fully. `a && b` is a decision: whether `b` runs depends on the *result* of
`a`, and `if` nests arbitrarily deep. A decision cannot live in a flat list
because the structure is recursive. The classic answer is the same one every
language uses: parse the text into a tree where each node knows its children,
then evaluate the tree top down. The node for `&&` holds two subtrees and
evaluates the right one only if the left one exited 0.

**Keywords are contextual.** `if` is only special where the grammar expects a
command to start. `echo if` prints "if" because that word sits in argument
position. Real shells work exactly this way, which is why the lexer stays dumb:
it emits words, and the parser decides whether a word in command position is a
keyword. One nullsh divergence, documented: the token model does not record
whether a word was double quoted, so `"if"` in command position is still the
keyword. Single facts like this go in the manual's limitations table.

**Incomplete is not invalid.** When you type `if true` and press Enter, the
input is not wrong, it is unfinished. The lexer and parser distinguish the two:
a construct that ran out of tokens returns the new `NSH_ERR_INCOMPLETE`, and
the driver responds by printing `> ` and reading another line, joining lines
with `\n` and re-parsing the whole buffer. A true dead end (`fi` with no `if`)
stays `NSH_ERR_SYNTAX`.

**Control flow is a signal, not a return value.** `break` deep inside an `if`
inside a `while` must unwind the evaluator's recursion up to the loop. The
evaluator does what real shells do: `break`, `continue` and `return` set a flag
on the Shell (`sh->flow`), and every list evaluator checks the flag after each
item and stops early. The loop evaluator consumes BREAK and CONTINUE; the
function call consumes RETURN.

**Positional parameters are a stack.** `$1` inside a function is the
function's first argument; after the call returns, `$1` is what it was before.
The evaluator saves `sh->argc/argv`, swaps in the call's argv, runs the body,
and restores. Scripts set the same fields once at startup, so `$0`, `$1`..`$9`
and `$#` work identically in both.

## Grammar

```
program  : list?                            (empty input is a no-op, *out NULL)
list     : item ((';' | '&' | NEWLINE) item?)*
item     : ['!'] pipeline (('&&' | '||') linebreak ['!'] pipeline)*
         | compound
pipeline : simple ('|' linebreak simple)*
simple   : (WORD | redirect)+               (unchanged from phase 3)
compound : if | while | for | funcdef
if       : 'if' body 'then' body ('elif' body 'then' body)* ('else' body)? 'fi'
while    : 'while' body 'do' body 'done'
for      : 'for' NAME linebreak 'in' WORD* (';' | NEWLINE) linebreak 'do' body 'done'
funcdef  : NAME '(' ')' linebreak '{' body '}'
body     : linebreak list linebreak         (must be non-empty)
linebreak: NEWLINE*
```

Rules the grammar shorthand cannot show:

- Keywords (`if then elif else fi while do done for in { } !`) are recognized
  only as a bare word (exactly one segment) in the position where the grammar
  wants them. Elsewhere they are ordinary words.
- `&` backgrounds only a plain pipeline. `a && b &` and `if ...; fi &` are
  syntax errors (the parser stays silent like all its errors; main prints the
  generic message). Rationale: bash runs those in a subshell and nullsh has no
  subshells.
- Compounds stand alone as an item: they cannot be piped into or out of, take
  no redirections, and do not chain with `&&`/`||`. `if ...; fi | grep x`,
  `fi > out` and `if ...; fi && echo` are all syntax errors. Strict and
  documented: operators connect pipelines only. Sequencing with `;` and
  newlines still works, and compounds nest freely inside each other.
- `!` applies to one pipeline and inverts its status (0 becomes 1, nonzero
  becomes 0). `! !` is a syntax error.
- NAME for `for` and funcdefs matches `[A-Za-z_][A-Za-z0-9_]*`.
- `}` and `then`/`do`/`fi`/`done` must sit in command position, which means
  after `;` or a newline, exactly like bash.
- A redirect target must be a WORD; `>` followed by an operator is a syntax
  error, `>` at end of input is INCOMPLETE.
- Ends of input inside any compound, after `&&`, `||`, `|`, or after `!` are
  INCOMPLETE, not syntax errors.

## Token changes (already landed in the contracts commit)

New kinds: `TOK_AND_IF` (&&), `TOK_OR_IF` (||), `TOK_SEMI` (;),
`TOK_NEWLINE` (\n), `TOK_LPAREN` ((), `TOK_RPAREN` ()).

Lexer behavior changes:

- `&&` before `&`, `||` before `|`, longest match first, same as `>>`.
- `(`, `)`, `;` and `\n` become operators everywhere outside quotes. `;;` is
  two TOK_SEMIs (the parser rejects the empty item between them: harmless).
- An unquoted `#` at the start of a word opens a comment running to the next
  `\n` or end of input. The `\n` itself still becomes TOK_NEWLINE. `a#b` stays
  one word.
- Unterminated `'` or `"` now returns `NSH_ERR_INCOMPLETE` (was SYNTAX), which
  is what lets quotes span lines: the driver appends the next line and the
  re-lex sees the closing quote with a literal `\n` inside the segment.
- A trailing `\` stays a literal backslash (no line continuation, documented
  limitation).

## AST (see src/shell/ast.h, frozen contract)

`Node` is a tagged union. `NODE_PIPELINE` embeds the existing `Pipeline` by
value plus a `negate` flag, so exec_pipeline stays the single place processes
are born. `NODE_ANDOR` holds a vector of `AndOrItem {op, node}` where op says
how the item chains to the one before it (`ANDOR_FIRST` for items[0]).
`NODE_LIST` is a vector of nodes run in order. `NODE_IF` holds parallel
`conds`/`bodies` vectors plus an optional `else_body`. `NODE_WHILE` and
`NODE_FOR` and `NODE_FUNCDEF` are as their fields read. `ast_free` is
recursive and safe on NULL; `ast_clone` deep-copies (the evaluator clones a
funcdef body into the function table because a funcdef inside a loop
evaluates more than once).

The old `parser_parse` (one flat pipeline) survives as a thin wrapper over
`parser_parse_program` that accepts exactly one plain foreground-or-background
pipeline and errors on anything else. Existing callers and tests keep passing.

## Semantics

| Construct | Status rules |
|---|---|
| `a && b` | run b only if a exited 0; `$?` is the last command actually run |
| `a \|\| b` | run b only if a exited nonzero |
| `! p` | 1 if p exited 0, else 0 |
| `a ; b` | run both; `$?` is b's |
| `if` | first true cond's body decides `$?`; no branch taken means 0 |
| `while` | last body iteration's status; 0 if the body never ran |
| `for x in w...` | expand each word, setenv the variable, run body; 0 if no words |
| `break` / `continue` | innermost loop only; outside a loop: message on stderr, status 1; `break` leaves the loop with status 0 |
| `return [n]` | functions only; status n (0..255) or `$?` at the call site of return; outside: message, status 1 |
| `f() { ... }` | defines or silently replaces; status 0 |
| function call | runs the body in-process when lone, in the forked child inside a pipeline; args become `$1`..; depth capped at 64, over the cap: message, status 1 |

Dispatch order for a command word: builtin, then function, then PATH search.
Functions therefore cannot shadow `exit` or `cd`, which is a documented
divergence from bash chosen so the shell can never be locked out of its own
controls.

Loops and Ctrl-C: the shell ignores SIGINT, so an all-builtin loop
(`while true; do :; done`) would be unkillable. The evaluator turns on
`signals_int_watch(1)` when loop depth goes 0 to 1 and off at 1 to 0; each
iteration checks `signals_int_take()` and also treats a pipeline status of 130
(child killed by SIGINT) as a stop request. Either way the loop ends with
status 130.

For-loop variables are set with `setenv(3)`: nullsh has no separate shell
variable table (deliberate: `export` is the only assignment primitive), so the
loop variable is an environment variable and `$x` expansion just works.

## Expansion additions

`expand_word` grows a context: `ExpandCtx { int last_status; int argc;
char **argv; }` (argv[0] is `$0`). New forms: `$0`..`$9` and `${0}`..`${9}`
(one digit only, `$10` is `$1` followed by `0`), `$#` (argc minus 1, never
negative). Out-of-range positionals expand to the empty string. `$@` and `$*`
are deliberate omissions (nullsh does no word splitting, so they would
mislead); the manual's limitation table says to use `$1`..`$9`.

## Multi-line input and scripts

`run.c` owns the read-eval loop for all three input styles:

- interactive: PS1 from the existing prompt builder, PS2 is `> `, line
  editing and history as today (each physical line is recorded, including
  continuation lines). Ctrl-C during PS2 abandons the buffer. Ctrl-D during
  PS2 prints `nullsh: syntax error: unexpected end of file`, sets status 2,
  keeps the shell alive.
- piped stdin: same accumulation, no prompts. EOF mid-construct: same message,
  status 2, then exit.
- script file: `nullsh PATH [ARGS...]` opens PATH (failure: `nullsh: PATH:
  <errno text>`, exit 127), sets `sh->argc/argv` so `$0` is PATH, runs the
  stream, exits with the last status. `#!/...` on line 1 is just a comment.
  History, prompts and editing are interactive-only.

The accumulator joins physical lines with `\n` and re-lexes the whole buffer
each time. Quadratic and fine: interactive constructs are short.

## Module ownership (one agent each, files are disjoint)

| Agent | Owns | Delivers |
|---|---|---|
| lexer | lexer.c, lexer_test.c | new operators, comments, INCOMPLETE quotes, `\n` handling |
| parser | parser.c, parser_test.c | recursive descent to AST, INCOMPLETE detection, legacy wrapper |
| exec | expand.h/.c, expand_test.c, exec.c, exec_test.c, builtin.c, builtin_test.c | ExpandCtx + positionals, function dispatch hook, break/continue/return builtins |
| eval | eval.c, func.c, eval_test.c | tree walker, flow flags, loops, function table and calls |
| run | run.c, main.c, tests/integration/10_interpreter.sh | drivers, PS2, script mode, end-to-end tests |
| docs | docs/manual.md, README.md, claude-docs/architecture.md | user manual with syntax tables, README scripting section, architecture updates |

Frozen contracts (do not edit, report problems instead): token.h, error.h/.c,
ast.h, ast.c, parser.h, eval.h, func.h, run.h, shell.h, signals.h/.c,
Makefile. expand.h belongs to the exec agent.

## Testing requirements

- Parser tests construct TokenLists programmatically (a local `mk_word`/`mk_op`
  helper), not via the lexer, so the two modules stay independently testable.
  Assert tree shapes, INCOMPLETE vs SYNTAX for every truncation point, and
  ownership (ASan runs the suite, leaks fail it).
- Eval tests build small trees by hand with ast_node_new and assert
  last_status, flow flag consumption, loop iteration counts via files in a
  mktemp dir, function arg save/restore, and the recursion cap.
- The exec agent tests positional expansion table-style and the three new
  builtins' outside-context errors.
- Integration test 10_interpreter.sh drives the debug binary with printf
  pipelines end to end: operators, if/while/for, break/continue/return,
  functions (define, call, args, redefine, recursion cap message), comments,
  multi-line constructs via piped stdin, script files with args and shebang,
  `$?` propagation, the background restriction message, and syntax error
  statuses (2). Follow the existing scripts' check/fail helper style.
- Everything must pass under both allocator strategies, which `make test`
  runs automatically.

## Deliberate limitations (manual gets the full table)

No `$@`/`$*`, no arithmetic `$(( ))`, no command substitution, no subshells,
no `case`, no `until`, no redirections or pipes on compound commands, no
backgrounding of lists, no `local` (function variables share the
environment), no line continuation backslash, quoted words can still be
keywords in command position, functions cannot shadow builtins.
