// Text for the NshError codes. One short lowercase phrase per code, meant to
// read well after a "nullsh: " prefix.

#include "error.h"

const char *nsh_error_str(NshError e) {
    switch (e) {
    case NSH_OK:
        return "ok";
    case NSH_ERR_ALLOC:
        return "out of memory";
    case NSH_ERR_SYNTAX:
        return "syntax error";
    case NSH_ERR_IO:
        return "i/o error";
    case NSH_ERR_NOT_FOUND:
        return "not found";
    case NSH_ERR_INVALID:
        return "invalid argument";
    case NSH_EOF:
        return "end of input";
    }
    return "unknown error";
}
