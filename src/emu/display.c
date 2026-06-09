// Framebuffer to text rendering for the CHIP-8 display. Pure: no terminal calls.

#include "display.h"

// Half blocks as explicit UTF-8 bytes so this file stays ASCII.
#define CELL_FULL "\xe2\x96\x88"
#define CELL_UPPER "\xe2\x96\x80"
#define CELL_LOWER "\xe2\x96\x84"

// Cursor home; the caller clears the screen once at startup.
#define ESC_HOME "\x1b[H"

_Static_assert(CHIP8_H % 2 == 0, "half block rendering pairs rows");

static const char *cell_for(int top, int bottom) {
    if (top && bottom) {
        return CELL_FULL;
    }
    if (top) {
        return CELL_UPPER;
    }
    if (bottom) {
        return CELL_LOWER;
    }
    return " ";
}

void display_render(const uint8_t *fb, Str *out) {
    str_clear(out);
    str_append(out, ESC_HOME);
    for (int y = 0; y < CHIP8_H; y += 2) {
        for (int x = 0; x < CHIP8_W; x++) {
            int top = fb[y * CHIP8_W + x] != 0;
            int bottom = fb[(y + 1) * CHIP8_W + x] != 0;
            str_append(out, cell_for(top, bottom));
        }
        str_push(out, '\n');
    }
}

void display_render_ascii(const uint8_t *fb, Str *out) {
    str_clear(out);
    for (int y = 0; y < CHIP8_H; y++) {
        for (int x = 0; x < CHIP8_W; x++) {
            str_push(out, fb[y * CHIP8_W + x] != 0 ? '#' : '.');
        }
        str_push(out, '\n');
    }
}
