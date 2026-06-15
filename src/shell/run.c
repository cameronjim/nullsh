// Read-eval drivers: the interactive REPL and non-interactive streams.

#define _POSIX_C_SOURCE 200809L

#include "run.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ast.h"
#include "edit.h"
#include "eval.h"
#include "jobs.h"
#include "lexer.h"
#include "parser.h"
#include "signals.h"

#include "../util/line.h"
#include "../util/str.h"

#define CWD_MAX 4096

// The continuation prompt, shown while a construct is still unfinished.
#define PS2 "> "

#define STATUS_SYNTAX 2

// The tilde replaces HOME only on a whole path component, never in /homework.
// The editor needs the prompt as text, so it is built rather than printed.
static void prompt_build(Str *p) {
    char cwd[CWD_MAX];
    const char *home = getenv("HOME");
    size_t hlen = (home == NULL) ? 0 : strlen(home);
    str_clear(p);
    if (getcwd(cwd, sizeof cwd) == NULL) {
        str_append(p, "nullsh$ ");
        return;
    }
    if (hlen > 0 && strncmp(cwd, home, hlen) == 0 &&
        (cwd[hlen] == '\0' || cwd[hlen] == '/')) {
        str_append(p, "nullsh:~");
        str_append(p, cwd + hlen);
    } else {
        str_append(p, "nullsh:");
        str_append(p, cwd);
    }
    str_append(p, "$ ");
}

// The prompt's garbage collection: drain every pending child event, then talk.
static void reap_jobs(void) {
    if (!signals_chld_take()) {
        return;
    }
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        jobs_update(pid, status);
    }
    jobs_reap_notify(stdout);
}

// What one pass over the accumulated buffer decided.
typedef enum { FEED_MORE, FEED_RAN, FEED_BAD } FeedResult;

// Lexes and parses the whole buffer again, then evaluates it if it is whole.
static FeedResult feed(Shell *sh, const char *text) {
    TokenList tl = {{NULL, 0, 0}};
    Node *tree = NULL;
    NshError err = lexer_scan(text, &tl);
    if (err == NSH_OK) {
        err = parser_parse_program(&tl, &tree);
    }
    token_list_free(&tl);
    if (err == NSH_ERR_INCOMPLETE) {
        return FEED_MORE;
    }
    if (err != NSH_OK) {
        return FEED_BAD;
    }
    if (tree != NULL) {
        NshError ran = eval_run(sh, tree);
        if (ran != NSH_OK) {
            fprintf(stderr, "nullsh: %s\n", nsh_error_str(ran));
            fflush(stderr);
        }
        ast_free(tree);
    }
    return FEED_RAN;
}

static void syntax_error(Shell *sh) {
    fputs("nullsh: syntax error\n", stderr);
    fflush(stderr);
    sh->last_status = STATUS_SYNTAX;
}

static void unexpected_eof(Shell *sh) {
    fputs("nullsh: syntax error: unexpected end of file\n", stderr);
    fflush(stderr);
    sh->last_status = STATUS_SYNTAX;
}

static void read_error(Shell *sh) {
    fputs("nullsh: read error on input\n", stderr);
    fflush(stderr);
    sh->last_status = 1;
}

NshError run_interactive(Shell *sh) {
    if (sh == NULL) {
        return NSH_ERR_INVALID;
    }
    Str line;
    Str prompt;
    Str buf;
    str_init(&line);
    str_init(&prompt);
    str_init(&buf);
    // True while buf holds a construct the parser called unfinished.
    bool more = false;

    for (;;) {
        const char *ps = PS2;
        if (!more) {
            reap_jobs();
            prompt_build(&prompt);
            ps = prompt.data;
        }
        // Raw mode is entered and left inside the call, so a command that
        // runs next inherits a cooked terminal.
        NshError err = edit_read_line(ps, &sh->history, &line);
        if (err == NSH_EOF) {
            fputc('\n', stdout);
            fflush(stdout);
            if (!more) {
                break;
            }
            unexpected_eof(sh);
            str_clear(&buf);
            more = false;
            continue;
        }
        if (err == NSH_INTERRUPT) {
            // Ctrl-C abandons the line and any half-built construct with it.
            str_clear(&buf);
            more = false;
            continue;
        }
        if (err != NSH_OK) {
            read_error(sh);
            break;
        }
        // A blank line inside a construct is part of it; on its own it is not.
        if (line.len == 0 && !more) {
            continue;
        }

        if (line.len > 0) {
            // Recorded before parsing, so a bad line can still be recalled.
            history_add(&sh->history, line.data);
        }
        if (more) {
            str_push(&buf, '\n');
        }
        str_append_n(&buf, line.data, line.len);

        FeedResult r = feed(sh, buf.data);
        if (r == FEED_MORE) {
            more = true;
            continue;
        }
        if (r == FEED_BAD) {
            syntax_error(sh);
        }
        str_clear(&buf);
        more = false;
        if (sh->want_exit) {
            break;
        }
    }

    str_free(&line);
    str_free(&prompt);
    str_free(&buf);
    return NSH_OK;
}

NshError run_stream(Shell *sh, FILE *fp) {
    if (sh == NULL || fp == NULL) {
        return NSH_ERR_INVALID;
    }
    Str line;
    Str buf;
    str_init(&line);
    str_init(&buf);
    bool more = false;
    // Piped stdin still feeds the history file the way it always has; a
    // script file is somebody else's text and stays out of it.
    bool record = (fp == stdin);

    for (;;) {
        if (!more) {
            reap_jobs();
        }
        NshError err = line_read(fp, &line);
        if (err == NSH_EOF) {
            if (more) {
                unexpected_eof(sh);
            }
            break;
        }
        if (err != NSH_OK) {
            read_error(sh);
            break;
        }
        // A blank line inside a construct is part of it; on its own it is not.
        if (line.len == 0 && !more) {
            continue;
        }

        if (record && line.len > 0) {
            history_add(&sh->history, line.data);
        }
        if (more) {
            str_push(&buf, '\n');
        }
        str_append_n(&buf, line.data, line.len);

        FeedResult r = feed(sh, buf.data);
        if (r == FEED_MORE) {
            more = true;
            continue;
        }
        if (r == FEED_BAD) {
            // Nobody is there to retype it, so the stream stops, like bash.
            syntax_error(sh);
            break;
        }
        str_clear(&buf);
        more = false;
        if (sh->want_exit) {
            break;
        }
    }

    str_free(&line);
    str_free(&buf);
    return NSH_OK;
}
