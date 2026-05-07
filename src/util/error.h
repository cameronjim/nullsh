// Error codes shared by every nullsh module. Functions return NshError and
// the caller turns it into text with nsh_error_str. No errno smuggling, no
// fprintf from deep inside a helper.

#pragma once

typedef enum {
    NSH_OK = 0,
    NSH_ERR_ALLOC,
    NSH_ERR_SYNTAX,
    NSH_ERR_IO,
    NSH_ERR_NOT_FOUND,
    NSH_ERR_INVALID
} NshError;

// Never returns NULL, including for values outside the enum.
const char *nsh_error_str(NshError e);
