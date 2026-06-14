// Parser: a TokenList in, one Pipeline out.

#pragma once

#include <stdbool.h>

#include "token.h"

#include "../util/error.h"

// Words stay unexpanded here; expansion happens at execution time.
typedef struct {
    Vec words;           // Token*, argv words in order, owned
    Token *redir_in;     // NULL or the filename word for <, owned
    Token *redir_out;    // NULL or the filename word for > / >>, owned
    bool redir_append;   // redir_out came from >>
    Token *redir_err;    // NULL or the filename word for 2>, owned
} Command;

typedef struct {
    Vec cmds;            // Command*, owned
    bool background;     // trailing &
} Pipeline;

// Consumes tl and overwrites out; both are left valid on every path.
NshError parser_parse(TokenList *tl, Pipeline *out);
void pipeline_free(Pipeline *p);  // safe on a zeroed or already-freed pipeline
