// Tests for the CHIP-8 CPU: every opcode, both quirk directions, and every bounds escape.

#include "cpu.h"

#include <string.h>

#include "../../tests/harness.h"

#define SCRATCH 0x300
#define FB_SIZE (CHIP8_W * CHIP8_H)

// Hand-assembled program: big-endian words dropped at the ROM base.
static void prog(Chip8 *c, const uint16_t *ops, size_t n) {
    cpu_init(c);
    for (size_t k = 0; k < n; k++) {
        c->mem[CHIP8_ROM_BASE + 2 * k] = (uint8_t)(ops[k] >> 8);
        c->mem[CHIP8_ROM_BASE + 2 * k + 1] = (uint8_t)(ops[k] & 0xFF);
    }
}

// Stops at the first non-OK step and returns it.
static NshError run(Chip8 *c, int steps) {
    NshError e = NSH_OK;
    for (int k = 0; k < steps; k++) {
        e = cpu_step(c);
        if (e != NSH_OK) {
            return e;
        }
    }
    return e;
}

static int px(const Chip8 *c, int x, int y) { return c->fb[y * CHIP8_W + x]; }

static int fb_lit(const Chip8 *c) {
    int n = 0;
    for (int k = 0; k < FB_SIZE; k++) {
        n += c->fb[k];
    }
    return n;
}

// Runs one opcode from a clean machine and returns its result.
static NshError one(Chip8 *c, uint16_t op) {
    prog(c, &op, 1);
    return cpu_step(c);
}

TEST(init_zeroes_state_and_loads_font) {
    Chip8 c;
    memset(&c, 0xAB, sizeof c);
    cpu_init(&c);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE);
    ASSERT_EQ(c.i, 0);
    ASSERT_EQ(c.sp, 0);
    ASSERT_EQ(c.delay, 0);
    ASSERT_EQ(c.sound, 0);
    ASSERT_EQ(c.keys, 0);
    ASSERT_TRUE(!c.waiting_key);
    ASSERT_EQ(c.waiting_reg, 0);
    ASSERT_TRUE(!c.fb_dirty);
    ASSERT_TRUE(c.rng != 0);
    for (int k = 0; k < 16; k++) {
        ASSERT_EQ(c.v[k], 0);
    }
    ASSERT_EQ(fb_lit(&c), 0);
    ASSERT_EQ(c.mem[CHIP8_FONT_BASE + 0], 0xF0);
    ASSERT_EQ(c.mem[CHIP8_FONT_BASE + 4], 0xF0);
    ASSERT_EQ(c.mem[CHIP8_FONT_BASE + 5], 0x20);
    ASSERT_EQ(c.mem[CHIP8_FONT_BASE + 5 * 0xF + 0], 0xF0);
    ASSERT_EQ(c.mem[CHIP8_FONT_BASE + 5 * 0xF + 4], 0x80);
    ASSERT_EQ(c.mem[CHIP8_FONT_BASE - 1], 0);
    ASSERT_EQ(c.mem[CHIP8_FONT_BASE + 80], 0);
    ASSERT_EQ(c.mem[CHIP8_ROM_BASE], 0);
}

TEST(init_is_idempotent) {
    Chip8 a;
    Chip8 b;
    cpu_init(&a);
    cpu_init(&b);
    uint16_t ops[] = {0x6042, 0xA300, 0xD005};
    for (size_t k = 0; k < 3; k++) {
        b.mem[CHIP8_ROM_BASE + 2 * k] = (uint8_t)(ops[k] >> 8);
        b.mem[CHIP8_ROM_BASE + 2 * k + 1] = (uint8_t)(ops[k] & 0xFF);
    }
    ASSERT_EQ(run(&b, 3), NSH_OK);
    cpu_init(&b);
    ASSERT_EQ(memcmp(&a, &b, sizeof a), 0);
}

