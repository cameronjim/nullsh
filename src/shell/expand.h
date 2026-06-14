// Variable expansion for lexed words. Turns the segments of one TOK_WORD into
// the single string the parser hands to argv, substituting $NAME, ${NAME} and
// $? in the segments the lexer marked expandable.

#pragma once

#include "token.h"
#include "../util/error.h"

// Expands one TOK_WORD token into a single nsh-allocated string (caller frees
// with nsh_free). Segments with expand=true get $ expansion; expand=false
// segments are copied verbatim. last_status feeds $?.
// Returns NSH_OK, NSH_ERR_INVALID if tok is not TOK_WORD,
// NSH_ERR_SYNTAX for an unterminated ${.
NshError expand_word(const Token *tok, int last_status, char **out);
