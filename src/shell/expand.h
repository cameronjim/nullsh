// Variable expansion for lexed words: $NAME, ${NAME} and $?.

#pragma once

#include "token.h"
#include "../util/error.h"

// Expands one TOK_WORD into one string the caller nsh_frees.
NshError expand_word(const Token *tok, int last_status, char **out);
