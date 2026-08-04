// Keyboard character to CHIP-8 hex key mapping. Pure: no input, no state.

#pragma once

// Returns the CHIP-8 key 0..15 for ch, or -1 when ch is not a keypad key.
// Layout is 1234 / qwer / asdf / zxcv, either case.
int keypad_map(unsigned char ch);
