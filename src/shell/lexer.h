// Lexer: one command line in, a TokenList out.

#pragma once

#include "token.h"

#include "../util/error.h"

// out is overwritten, not appended, and left valid and empty on error.
NshError lexer_scan(const char *line, TokenList *out);
