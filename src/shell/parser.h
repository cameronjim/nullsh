// Parser: a TokenList in, an AST out, plus the legacy single-pipeline entry.

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

typedef struct Node Node;

// Consumes tl and writes the tree through out; *out is NULL for an empty
// program. NSH_ERR_INCOMPLETE means the input ended mid-construct and the
// caller should gather more lines. tl is left valid and empty on every path.
NshError parser_parse_program(TokenList *tl, Node **out);

// Legacy: accepts exactly one plain pipeline, kept for existing callers.
NshError parser_parse(TokenList *tl, Pipeline *out);
void pipeline_free(Pipeline *p);  // safe on a zeroed or already-freed pipeline
