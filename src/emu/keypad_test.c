// Tests for the keypad mapping: both cases, rejects, and the whole byte range.

#define _POSIX_C_SOURCE 200809L

#include "keypad.h"

#include <string.h>

#include "../../tests/harness.h"

// 1234 / qwer / asdf / zxcv in layout order, paired with the hex key.
static const struct {
    char ch;
    int key;
} mapping[] = {
    {'1', 0x1}, {'2', 0x2}, {'3', 0x3}, {'4', 0xC},
    {'q', 0x4}, {'w', 0x5}, {'e', 0x6}, {'r', 0xD},
    {'a', 0x7}, {'s', 0x8}, {'d', 0x9}, {'f', 0xE},
    {'z', 0xA}, {'x', 0x0}, {'c', 0xB}, {'v', 0xF},
};

#define MAPPING_COUNT ((int)(sizeof mapping / sizeof mapping[0]))

// The four digits have one form, the twelve letters have two.
#define MAPPED_CHARS (4 + 12 * 2)

static unsigned char to_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (unsigned char)(c - 'a' + 'A');
    }
    return (unsigned char)c;
}

TEST(lowercase_layout_maps) {
    for (int i = 0; i < MAPPING_COUNT; i++) {
        ASSERT_EQ(keypad_map((unsigned char)mapping[i].ch), mapping[i].key);
    }
}

TEST(uppercase_layout_maps) {
    for (int i = 0; i < MAPPING_COUNT; i++) {
        ASSERT_EQ(keypad_map(to_upper(mapping[i].ch)), mapping[i].key);
    }
}

TEST(every_hex_key_is_reachable) {
    int seen[16] = {0};
    for (int i = 0; i < MAPPING_COUNT; i++) {
        seen[mapping[i].key] = 1;
    }
    for (int k = 0; k < 16; k++) {
        ASSERT_EQ(seen[k], 1);
    }
}

TEST(unmapped_chars_are_rejected) {
    static const unsigned char rejects[] = {
        27, '\n', '\r', ' ', 0, '5', '6', '7', '8', '9', '0',
        't', 'y', 'u', 'i', 'o', 'p', 'g', 'h', 'j', 'k', 'l',
        'b', 'n', 'm', 'T', 'B', '!', '@', 127, 128, 255,
    };
    for (size_t i = 0; i < sizeof rejects / sizeof rejects[0]; i++) {
        ASSERT_EQ(keypad_map(rejects[i]), -1);
    }
}

TEST(exhaustive_byte_range) {
    int hits = 0;
    for (int c = 0; c <= 255; c++) {
        int key = keypad_map((unsigned char)c);
        if (key == -1) {
            continue;
        }
        hits++;
        ASSERT_TRUE(key >= 0 && key <= 15);
    }
    ASSERT_EQ(hits, MAPPED_CHARS);
}

TEST_MAIN()
