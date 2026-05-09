// Lexer: one command line in, a TokenList out. Quotes are resolved here, so
// consumers see words already split into segments that say whether expansion
// may touch them.

#pragma once

#include "token.h"

#include "../util/error.h"

// Scans one command line into out (out is overwritten, not appended). out must
// be a zeroed TokenList or one filled by an earlier scan.
// NSH_OK, or NSH_ERR_SYNTAX for unterminated quotes. On error, out is left
// as a valid empty list (nothing leaks).
NshError lexer_scan(const char *line, TokenList *out);