TEST(load_rom_size_limits) {
    Chip8 c;
    static uint8_t rom[CHIP8_MEM];
    for (size_t k = 0; k < sizeof rom; k++) {
        rom[k] = (uint8_t)(k & 0xFF);
    }
    cpu_init(&c);
    ASSERT_EQ(cpu_load_rom(&c, rom, 0), NSH_ERR_INVALID);
    ASSERT_EQ(cpu_load_rom(&c, rom, CHIP8_MEM - CHIP8_ROM_BASE + 1), NSH_ERR_INVALID);
    ASSERT_EQ(cpu_load_rom(&c, rom, CHIP8_MEM), NSH_ERR_INVALID);
    ASSERT_EQ(c.mem[CHIP8_ROM_BASE], 0);
    ASSERT_EQ(cpu_load_rom(&c, rom, CHIP8_MEM - CHIP8_ROM_BASE), NSH_OK);
    ASSERT_EQ(c.mem[CHIP8_ROM_BASE], 0);
    ASSERT_EQ(c.mem[CHIP8_ROM_BASE + 1], 1);
    ASSERT_EQ(c.mem[CHIP8_MEM - 1], (CHIP8_MEM - CHIP8_ROM_BASE - 1) & 0xFF);
    ASSERT_EQ(c.mem[CHIP8_FONT_BASE], 0xF0);
    cpu_init(&c);
    ASSERT_EQ(cpu_load_rom(&c, rom, 3), NSH_OK);
    ASSERT_EQ(c.mem[CHIP8_ROM_BASE + 2], 2);
    ASSERT_EQ(c.mem[CHIP8_ROM_BASE + 3], 0);
}

TEST(cls_00e0_clears_and_dirties) {
    Chip8 c;
    uint16_t ops[] = {0xA050, 0x6000, 0x6100, 0xD015, 0x00E0};
    prog(&c, ops, 5);
    ASSERT_EQ(run(&c, 4), NSH_OK);
    ASSERT_TRUE(fb_lit(&c) > 0);
    c.fb_dirty = false;
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(fb_lit(&c), 0);
    ASSERT_TRUE(c.fb_dirty);
}

TEST(jump_1nnn) {
    Chip8 c;
    ASSERT_EQ(one(&c, 0x1ABC), NSH_OK);
    ASSERT_EQ(c.pc, 0xABC);
}

TEST(jump_to_last_byte_then_fetch_fails) {
    Chip8 c;
    ASSERT_EQ(one(&c, 0x1FFF), NSH_OK);
    ASSERT_EQ(c.pc, 0xFFF);
    ASSERT_EQ(cpu_step(&c), NSH_ERR_INVALID);
    ASSERT_EQ(c.pc, 0xFFF);
    c.pc = 0xFFE;
    ASSERT_EQ(cpu_step(&c), NSH_ERR_INVALID);
    ASSERT_EQ(c.pc, CHIP8_MEM);
}

TEST(call_ret_nesting_to_depth_16_then_overflow) {
    Chip8 c;
    uint16_t ops[] = {0x2200};
    prog(&c, ops, 1);
    ASSERT_EQ(run(&c, 16), NSH_OK);
    ASSERT_EQ(c.sp, 16);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE);
    for (int k = 0; k < 16; k++) {
        ASSERT_EQ(c.stack[k], CHIP8_ROM_BASE + 2);
    }
    ASSERT_EQ(cpu_step(&c), NSH_ERR_INVALID);
    ASSERT_EQ(c.sp, 16);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 2);
    c.mem[CHIP8_ROM_BASE + 2] = 0x00;
    c.mem[CHIP8_ROM_BASE + 3] = 0xEE;
    ASSERT_EQ(run(&c, 16), NSH_OK);
    ASSERT_EQ(c.sp, 0);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 2);
    ASSERT_EQ(cpu_step(&c), NSH_ERR_INVALID);
    ASSERT_EQ(c.sp, 0);
}

TEST(ret_underflow_is_invalid) {
    Chip8 c;
    ASSERT_EQ(one(&c, 0x00EE), NSH_ERR_INVALID);
    ASSERT_EQ(c.sp, 0);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 2);
}

TEST(call_and_ret_round_trip) {
    Chip8 c;
    uint16_t ops[] = {0x2206, 0x6002, 0x1204, 0x6101, 0x00EE};
    prog(&c, ops, 5);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.v[1], 1);
    ASSERT_EQ(c.sp, 1);
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.sp, 0);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 2);
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.v[0], 2);
}

