// Expansion for lexed words: $NAME, ${NAME}, $?, $0..$9, $# and a leading ~.

#pragma once

#include "token.h"
#include "../util/error.h"

typedef struct {
    int last_status;  // feeds $?
    int argc;         // positional count including $0
    char **argv;      // argv[0] is $0; may be NULL when argc is 0
} ExpandCtx;

// Expands one TOK_WORD into one string the caller nsh_frees. The tilde only
// counts at the very start of the word, in an expandable (unquoted) segment,
// and only when followed by / or by the end of the word. ~user is left alone.
// A NULL ctx reads as status 0 with no positional parameters.
NshError expand_word(const Token *tok, const ExpandCtx *ctx, char **out);
