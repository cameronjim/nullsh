// CHIP-8 CPU: fetch, decode, execute over Chip8 state. No I/O, no time, no libc rand.

#include "cpu.h"

#include <string.h>

#define OP_BYTES 2
#define GLYPH_BYTES 5
#define FONT_BYTES (16 * GLYPH_BYTES)
#define STACK_DEPTH 16
#define RNG_SEED 0x13375EEDu
#define VF 0xF

static const uint8_t k_font[FONT_BYTES] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0,  // 0
    0x20, 0x60, 0x20, 0x20, 0x70,  // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0,  // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0,  // 3
    0x90, 0x90, 0xF0, 0x10, 0x10,  // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0,  // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0,  // 6
    0xF0, 0x10, 0x20, 0x40, 0x40,  // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0,  // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0,  // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90,  // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0,  // B
    0xF0, 0x80, 0x80, 0x80, 0xF0,  // C
    0xE0, 0x90, 0x90, 0x90, 0xE0,  // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0,  // E
    0xF0, 0x80, 0xF0, 0x80, 0x80,  // F
};

// A fetch needs both opcode bytes inside memory.
static bool fetch_ok(uint16_t pc) { return (uint32_t)pc + 1 < CHIP8_MEM; }

// Inclusive span check for every i-relative access; i itself may sit past the end.
static bool span_ok(uint16_t base, uint16_t len) {
    return (uint32_t)base + len <= CHIP8_MEM;
}

static uint32_t next_rand(Chip8 *c) {
    uint32_t x = c->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    c->rng = x;
    return x;
}

static NshError skip_next(Chip8 *c, bool cond) {
    if (!cond) {
        return NSH_OK;
    }
    uint16_t target = (uint16_t)(c->pc + OP_BYTES);
    if (!fetch_ok(target)) {
        return NSH_ERR_INVALID;
    }
    c->pc = target;
    return NSH_OK;
}

static NshError exec_system(Chip8 *c, uint16_t op) {
    if (op == 0x00E0) {
        memset(c->fb, 0, sizeof c->fb);
        c->fb_dirty = true;
        return NSH_OK;
    }
    if (op == 0x00EE) {
        if (c->sp == 0) {
            return NSH_ERR_INVALID;
        }
        c->pc = c->stack[--c->sp];
        return NSH_OK;
    }
    return NSH_ERR_INVALID;
}

// VF is written after the result so 8xyN with x == 0xF yields the flag, not the value.
static NshError exec_alu(Chip8 *c, uint8_t x, uint8_t y, uint8_t n) {
    uint8_t a = c->v[x];
    uint8_t b = c->v[y];
    uint8_t res;
    uint8_t flag;
    switch (n) {
    case 0x0: c->v[x] = b; return NSH_OK;
    case 0x1: c->v[x] = (uint8_t)(a | b); return NSH_OK;
    case 0x2: c->v[x] = (uint8_t)(a & b); return NSH_OK;
    case 0x3: c->v[x] = (uint8_t)(a ^ b); return NSH_OK;
    case 0x4:
        res = (uint8_t)(a + b);
        flag = (uint16_t)(a + b) > 0xFF ? 1 : 0;
        break;
    case 0x5:
        res = (uint8_t)(a - b);
        flag = a >= b ? 1 : 0;
        break;
    case 0x6:
        res = (uint8_t)(a >> 1);
        flag = (uint8_t)(a & 1);
        break;
    case 0x7:
        res = (uint8_t)(b - a);
        flag = b >= a ? 1 : 0;
        break;
    case 0xE:
        res = (uint8_t)(a << 1);
        flag = (uint8_t)((a >> 7) & 1);
        break;
    default: return NSH_ERR_INVALID;
    }
    c->v[x] = res;
    c->v[VF] = flag;
    return NSH_OK;
}

// Start coords wrap modulo the screen; sprite bytes and bits past an edge are clipped.
static NshError exec_draw(Chip8 *c, uint8_t x, uint8_t y, uint8_t n) {
    if (n > 0 && !span_ok(c->i, n)) {
        return NSH_ERR_INVALID;
    }
    uint8_t x0 = (uint8_t)(c->v[x] % CHIP8_W);
    uint8_t y0 = (uint8_t)(c->v[y] % CHIP8_H);
    uint8_t hit = 0;
    for (uint8_t row = 0; row < n; row++) {
        uint8_t py = (uint8_t)(y0 + row);
        if (py >= CHIP8_H) {
            break;
        }
        uint8_t bits = c->mem[c->i + row];
        for (uint8_t col = 0; col < 8; col++) {
            uint8_t px = (uint8_t)(x0 + col);
            if (px >= CHIP8_W) {
                break;
            }
            if (((bits >> (7 - col)) & 1) == 0) {
                continue;
            }
            uint8_t *p = &c->fb[py * CHIP8_W + px];
            hit |= *p;
            *p ^= 1;
        }
    }
    c->v[VF] = hit ? 1 : 0;
    c->fb_dirty = true;
    return NSH_OK;
}

static NshError exec_key(Chip8 *c, uint8_t x, uint8_t kk) {
    bool held = (c->keys >> (c->v[x] & 0xF)) & 1;
    if (kk == 0x9E) {
        return skip_next(c, held);
    }
    if (kk == 0xA1) {
        return skip_next(c, !held);
    }
    return NSH_ERR_INVALID;
}