TEST(skip_3xkk_4xkk) {
    Chip8 c;
    uint16_t ops[] = {0x6042, 0x3042, 0x6001, 0x3099, 0x6002, 0x4099, 0x6003, 0x4002};
    prog(&c, ops, 8);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 6);
    ASSERT_EQ(c.v[0], 0x42);
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 8);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.v[0], 2);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 14);
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 16);
    ASSERT_EQ(c.v[0], 2);
}

TEST(skip_5xy0_9xy0) {
    Chip8 c;
    uint16_t ops[] = {0x6007, 0x6107, 0x5010, 0x6300, 0x6208,
                      0x9020, 0x6301, 0x9010, 0x5020};
    prog(&c, ops, 9);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 8);
    ASSERT_EQ(c.v[3], 0);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.v[2], 8);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 14);
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 16);
    ASSERT_EQ(c.v[3], 0);
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 18);
}

TEST(skip_over_skip) {
    Chip8 c;
    uint16_t ops[] = {0x6000, 0x3000, 0x3000, 0x6002};
    prog(&c, ops, 4);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 6);
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.v[0], 2);
}

TEST(skip_past_memory_end_is_invalid) {
    Chip8 c;
    cpu_init(&c);
    c.pc = CHIP8_MEM - 4;
    c.mem[CHIP8_MEM - 4] = 0x30;
    c.mem[CHIP8_MEM - 3] = 0x00;
    ASSERT_EQ(cpu_step(&c), NSH_ERR_INVALID);
    ASSERT_EQ(c.pc, CHIP8_MEM - 2);
}

TEST(set_and_add_6xkk_7xkk) {
    Chip8 c;
    uint16_t ops[] = {0x60FF, 0x7002, 0x6F0A, 0x7F01};
    prog(&c, ops, 4);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.v[0], 0x01);
    ASSERT_EQ(c.v[0xF], 0);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.v[0xF], 0x0B);
}

TEST(alu_move_or_and_xor) {
    Chip8 c;
    uint16_t ops[] = {0x60F0, 0x610F, 0x8010, 0x60CC, 0x61AA, 0x8011,
                      0x60CC, 0x61AA, 0x8012, 0x60CC, 0x61AA, 0x8013};
    prog(&c, ops, 12);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 0x0F);
    ASSERT_EQ(c.v[1], 0x0F);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 0xEE);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 0x88);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 0x66);
    ASSERT_EQ(c.v[0xF], 0);
}

TEST(alu_add_carry_both_ways) {
    Chip8 c;
    uint16_t ops[] = {0x6001, 0x6102, 0x8014, 0x60FF, 0x6101, 0x8014};
    prog(&c, ops, 6);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 3);
    ASSERT_EQ(c.v[0xF], 0);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 0x00);
    ASSERT_EQ(c.v[0xF], 1);
}

TEST(alu_sub_borrow_both_ways) {
    Chip8 c;
    uint16_t ops[] = {0x6005, 0x6103, 0x8015, 0x6003, 0x6105, 0x8015};
    prog(&c, ops, 6);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 2);
    ASSERT_EQ(c.v[0xF], 1);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 0xFE);
    ASSERT_EQ(c.v[0xF], 0);
}

TEST(alu_subn_borrow_both_ways) {
    Chip8 c;
    uint16_t ops[] = {0x6003, 0x6105, 0x8017, 0x6005, 0x6103, 0x8017, 0x6004, 0x6104, 0x8017};
    prog(&c, ops, 9);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 2);
    ASSERT_EQ(c.v[0xF], 1);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 0xFE);
    ASSERT_EQ(c.v[0xF], 0);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 0);
    ASSERT_EQ(c.v[0xF], 1);
}

TEST(alu_shift_right_quirk_and_bit_out) {
    Chip8 c;
    uint16_t ops[] = {0x6003, 0x61FF, 0x8016, 0x6002, 0x61FF, 0x8016};
    prog(&c, ops, 6);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 1);
    ASSERT_EQ(c.v[0xF], 1);
    ASSERT_EQ(c.v[1], 0xFF);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 1);
    ASSERT_EQ(c.v[0xF], 0);
}

TEST(alu_shift_left_quirk_and_bit_out) {
    Chip8 c;
    uint16_t ops[] = {0x6081, 0x6101, 0x801E, 0x6041, 0x6101, 0x801E};
    prog(&c, ops, 6);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 0x02);
    ASSERT_EQ(c.v[0xF], 1);
    ASSERT_EQ(c.v[1], 1);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 0x82);
    ASSERT_EQ(c.v[0xF], 0);
}

