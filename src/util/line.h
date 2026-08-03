// Line reader built on Str.

#pragma once

#include <stdio.h>

#include "error.h"
#include "str.h"

// Clears out first and strips the newline. NSH_EOF only when nothing was read.
NshError line_read(FILE *in, Str *out);
