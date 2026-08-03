// nullsh entry point. Phase 0 is a REPL stub: prompt, read a line, honor exit
// and blank input, and say plainly that running commands waits for phase 1.

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char PROMPT[] = "nullsh> ";

// Points at the first whitespace delimited word in line and reports its
// length. Returns NULL when the line holds nothing but whitespace.
static const char *first_word(const char *line, size_t *out_len) {
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    const char *start = line;
    while (*line != '\0' && *line != ' ' && *line != '\t') {
        line++;
    }
    *out_len = (size_t)(line - start);
    return *out_len == 0 ? NULL : start;
}

int main(void) {
    // The prompt goes out only on a terminal, so piped scripts see clean output.
    const int interactive = isatty(STDIN_FILENO);

    // getline allocates and grows this buffer inside libc, so it is the one
    // block in nullsh released with libc free instead of nsh_free.
    char *line = NULL;
    size_t cap = 0;
    int status = 0;

    for (;;) {
        if (interactive) {
            fputs(PROMPT, stdout);
            if (fflush(stdout) != 0) {
                perror("nullsh: fflush");
                status = 1;
                break;
            }
        }

        ssize_t len = getline(&line, &cap, stdin);
        if (len < 0) {
            if (ferror(stdin)) {
                perror("nullsh: getline");
                status = 1;
            }
            if (interactive) {
                fputc('\n', stdout);
            }
            break;
        }

        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        size_t word_len = 0;
        const char *word = first_word(line, &word_len);
        if (word == NULL) {
            continue;
        }
        if (word_len == 4 && strncmp(word, "exit", 4) == 0) {
            break;
        }

        fprintf(stderr, "nullsh: %.*s: command execution arrives in phase 1\n",
                (int)word_len, word);
    }

    free(line);
    return status;
}