TEST(alu_vf_as_destination_flag_wins) {
    Chip8 c;
    uint16_t ops[] = {0x6FFF, 0x6E01, 0x8FE4, 0x6F81, 0x6EFF, 0x8FE6,
                      0x6F81, 0x8FEE, 0x6F02, 0x6E05, 0x8FE5};
    prog(&c, ops, 11);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0xF], 1);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0xF], 1);
    ASSERT_EQ(c.v[0xE], 0xFF);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.v[0xF], 1);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0xF], 0);
}

TEST(alu_vf_as_source_reads_before_flag_write) {
    Chip8 c;
    uint16_t ops[] = {0x6001, 0x6F02, 0x80F4};
    prog(&c, ops, 3);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.v[0], 3);
    ASSERT_EQ(c.v[0xF], 0);
}

TEST(index_annn_and_fx1e) {
    Chip8 c;
    uint16_t ops[] = {0xA123, 0x60FF, 0x6F01, 0xF01E};
    prog(&c, ops, 4);
    ASSERT_EQ(run(&c, 4), NSH_OK);
    ASSERT_EQ(c.i, 0x222);
    ASSERT_EQ(c.v[0xF], 1);
}

TEST(jump_bnnn_adds_v0) {
    Chip8 c;
    uint16_t ops[] = {0x6010, 0xB300};
    prog(&c, ops, 2);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.pc, 0x310);
}

TEST(rand_cxkk_deterministic_and_masked) {
    Chip8 a;
    Chip8 b;
    uint16_t ops[] = {0xC0FF, 0xC1FF, 0xC2FF, 0xC300};
    prog(&a, ops, 4);
    prog(&b, ops, 4);
    ASSERT_EQ(run(&a, 4), NSH_OK);
    ASSERT_EQ(run(&b, 4), NSH_OK);
    ASSERT_EQ(memcmp(a.v, b.v, sizeof a.v), 0);
    ASSERT_EQ(a.rng, b.rng);
    ASSERT_EQ(a.v[3], 0);
    ASSERT_TRUE(a.v[0] != a.v[1] || a.v[1] != a.v[2]);
    uint16_t masked[] = {0xC00F, 0xC10F, 0xC20F};
    prog(&a, masked, 3);
    ASSERT_EQ(run(&a, 3), NSH_OK);
    ASSERT_TRUE(a.v[0] <= 0x0F);
    ASSERT_TRUE(a.v[1] <= 0x0F);
    ASSERT_TRUE(a.v[2] <= 0x0F);
}

TEST(draw_dxyn_collision_on_and_off) {
    Chip8 c;
    uint16_t ops[] = {0xA050, 0x6000, 0x6100, 0xD015, 0xD015};
    prog(&c, ops, 5);
    ASSERT_EQ(run(&c, 4), NSH_OK);
    ASSERT_TRUE(c.fb_dirty);
    ASSERT_EQ(c.v[0xF], 0);
    ASSERT_EQ(fb_lit(&c), 14);
    ASSERT_EQ(px(&c, 0, 0), 1);
    ASSERT_EQ(px(&c, 3, 0), 1);
    ASSERT_EQ(px(&c, 4, 0), 0);
    ASSERT_EQ(px(&c, 1, 1), 0);
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.v[0xF], 1);
    ASSERT_EQ(fb_lit(&c), 0);
}

TEST(draw_dxyn_partial_collision) {
    Chip8 c;
    uint16_t ops[] = {0xA300, 0x6000, 0x6100, 0xD011, 0x6001, 0xD011};
    prog(&c, ops, 6);
    c.mem[SCRATCH] = 0xC0;
    ASSERT_EQ(run(&c, 6), NSH_OK);
    ASSERT_EQ(c.v[0xF], 1);
    ASSERT_EQ(px(&c, 0, 0), 1);
    ASSERT_EQ(px(&c, 1, 0), 0);
    ASSERT_EQ(px(&c, 2, 0), 1);
    ASSERT_EQ(fb_lit(&c), 2);
}

