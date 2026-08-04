// Error codes shared by every nullsh module.

#pragma once

typedef enum {
    NSH_OK = 0,
    NSH_ERR_ALLOC,
    NSH_ERR_SYNTAX,
    NSH_ERR_IO,
    NSH_ERR_NOT_FOUND,
    NSH_ERR_INVALID,
    // Not an error: end of input reached with nothing read.
    NSH_EOF
} NshError;

// Never returns NULL, including for values outside the enum.
const char *nsh_error_str(NshError e);
