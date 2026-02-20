// kernel/video.c
#include <stdint.h>

#define VGA_MEMORY ((uint8_t *)0xB8000)

void clear_screen() {
    for(int i = 0; i < 80 * 25 * 2; i++) {
        VGA_MEMORY[i] = 0;
    }
}

void print_char(char c) {
    static int x = 0, y = 0;
    const int video_mem = 0xB8000;
    
    if(c == '\n') {
        x = 0;
        y++;
        return;
    }
    
    if(x >= 80) {
        x = 0;
        y++;
    }
    
    if(y >= 25) {
        // Scroll up
        for(int i = 0; i < 80 * 24 * 2; i++) {
            VGA_MEMORY[i] = VGA_MEMORY[i + 80 * 2];
        }
        // Clear last line
        for(int i = 80 * 24 * 2; i < 80 * 25 * 2; i++) {
            VGA_MEMORY[i] = 0;
        }
        y = 24;
    }

    VGA_MEMORY[(x + y * 80) * 2] = c;
    VGA_MEMORY[(x + y * 80) * 2 + 1] = 0x07; // White on Black
    x++;
}

void print_str(char *str) {
    for(int i = 0; str[i] != 0; i++) {
        print_char(str[i]);
    }
}
