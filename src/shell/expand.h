// Expansion for lexed words: $NAME, ${NAME}, $? and a leading ~.

#pragma once

#include "token.h"
#include "../util/error.h"

// Expands one TOK_WORD into one string the caller nsh_frees. The tilde only
// counts at the very start of the word, in an expandable (unquoted) segment,
// and only when followed by / or by the end of the word. ~user is left alone.
NshError expand_word(const Token *tok, int last_status, char **out);
