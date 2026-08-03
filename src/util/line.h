// Line reader built on Str, so the shell can drop getline and keep every byte
// it allocates inside src/alloc.

#pragma once

#include <stdio.h>

#include "error.h"
#include "str.h"

// Reads one line from in into out. out must already be initialized and is
// cleared first. The trailing newline is stripped. Returns NSH_OK when a line
// was read, including a last line that ended at EOF with no newline, NSH_EOF
// when EOF arrived with nothing read, and NSH_ERR_IO on a read error.
NshError line_read(FILE *in, Str *out);
