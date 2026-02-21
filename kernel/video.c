/* kernel/video.c */
// ✅ NO #include <stdio.h> or <stddef.h>
// Uses kernel/types.h instead

#include "types.h"
#include "video.h"

#define VGA_MEMORY ((volatile uint8_t*)0xB8000)

static uint16_t x = 0;
static uint16_t y = 0;

void clear_screen(void) {
    for (uint16_t i = 0; i < 80 * 25; i++) {
        VGA_MEMORY[i * 2] = ' ';
        VGA_MEMORY[i * 2 + 1] = 0x07;
    }
    x = 0;
    y = 0;
}

void print_char(char c) {
    if (c == '\n') {
        x = 0;
        y++;
        if (y >= 25) {
            // Scroll up
            for (uint16_t i = 0; i < 80 * 24; i++) {
                VGA_MEMORY[i * 2] = VGA_MEMORY[(i + 80) * 2];
                VGA_MEMORY[i * 2 + 1] = VGA_MEMORY[(i + 80) * 2 + 1];
            }
            // Clear last line
            for (uint16_t i = 0; i < 80; i++) {
                VGA_MEMORY[(i + 80 * 24) * 2] = ' ';
                VGA_MEMORY[(i + 80 * 24) * 2 + 1] = 0x07;
            }
            y = 24;
        }
        return;
    }

    VGA_MEMORY[(x + y * 80) * 2] = c;
    VGA_MEMORY[(x + y * 80) * 2 + 1] = 0x07;
    x++;

    if (x >= 80) {
        x = 0;
        y++;
        if (y >= 25) y = 24;
    }
}

void print_str(const char *s) {
    while (*s) print_char(*s++);
}
