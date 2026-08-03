// Parser: a TokenList in, one Pipeline out. It decides command boundaries,
// which word fills which redirect slot, and whether the line ends in a
// background &. Words are not expanded here; they stay as lexer tokens.

#pragma once

#include <stdbool.h>

#include "token.h"

#include "../util/error.h"

// One command in a pipeline. Words stay as unexpanded Token* (TOK_WORD);
// expansion happens at execution time.
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

// Consumes tl: word tokens move into out, operator tokens are freed, and tl is
// left as a valid empty list either way. On error, out is a valid empty
// pipeline. An empty tl gives NSH_OK and an empty pipeline (cmds.len == 0).
// out is overwritten, not appended, so it must be a zeroed Pipeline or one
// filled by an earlier parse.
NshError parser_parse(TokenList *tl, Pipeline *out);
void pipeline_free(Pipeline *p);   // frees commands, their tokens, the vec; safe on zeroed or already-freed
