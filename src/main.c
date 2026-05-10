// nullsh entry point: prompt, read, lex, parse, execute, and the history file.

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "alloc/alloc.h"
#include "shell/exec.h"
#include "shell/lexer.h"
#include "shell/parser.h"
#include "shell/shell.h"
#include "util/line.h"
#include "util/str.h"

#define HISTORY_CAP 1000
#define HISTORY_FILE "/.nullsh_history"
#define CWD_MAX 4096

// NULL when HOME says nothing. Caller nsh_frees the result.
static char *history_path(void) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return NULL;
    }
    Str p;
    str_init(&p);
    str_append(&p, home);
    str_append(&p, HISTORY_FILE);
    char *out = str_take(&p);
    str_free(&p);
    return out;
}

// The tilde replaces HOME only on a whole path component, never in /homework.
static void print_prompt(void) {
    char cwd[CWD_MAX];
    const char *home = getenv("HOME");
    size_t hlen = (home == NULL) ? 0 : strlen(home);
    if (getcwd(cwd, sizeof cwd) == NULL) {
        fputs("nullsh$ ", stdout);
    } else if (hlen > 0 && strncmp(cwd, home, hlen) == 0 &&
               (cwd[hlen] == '\0' || cwd[hlen] == '/')) {
        printf("nullsh:~%s$ ", cwd + hlen);
    } else {
        printf("nullsh:%s$ ", cwd);
    }
    fflush(stdout);
}

int main(void) {
    Shell sh = {{NULL, 0, 0, 0}, 0, false, 0};
    history_init(&sh.history, HISTORY_CAP);
    char *hist_file = history_path();
    if (hist_file != NULL) {
        // A first run has no file yet, which is not an error.
        history_load(&sh.history, hist_file);
    }

    const int interactive = isatty(STDIN_FILENO);
    Str line;
    str_init(&line);
    TokenList tl = {{NULL, 0, 0}};
    Pipeline pl = {{NULL, 0, 0}, false};

    for (;;) {
        if (interactive) {
            print_prompt();
        }
        NshError err = line_read(stdin, &line);
        if (err == NSH_EOF) {
            if (interactive) {
                fputc('\n', stdout);
            }
            break;
        }
        if (err != NSH_OK) {
            fputs("nullsh: read error on stdin\n", stderr);
            sh.last_status = 1;
            break;
        }
        if (line.len == 0) {
            continue;
        }

        // Recorded before parsing, so a bad line can still be recalled.
        history_add(&sh.history, line.data);
        if (lexer_scan(line.data, &tl) != NSH_OK ||
            parser_parse(&tl, &pl) != NSH_OK) {
            fputs("nullsh: syntax error\n", stderr);
            sh.last_status = 2;
        } else {
            exec_pipeline(&sh, &pl);
        }
        token_list_free(&tl);
        pipeline_free(&pl);
        if (sh.want_exit) {
            break;
        }
    }
    if (hist_file != NULL) {
        if (history_save(&sh.history, hist_file) != NSH_OK) {
            fprintf(stderr, "nullsh: could not save history to %s\n",
                    hist_file);
        }
        nsh_free(hist_file);
    }
    token_list_free(&tl);
    pipeline_free(&pl);
    str_free(&line);
    history_free(&sh.history);
    // Ctrl-D leaves with the status of the last command, like bash.
    return sh.want_exit ? sh.exit_code : sh.last_status;
}
