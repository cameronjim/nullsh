// CHIP-8 interpreter state and stepping. Pure logic: no I/O, no time, no rand.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../util/error.h"

#define CHIP8_MEM 4096
#define CHIP8_W 64
#define CHIP8_H 32
#define CHIP8_ROM_BASE 0x200
#define CHIP8_FONT_BASE 0x50

// Quirks, fixed for nullsh: 8xy6/8xyE shift VX (VY ignored), Fx55/Fx65 leave I
// unchanged, BNNN jumps to NNN+V0, sprites clip at the edges (start wraps).
typedef struct {
    uint8_t mem[CHIP8_MEM];
    uint8_t v[16];
    uint16_t i;
    uint16_t pc;
    uint16_t stack[16];
    uint8_t sp;
    uint8_t delay;
    uint8_t sound;
    uint8_t fb[CHIP8_W * CHIP8_H];  // one byte per pixel, 0 or 1
    bool fb_dirty;                  // set by 00E0 and DXYN, cleared by the renderer
    uint16_t keys;                  // bit N set while hex key N is held
    bool waiting_key;               // Fx0A latch: true until the next key press
    uint8_t waiting_reg;
    uint32_t rng;                   // xorshift32 state; cpu_init seeds a constant
} Chip8;

// Zeroes everything, loads the font at CHIP8_FONT_BASE, sets pc to CHIP8_ROM_BASE.
void cpu_init(Chip8 *c);

// Copies rom to CHIP8_ROM_BASE. NSH_ERR_INVALID when it does not fit or len is 0.
NshError cpu_load_rom(Chip8 *c, const uint8_t *rom, size_t len);

// One fetch-decode-execute. A waiting Fx0A latch makes it a no-op. Returns
// NSH_ERR_INVALID for unknown opcodes, pc/stack escapes, or i escapes on use.
NshError cpu_step(Chip8 *c);

// One 60 Hz tick: decrements delay and sound toward zero.
void cpu_tick_timers(Chip8 *c);

// key is 0..15. A press satisfies a waiting Fx0A latch.
void cpu_key_event(Chip8 *c, int key, bool down);
