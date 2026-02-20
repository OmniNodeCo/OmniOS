// kernel/video.c
#include "video.h"
#include <stdint.h>

#define VGA_MEMORY ((volatile uint8_t*)0xB8000)

static int x = 0;
static int y = 0;

void clear_screen(void) {
    for (int i = 0; i < 80 * 25; i++) {
        VGA_MEMORY[i * 2] = ' ';
        VGA_MEMORY[i * 2 + 1] = 0x07; // Light gray on black
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
            for (int i = 0; i < 80 * 24; i++) {
                VGA_MEMORY[i * 2] = VGA_MEMORY[(i + 80) * 2];
                VGA_MEMORY[i * 2 + 1] = VGA_MEMORY[(i + 80) * 2 + 1];
            }
            // Clear last line
            for (int i = 0; i < 80; i++) {
                VGA_MEMORY[(i + 80 * 24) * 2] = ' ';
                VGA_MEMORY[(i + 80 * 24) * 2 + 1] = 0x07;
            }
            y = 24;
        }
        return;
    }

    // Write character and attribute
    VGA_MEMORY[(x + y * 80) * 2] = c;
    VGA_MEMORY[(x + y * 80) * 2 + 1] = 0x07;
    x++;

    if (x >= 80) {
        x = 0;
        y++;
        if (y >= 25) {
            y = 24;
        }
    }
}

void print_str(const char *s) {
    while (*s) {
        print_char(*s++);
    }
}