TEST(draw_dxyn_clips_at_right_edge) {
    Chip8 c;
    uint16_t ops[] = {0xA300, 0x603C, 0x6100, 0xD011};
    prog(&c, ops, 4);
    c.mem[SCRATCH] = 0xFF;
    ASSERT_EQ(run(&c, 4), NSH_OK);
    for (int x = 60; x < 64; x++) {
        ASSERT_EQ(px(&c, x, 0), 1);
    }
    for (int x = 0; x < 4; x++) {
        ASSERT_EQ(px(&c, x, 0), 0);
    }
    ASSERT_EQ(fb_lit(&c), 4);
    ASSERT_EQ(c.v[0xF], 0);
}

TEST(draw_dxyn_clips_at_bottom_edge) {
    Chip8 c;
    uint16_t ops[] = {0xA300, 0x6000, 0x611E, 0xD015};
    prog(&c, ops, 4);
    for (int k = 0; k < 5; k++) {
        c.mem[SCRATCH + k] = 0xFF;
    }
    ASSERT_EQ(run(&c, 4), NSH_OK);
    ASSERT_EQ(fb_lit(&c), 16);
    for (int x = 0; x < 8; x++) {
        ASSERT_EQ(px(&c, x, 30), 1);
        ASSERT_EQ(px(&c, x, 31), 1);
        ASSERT_EQ(px(&c, x, 0), 0);
        ASSERT_EQ(px(&c, x, 1), 0);
    }
}

TEST(draw_dxyn_start_coordinate_wraps) {
    Chip8 c;
    uint16_t ops[] = {0xA300, 0x6044, 0x6122, 0xD011};
    prog(&c, ops, 4);
    c.mem[SCRATCH] = 0x80;
    ASSERT_EQ(run(&c, 4), NSH_OK);
    ASSERT_EQ(fb_lit(&c), 1);
    ASSERT_EQ(px(&c, 4, 2), 1);
}

TEST(draw_dxy0_draws_nothing) {
    Chip8 c;
    uint16_t ops[] = {0xA050, 0x6000, 0x6100, 0xD010};
    prog(&c, ops, 4);
    ASSERT_EQ(run(&c, 4), NSH_OK);
    ASSERT_EQ(fb_lit(&c), 0);
    ASSERT_EQ(c.v[0xF], 0);
    ASSERT_TRUE(c.fb_dirty);
}

TEST(draw_sprite_past_memory_end_is_invalid) {
    Chip8 c;
    uint16_t ops[] = {0x6000, 0x6100, 0xD015};
    prog(&c, ops, 3);
    c.i = CHIP8_MEM - 4;
    ASSERT_EQ(run(&c, 3), NSH_ERR_INVALID);
    ASSERT_EQ(fb_lit(&c), 0);
    ASSERT_TRUE(!c.fb_dirty);
    prog(&c, ops, 3);
    c.mem[CHIP8_MEM - 5] = 0x80;
    c.i = CHIP8_MEM - 5;
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(px(&c, 0, 0), 1);
}

TEST(key_skips_with_multiple_keys_held) {
    Chip8 c;
    uint16_t ops[] = {0x6001, 0x6109, 0x6205, 0xE09E, 0x0000, 0xE29E,
                      0xE0A1, 0xE2A1, 0x0000};
    prog(&c, ops, 9);
    cpu_key_event(&c, 1, true);
    cpu_key_event(&c, 9, true);
    ASSERT_EQ(c.keys, 0x0202);
    ASSERT_EQ(run(&c, 4), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 10);
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 12);
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 14);
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 18);
    cpu_key_event(&c, 1, false);
    ASSERT_EQ(c.keys, 0x0200);
    c.pc = CHIP8_ROM_BASE + 6;
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 8);
}

TEST(key_event_ignores_out_of_range) {
    Chip8 c;
    cpu_init(&c);
    cpu_key_event(&c, -1, true);
    cpu_key_event(&c, 16, true);
    ASSERT_EQ(c.keys, 0);
    cpu_key_event(&c, 15, true);
    ASSERT_EQ(c.keys, 0x8000);
}

