// Framebuffer to text rendering for the CHIP-8 display. Pure: no terminal calls.

#pragma once

#include <stdint.h>

#include "cpu.h"

#include "../util/str.h"

// Clears out, then writes "\x1b[H" and 16 lines of 64 half block cells, each
// cell holding the two vertically stacked pixels of a row pair.
void display_render(const uint8_t *fb, Str *out);

// Clears out, then writes 32 lines of 64 '#' or '.', one pixel per character.
void display_render_ascii(const uint8_t *fb, Str *out);
