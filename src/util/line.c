// One line at a time out of a FILE and into a Str. Reads byte by byte so a
// NUL in the middle of a line is kept rather than ending the line, which is
// what getline does too.

#include "line.h"

NshError line_read(FILE *in, Str *out) {
    str_clear(out);
    int got_any = 0;
    for (;;) {
        int c = fgetc(in);
        if (c == EOF) {
            // EOF and error share the same return value, so ask the stream
            // which one actually happened.
            if (ferror(in)) {
                return NSH_ERR_IO;
            }
            return got_any ? NSH_OK : NSH_EOF;
        }
        got_any = 1;
        if (c == '\n') {
            return NSH_OK;
        }
        str_push(out, (char)(unsigned char)c);
    }
}