TEST(fx0a_latch_blocks_until_press) {
    Chip8 c;
    uint16_t ops[] = {0xF00A, 0x6105};
    prog(&c, ops, 2);
    ASSERT_EQ(cpu_step(&c), NSH_OK);
    ASSERT_TRUE(c.waiting_key);
    ASSERT_EQ(c.waiting_reg, 0);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 2);
    ASSERT_EQ(cpu_step(&c), NSH_OK);
    ASSERT_EQ(cpu_step(&c), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 2);
    ASSERT_EQ(c.v[1], 0);
    cpu_key_event(&c, 7, false);
    ASSERT_TRUE(c.waiting_key);
    ASSERT_EQ(cpu_step(&c), NSH_OK);
    ASSERT_EQ(c.v[0], 0);
    cpu_key_event(&c, 7, true);
    ASSERT_TRUE(!c.waiting_key);
    ASSERT_EQ(c.v[0], 7);
    ASSERT_EQ(cpu_step(&c), NSH_OK);
    ASSERT_EQ(c.v[1], 5);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 4);
}

TEST(fx0a_latch_targets_its_register) {
    Chip8 c;
    uint16_t ops[] = {0xFA0A};
    prog(&c, ops, 1);
    ASSERT_EQ(cpu_step(&c), NSH_OK);
    ASSERT_EQ(c.waiting_reg, 0xA);
    cpu_key_event(&c, 3, true);
    ASSERT_EQ(c.v[0xA], 3);
    ASSERT_TRUE(!c.waiting_key);
}

TEST(timers_set_read_and_decrement_to_zero) {
    Chip8 c;
    uint16_t ops[] = {0x6003, 0xF015, 0xF018, 0xF107};
    prog(&c, ops, 4);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.delay, 3);
    ASSERT_EQ(c.sound, 3);
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.v[1], 3);
    cpu_tick_timers(&c);
    ASSERT_EQ(c.delay, 2);
    ASSERT_EQ(c.sound, 2);
    cpu_tick_timers(&c);
    cpu_tick_timers(&c);
    ASSERT_EQ(c.delay, 0);
    ASSERT_EQ(c.sound, 0);
    cpu_tick_timers(&c);
    cpu_tick_timers(&c);
    ASSERT_EQ(c.delay, 0);
    ASSERT_EQ(c.sound, 0);
}

TEST(fx29_font_address) {
    Chip8 c;
    uint16_t ops[] = {0x600A, 0xF029, 0x611A, 0xF129, 0x6200, 0xF229};
    prog(&c, ops, 6);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.i, CHIP8_FONT_BASE + 5 * 0xA);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.i, CHIP8_FONT_BASE + 5 * 0xA);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.i, CHIP8_FONT_BASE);
}

TEST(fx33_bcd_edges) {
    Chip8 c;
    uint16_t ops[] = {0xA300, 0x6000, 0xF033, 0x60FF, 0xF033, 0x607B, 0xF033};
    prog(&c, ops, 7);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.mem[SCRATCH], 0);
    ASSERT_EQ(c.mem[SCRATCH + 1], 0);
    ASSERT_EQ(c.mem[SCRATCH + 2], 0);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.mem[SCRATCH], 2);
    ASSERT_EQ(c.mem[SCRATCH + 1], 5);
    ASSERT_EQ(c.mem[SCRATCH + 2], 5);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.mem[SCRATCH], 1);
    ASSERT_EQ(c.mem[SCRATCH + 1], 2);
    ASSERT_EQ(c.mem[SCRATCH + 2], 3);
    ASSERT_EQ(c.i, SCRATCH);
}

TEST(fx33_past_memory_end_is_invalid) {
    Chip8 c;
    uint16_t ops[] = {0x60FF, 0xF033};
    prog(&c, ops, 2);
    c.i = CHIP8_MEM - 2;
    ASSERT_EQ(run(&c, 2), NSH_ERR_INVALID);
    ASSERT_EQ(c.mem[CHIP8_MEM - 2], 0);
    ASSERT_EQ(c.mem[CHIP8_MEM - 1], 0);
    prog(&c, ops, 2);
    c.i = CHIP8_MEM - 3;
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.mem[CHIP8_MEM - 1], 5);
}

TEST(fx55_fx65_round_trip_x0) {
    Chip8 c;
    uint16_t ops[] = {0xA300, 0x60AB, 0xF055, 0x6000, 0xF065};
    prog(&c, ops, 5);
    c.mem[SCRATCH + 1] = 0x5A;
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.mem[SCRATCH], 0xAB);
    ASSERT_EQ(c.mem[SCRATCH + 1], 0x5A);
    ASSERT_EQ(c.i, SCRATCH);
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.v[0], 0xAB);
    ASSERT_EQ(c.v[1], 0);
    ASSERT_EQ(c.i, SCRATCH);
}

