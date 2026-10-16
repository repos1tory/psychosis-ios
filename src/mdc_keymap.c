// mdc_keymap.c - deterministic ASCII -> HID usage map for the `type` command.
// Pure C so it can be unit tested on any host.
#include "mdc_keymap.h"

#include <stddef.h>

typedef struct {
    char c;
    unsigned short usage;
    unsigned char shift;
} MdcKeyMapEntry;

static const MdcKeyMapEntry kMap[] = {
    /* letters */
    {'a', 0x04, 0}, {'b', 0x05, 0}, {'c', 0x06, 0}, {'d', 0x07, 0},
    {'e', 0x08, 0}, {'f', 0x09, 0}, {'g', 0x0A, 0}, {'h', 0x0B, 0},
    {'i', 0x0C, 0}, {'j', 0x0D, 0}, {'k', 0x0E, 0}, {'l', 0x0F, 0},
    {'m', 0x10, 0}, {'n', 0x11, 0}, {'o', 0x12, 0}, {'p', 0x13, 0},
    {'q', 0x14, 0}, {'r', 0x15, 0}, {'s', 0x16, 0}, {'t', 0x17, 0},
    {'u', 0x18, 0}, {'v', 0x19, 0}, {'w', 0x1A, 0}, {'x', 0x1B, 0},
    {'y', 0x1C, 0}, {'z', 0x1D, 0},
    {'A', 0x04, 1}, {'B', 0x05, 1}, {'C', 0x06, 1}, {'D', 0x07, 1},
    {'E', 0x08, 1}, {'F', 0x09, 1}, {'G', 0x0A, 1}, {'H', 0x0B, 1},
    {'I', 0x0C, 1}, {'J', 0x0D, 1}, {'K', 0x0E, 1}, {'L', 0x0F, 1},
    {'M', 0x10, 1}, {'N', 0x11, 1}, {'O', 0x12, 1}, {'P', 0x13, 1},
    {'Q', 0x14, 1}, {'R', 0x15, 1}, {'S', 0x16, 1}, {'T', 0x17, 1},
    {'U', 0x18, 1}, {'V', 0x19, 1}, {'W', 0x1A, 1}, {'X', 0x1B, 1},
    {'Y', 0x1C, 1}, {'Z', 0x1D, 1},
    /* digits and their shifted symbols */
    {'1', 0x1E, 0}, {'2', 0x1F, 0}, {'3', 0x20, 0}, {'4', 0x21, 0},
    {'5', 0x22, 0}, {'6', 0x23, 0}, {'7', 0x24, 0}, {'8', 0x25, 0},
    {'9', 0x26, 0}, {'0', 0x27, 0},
    {'!', 0x1E, 1}, {'@', 0x1F, 1}, {'#', 0x20, 1}, {'$', 0x21, 1},
    {'%', 0x22, 1}, {'^', 0x23, 1}, {'&', 0x24, 1}, {'*', 0x25, 1},
    {'(', 0x26, 1}, {')', 0x27, 1},
    /* whitespace and punctuation */
    {' ',  0x2C, 0},
    {'\n', 0x28, 0}, {'\t', 0x2B, 0},
    {'-', 0x2D, 0}, {'_', 0x2D, 1},
    {'=', 0x2E, 0}, {'+', 0x2E, 1},
    {'[', 0x2F, 0}, {'{', 0x2F, 1},
    {']', 0x30, 0}, {'}', 0x30, 1},
    {'\\', 0x31, 0}, {'|', 0x31, 1},
    {';', 0x33, 0}, {':', 0x33, 1},
    {'\'', 0x34, 0}, {'"', 0x34, 1},
    {'`', 0x35, 0}, {'~', 0x35, 1},
    {',', 0x36, 0}, {'<', 0x36, 1},
    {'.', 0x37, 0}, {'>', 0x37, 1},
    {'/', 0x38, 0}, {'?', 0x38, 1},
};

int mdc_key_for_char(char c, unsigned short *usage, int *shift) {
    if (!usage || !shift) return -1;
    for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
        if (kMap[i].c == c) {
            *usage = kMap[i].usage;
            *shift = kMap[i].shift ? 1 : 0;
            return 0;
        }
    }
    return -1;
}

unsigned long mdc_utf8_to_utf16le(const char *input, unsigned char *output, unsigned long output_cap) {
    unsigned long out = 0;
    const unsigned char *p = (const unsigned char *)input;
    if (!input || !output) return 0;
    while (*p) {
        unsigned long cp = 0;
        int extra = 0;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = (unsigned long)(*p++ & 0x1F); extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = (unsigned long)(*p++ & 0x0F); extra = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = (unsigned long)(*p++ & 0x07); extra = 3;
        } else {
            p++;
            cp = 0xFFFD;
        }
        for (int i = 0; i < extra; i++) {
            if ((*p & 0xC0) != 0x80) { cp = 0xFFFD; break; }
            cp = (cp << 6) | (unsigned long)(*p++ & 0x3F);
        }
        if (cp > 0x10FFFF) cp = 0xFFFD;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unsigned long hi = 0xD800 + (cp >> 10);
            unsigned long lo = 0xDC00 + (cp & 0x3FF);
            if (out + 4 > output_cap) break;
            output[out++] = (unsigned char)(hi & 0xFF); output[out++] = (unsigned char)(hi >> 8);
            output[out++] = (unsigned char)(lo & 0xFF); output[out++] = (unsigned char)(lo >> 8);
        } else {
            if (out + 2 > output_cap) break;
            output[out++] = (unsigned char)(cp & 0xFF); output[out++] = (unsigned char)(cp >> 8);
        }
    }
    return out;
}
