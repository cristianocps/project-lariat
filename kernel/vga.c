#include "vga.h"

static size_t cursor_x = 0;
static size_t cursor_y = 0;
static uint8_t color = 0;

void vga_init(void) {
    color = VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

void vga_set_color(uint8_t c) {
    color = c;
}

void vga_set_cursor(size_t x, size_t y) {
    cursor_x = x;
    cursor_y = y;
}

void vga_clear(void) {
    uint16_t blank = (uint16_t)' ' | ((uint16_t)color << 8);
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_BUFFER[i] = blank;
    }
    cursor_x = 0;
    cursor_y = 0;
}

static void vga_scroll(void) {
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            VGA_BUFFER[y * VGA_WIDTH + x] = VGA_BUFFER[(y + 1) * VGA_WIDTH + x];
        }
    }
    uint16_t blank = (uint16_t)' ' | ((uint16_t)color << 8);
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        VGA_BUFFER[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = blank;
    }
    cursor_y = VGA_HEIGHT - 1;
}

void vga_putchar(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 4) & ~3;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            VGA_BUFFER[cursor_y * VGA_WIDTH + cursor_x] = (uint16_t)' ' | ((uint16_t)color << 8);
        }
    } else {
        VGA_BUFFER[cursor_y * VGA_WIDTH + cursor_x] = (uint16_t)c | ((uint16_t)color << 8);
        cursor_x++;
    }

    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
    }
    if (cursor_y >= VGA_HEIGHT) {
        vga_scroll();
    }
}

void vga_print(const char *str) {
    while (*str) {
        vga_putchar(*str++);
    }
}