TEST(fx55_fx65_round_trip_x15) {
    Chip8 c;
    uint16_t ops[] = {0xA300, 0xFF55};
    prog(&c, ops, 2);
    for (int k = 0; k < 16; k++) {
        c.v[k] = (uint8_t)(0x10 + k);
    }
    ASSERT_EQ(run(&c, 2), NSH_OK);
    ASSERT_EQ(c.i, SCRATCH);
    for (int k = 0; k < 16; k++) {
        ASSERT_EQ(c.mem[SCRATCH + k], 0x10 + k);
    }
    ASSERT_EQ(c.mem[SCRATCH + 16], 0);
    memset(c.v, 0, sizeof c.v);
    c.mem[CHIP8_ROM_BASE + 4] = 0xFF;
    c.mem[CHIP8_ROM_BASE + 5] = 0x65;
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.i, SCRATCH);
    for (int k = 0; k < 16; k++) {
        ASSERT_EQ(c.v[k], 0x10 + k);
    }
}

TEST(fx55_fx65_bounds) {
    Chip8 c;
    uint16_t store[] = {0xFF55};
    uint16_t load[] = {0xFF65};
    prog(&c, store, 1);
    c.i = CHIP8_MEM - 15;
    ASSERT_EQ(run(&c, 1), NSH_ERR_INVALID);
    ASSERT_EQ(c.mem[CHIP8_MEM - 15], 0);
    prog(&c, store, 1);
    c.v[0] = 0x77;
    c.i = CHIP8_MEM - 16;
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.mem[CHIP8_MEM - 16], 0x77);
    prog(&c, load, 1);
    c.i = CHIP8_MEM - 15;
    ASSERT_EQ(run(&c, 1), NSH_ERR_INVALID);
    prog(&c, load, 1);
    c.mem[CHIP8_MEM - 1] = 0x99;
    c.i = CHIP8_MEM - 16;
    ASSERT_EQ(run(&c, 1), NSH_OK);
    ASSERT_EQ(c.v[15], 0x99);
}

TEST(unknown_opcodes_are_invalid) {
    Chip8 c;
    uint16_t bad[] = {0x0123, 0x00E1, 0x00FF, 0x5121, 0x9121,
                      0x8128, 0x8129, 0x812F, 0xE012, 0xE0A2, 0xF000, 0xF066};
    for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        ASSERT_EQ(one(&c, bad[k]), NSH_ERR_INVALID);
        ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 2);
        ASSERT_EQ(c.sp, 0);
        ASSERT_EQ(c.i, 0);
    }
}

TEST(waiting_latch_makes_step_a_noop_even_on_bad_opcode) {
    Chip8 c;
    uint16_t ops[] = {0xF00A, 0x0123};
    prog(&c, ops, 2);
    ASSERT_EQ(run(&c, 3), NSH_OK);
    ASSERT_EQ(c.pc, CHIP8_ROM_BASE + 2);
}

TEST(draws_font_digit_one_at_origin) {
    Chip8 c;
    uint16_t ops[] = {0x00E0, 0x6001, 0xF029, 0x6100, 0x6200, 0xD125};
    prog(&c, ops, 6);
    ASSERT_EQ(run(&c, 6), NSH_OK);
    ASSERT_EQ(c.i, CHIP8_FONT_BASE + 5);
    ASSERT_EQ(c.v[0xF], 0);
    ASSERT_TRUE(c.fb_dirty);
    static const uint8_t want[5] = {0x20, 0x60, 0x20, 0x20, 0x70};
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 8; x++) {
            ASSERT_EQ(px(&c, x, y), (want[y] >> (7 - x)) & 1);
        }
    }
    ASSERT_EQ(fb_lit(&c), 8);
    for (int x = 8; x < CHIP8_W; x++) {
        ASSERT_EQ(px(&c, x, 0), 0);
    }
    for (int y = 5; y < CHIP8_H; y++) {
        ASSERT_EQ(px(&c, 2, y), 0);
    }
}

TEST_MAIN()