static NshError exec_misc(Chip8 *c, uint8_t x, uint8_t kk) {
    switch (kk) {
    case 0x07: c->v[x] = c->delay; return NSH_OK;
    case 0x0A:
        c->waiting_key = true;
        c->waiting_reg = x;
        return NSH_OK;
    case 0x15: c->delay = c->v[x]; return NSH_OK;
    case 0x18: c->sound = c->v[x]; return NSH_OK;
    case 0x1E: c->i = (uint16_t)(c->i + c->v[x]); return NSH_OK;
    case 0x29:
        c->i = (uint16_t)(CHIP8_FONT_BASE + GLYPH_BYTES * (c->v[x] & 0xF));
        return NSH_OK;
    case 0x33:
        if (!span_ok(c->i, 3)) {
            return NSH_ERR_INVALID;
        }
        c->mem[c->i] = (uint8_t)(c->v[x] / 100);
        c->mem[c->i + 1] = (uint8_t)(c->v[x] / 10 % 10);
        c->mem[c->i + 2] = (uint8_t)(c->v[x] % 10);
        return NSH_OK;
    case 0x55:
        if (!span_ok(c->i, (uint16_t)(x + 1))) {
            return NSH_ERR_INVALID;
        }
        memcpy(&c->mem[c->i], c->v, (size_t)x + 1);
        return NSH_OK;
    case 0x65:
        if (!span_ok(c->i, (uint16_t)(x + 1))) {
            return NSH_ERR_INVALID;
        }
        memcpy(c->v, &c->mem[c->i], (size_t)x + 1);
        return NSH_OK;
    default: return NSH_ERR_INVALID;
    }
}

static NshError exec(Chip8 *c, uint16_t op) {
    uint8_t x = (uint8_t)((op >> 8) & 0xF);
    uint8_t y = (uint8_t)((op >> 4) & 0xF);
    uint8_t n = (uint8_t)(op & 0xF);
    uint8_t kk = (uint8_t)(op & 0xFF);
    uint16_t nnn = (uint16_t)(op & 0x0FFF);
    switch (op >> 12) {
    case 0x0: return exec_system(c, op);
    case 0x1: c->pc = nnn; return NSH_OK;
    case 0x2:
        if (c->sp >= STACK_DEPTH) {
            return NSH_ERR_INVALID;
        }
        c->stack[c->sp++] = c->pc;
        c->pc = nnn;
        return NSH_OK;
    case 0x3: return skip_next(c, c->v[x] == kk);
    case 0x4: return skip_next(c, c->v[x] != kk);
    case 0x5: return n == 0 ? skip_next(c, c->v[x] == c->v[y]) : NSH_ERR_INVALID;
    case 0x6: c->v[x] = kk; return NSH_OK;
    case 0x7: c->v[x] = (uint8_t)(c->v[x] + kk); return NSH_OK;
    case 0x8: return exec_alu(c, x, y, n);
    case 0x9: return n == 0 ? skip_next(c, c->v[x] != c->v[y]) : NSH_ERR_INVALID;
    case 0xA: c->i = nnn; return NSH_OK;
    // BNNN is NNN+V0 unmasked; an escape is caught by the next fetch.
    case 0xB: c->pc = (uint16_t)(nnn + c->v[0]); return NSH_OK;
    case 0xC: c->v[x] = (uint8_t)(next_rand(c) & kk); return NSH_OK;
    case 0xD: return exec_draw(c, x, y, n);
    case 0xE: return exec_key(c, x, kk);
    default: return exec_misc(c, x, kk);
    }
}

void cpu_init(Chip8 *c) {
    memset(c, 0, sizeof *c);
    memcpy(&c->mem[CHIP8_FONT_BASE], k_font, sizeof k_font);
    c->pc = CHIP8_ROM_BASE;
    c->rng = RNG_SEED;
}

NshError cpu_load_rom(Chip8 *c, const uint8_t *rom, size_t len) {
    if (rom == NULL || len == 0 ||
        len > (size_t)(CHIP8_MEM - CHIP8_ROM_BASE)) {
        return NSH_ERR_INVALID;
    }
    memcpy(&c->mem[CHIP8_ROM_BASE], rom, len);
    return NSH_OK;
}

NshError cpu_step(Chip8 *c) {
    if (c->waiting_key) {
        return NSH_OK;
    }
    if (!fetch_ok(c->pc)) {
        return NSH_ERR_INVALID;
    }
    uint16_t op = (uint16_t)((c->mem[c->pc] << 8) | c->mem[c->pc + 1]);
    c->pc = (uint16_t)(c->pc + OP_BYTES);
    return exec(c, op);
}

void cpu_tick_timers(Chip8 *c) {
    if (c->delay > 0) {
        c->delay--;
    }
    if (c->sound > 0) {
        c->sound--;
    }
}

void cpu_key_event(Chip8 *c, int key, bool down) {
    if (key < 0 || key > 15) {
        return;
    }
    uint16_t bit = (uint16_t)(1u << key);
    if (down) {
        c->keys |= bit;
        if (c->waiting_key) {
            c->v[c->waiting_reg] = (uint8_t)key;
            c->waiting_key = false;
        }
        return;
    }
    c->keys = (uint16_t)(c->keys & ~bit);
}
