#pragma once
#include <stdbool.h>
#include <stdint.h>

extern volatile bool ws_backplane_enabled;
extern const uint8_t ws_backplane[320 * 240];
extern const uint32_t ws_backplane_palette[208];
extern const uint8_t ws_backplane_palette_slots[208];
extern const uint8_t ws_backplane_vga[320 * 240];
extern const uint32_t ws_backplane_vga_palette[27];
