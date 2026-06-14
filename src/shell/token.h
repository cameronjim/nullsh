// Token structures shared by the lexer (producer), expander, and parser
// (consumers). Construction and destruction live in lexer.c; the consumers
// only read these.

#pragma once

#include <stdbool.h>

#include "../util/vec.h"

typedef enum {
    TOK_WORD,
    TOK_PIPE,          // |
    TOK_REDIR_IN,      // <
    TOK_REDIR_OUT,     // >
    TOK_REDIR_APPEND,  // >>
    TOK_REDIR_ERR,     // 2>
    TOK_AMP            // &
} TokenKind;

// One quoting run inside a word. "a"'b'c is three segments; expansion applies
// only where expand is true (bare text and double quotes, never single quotes).
typedef struct {
    char *text;   // nsh-allocated, NUL terminated, may be empty for ""
    bool expand;
} WordSeg;

typedef struct {
    TokenKind kind;
    Vec segs;  // WordSeg*, non-empty only for TOK_WORD
} Token;

typedef struct {
    Vec tokens;  // Token*
} TokenList;

// Frees a token and everything it owns. Implemented in lexer.c.
void token_free(Token *t);

// Frees every token and the list's storage. Safe on a zeroed list.
void token_list_free(TokenList *tl);
