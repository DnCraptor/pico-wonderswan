#include "graphics.h"
#include <string.h>
#include <stdio.h>

volatile bool graphics_fps_overlay_enabled = false;
char graphics_fps_overlay_text[8] = "--.-";
volatile bool graphics_demo_overlay_enabled = false;
char graphics_demo_overlay_text[53] = "";
volatile uint8_t graphics_overlay_palette_index = 0;

void graphics_set_fps_overlay(const bool enabled, const uint16_t fps_x10) {
    /* Disable while replacing the string: core 1 may be scanning it. */
    graphics_fps_overlay_enabled = false;
    if (enabled) {
        const unsigned fps = fps_x10 / 10u;
        const unsigned tenth = fps_x10 % 10u;
        snprintf(graphics_fps_overlay_text, sizeof(graphics_fps_overlay_text),
                 "%u.%u", fps, tenth);
        graphics_fps_overlay_enabled = true;
    }
}

void graphics_set_demo_overlay(const bool enabled, const char *text) {
    /* Same lock-free convention as FPS: hide while core 0 replaces text. */
    graphics_demo_overlay_enabled = false;
    if (enabled && text && *text) {
        strncpy(graphics_demo_overlay_text, text, sizeof(graphics_demo_overlay_text) - 1);
        graphics_demo_overlay_text[sizeof(graphics_demo_overlay_text) - 1] = '\0';
        graphics_demo_overlay_enabled = true;
    }
}

void draw_text(const char string[TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint8_t color, uint8_t bgcolor) {
    uint8_t* t_buf = text_buffer + TEXTMODE_COLS * 2 * y + 2 * x;
    for (int xi = TEXTMODE_COLS * 2; xi--;) {
        if (!*string) break;
        *t_buf++ = *string++;
        *t_buf++ = bgcolor << 4 | color & 0xF;
    }
}

void draw_window(const char title[TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    char line[width + 1];
    memset(line, 0, sizeof line);
    width--;
    height--;
    // Рисуем рамки

    memset(line, 0xCD, width); // ═══


    line[0] = 0xC9; // ╔
    line[width] = 0xBB; // ╗
    draw_text(line, x, y, 11, 1);

    line[0] = 0xC8; // ╚
    line[width] = 0xBC; //  ╝
    draw_text(line, x, height + y, 11, 1);

    memset(line, ' ', width);
    line[0] = line[width] = 0xBA;

    for (int i = 1; i < height; i++) {
        draw_text(line, x, y + i, 11, 1);
    }

    snprintf(line, width - 1, " %s ", title);
    draw_text(line, x + (width - strlen(line)) / 2, y, 14, 3);
}
