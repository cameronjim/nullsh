// Tests for the framebuffer renderers: half block cells and the ascii format.

#define _POSIX_C_SOURCE 200809L

#include "display.h"

#include <string.h>

#include "../../tests/harness.h"

#define HOME "\x1b[H"
#define HOME_LEN 3

#define FULL "\xe2\x96\x88"
#define UPPER "\xe2\x96\x80"
#define LOWER "\xe2\x96\x84"
#define CELL_LEN 3

#define PAIRS (CHIP8_H / 2)
#define BLANK_LINE_LEN (CHIP8_W + 1)
#define ASCII_LINE_LEN (CHIP8_W + 1)

static uint8_t fb[CHIP8_W * CHIP8_H];

static void fb_reset(void) { memset(fb, 0, sizeof fb); }

static void fb_set(int x, int y) { fb[y * CHIP8_W + x] = 1; }

TEST(all_off_renders_blank_lines) {
    fb_reset();
    Str s;
    str_init(&s);
    display_render(fb, &s);

    ASSERT_EQ(s.len, HOME_LEN + PAIRS * BLANK_LINE_LEN);
    ASSERT_TRUE(memcmp(s.data, HOME, HOME_LEN) == 0);
    for (int line = 0; line < PAIRS; line++) {
        const char *p = s.data + HOME_LEN + line * BLANK_LINE_LEN;
        for (int x = 0; x < CHIP8_W; x++) {
            ASSERT_EQ(p[x], ' ');
        }
        ASSERT_EQ(p[CHIP8_W], '\n');
    }
    ASSERT_EQ(s.data[s.len], '\0');
    str_free(&s);
}

TEST(top_pixel_is_upper_half) {
    fb_reset();
    fb_set(0, 0);
    Str s;
    str_init(&s);
    display_render(fb, &s);

    ASSERT_TRUE(memcmp(s.data, HOME UPPER, HOME_LEN + CELL_LEN) == 0);
    // The rest of the pair line is 63 spaces and a newline.
    const char *rest = s.data + HOME_LEN + CELL_LEN;
    for (int x = 1; x < CHIP8_W; x++) {
        ASSERT_EQ(rest[x - 1], ' ');
    }
    ASSERT_EQ(rest[CHIP8_W - 1], '\n');
    ASSERT_EQ(s.len,
              HOME_LEN + CELL_LEN + (CHIP8_W - 1) + 1 +
                  (PAIRS - 1) * BLANK_LINE_LEN);
    str_free(&s);
}

TEST(bottom_pixel_is_lower_half) {
    fb_reset();
    fb_set(0, 1);
    Str s;
    str_init(&s);
    display_render(fb, &s);

    ASSERT_TRUE(memcmp(s.data, HOME LOWER, HOME_LEN + CELL_LEN) == 0);
    str_free(&s);
}

TEST(both_pixels_are_full_block) {
    fb_reset();
    fb_set(0, 0);
    fb_set(0, 1);
    Str s;
    str_init(&s);
    display_render(fb, &s);

    ASSERT_TRUE(memcmp(s.data, HOME FULL, HOME_LEN + CELL_LEN) == 0);
    str_free(&s);
}

TEST(pixel_lands_on_its_own_pair_line) {
    fb_reset();
    fb_set(5, 2);
    Str s;
    str_init(&s);
    display_render(fb, &s);

    // Pair line 0 is blank, so line 1 starts one blank line in.
    const char *line = s.data + HOME_LEN + BLANK_LINE_LEN;
    for (int x = 0; x < 5; x++) {
        ASSERT_EQ(line[x], ' ');
    }
    ASSERT_TRUE(memcmp(line + 5, UPPER, CELL_LEN) == 0);
    ASSERT_EQ(line[5 + CELL_LEN + (CHIP8_W - 6)], '\n');
    str_free(&s);
}

TEST(checkerboard_row_pair_alternates_halves) {
    fb_reset();
    for (int x = 0; x < CHIP8_W; x++) {
        fb_set(x, x % 2 == 0 ? 0 : 1);
    }
    Str s;
    str_init(&s);
    display_render(fb, &s);

    const char *line = s.data + HOME_LEN;
    for (int x = 0; x < CHIP8_W; x++) {
        const char *want = (x % 2 == 0) ? UPPER : LOWER;
        ASSERT_TRUE(memcmp(line + x * CELL_LEN, want, CELL_LEN) == 0);
    }
    ASSERT_EQ(line[CHIP8_W * CELL_LEN], '\n');
    ASSERT_EQ(s.len,
              HOME_LEN + CHIP8_W * CELL_LEN + 1 + (PAIRS - 1) * BLANK_LINE_LEN);
    str_free(&s);
}

TEST(render_clears_the_output_first) {
    fb_reset();
    Str s;
    str_init(&s);
    str_append(&s, "stale");
    display_render(fb, &s);

    ASSERT_TRUE(memcmp(s.data, HOME, HOME_LEN) == 0);
    ASSERT_EQ(s.len, HOME_LEN + PAIRS * BLANK_LINE_LEN);
    str_free(&s);
}

TEST(ascii_corners_and_layout) {
    fb_reset();
    fb_set(0, 0);
    fb_set(CHIP8_W - 1, CHIP8_H - 1);
    Str s;
    str_init(&s);
    display_render_ascii(fb, &s);

    ASSERT_EQ(s.len, CHIP8_H * ASCII_LINE_LEN);
    ASSERT_TRUE(memchr(s.data, '\x1b', s.len) == NULL);
    for (int y = 0; y < CHIP8_H; y++) {
        const char *line = s.data + y * ASCII_LINE_LEN;
        for (int x = 0; x < CHIP8_W; x++) {
            char want =
                ((x == 0 && y == 0) ||
                 (x == CHIP8_W - 1 && y == CHIP8_H - 1))
                    ? '#'
                    : '.';
            ASSERT_EQ(line[x], want);
        }
        ASSERT_EQ(line[CHIP8_W], '\n');
    }
    str_free(&s);
}

TEST(ascii_clears_the_output_first) {
    fb_reset();
    fb_set(3, 4);
    Str s;
    str_init(&s);
    display_render_ascii(fb, &s);
    fb_reset();
    display_render_ascii(fb, &s);

    ASSERT_EQ(s.len, CHIP8_H * ASCII_LINE_LEN);
    ASSERT_TRUE(memchr(s.data, '#', s.len) == NULL);
    str_free(&s);
}

TEST_MAIN()
