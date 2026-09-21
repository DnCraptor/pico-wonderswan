//////////////////////////////////////////////////////////////////////////////
//
//////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//////////////////////////////////////////////////////////////////////////////

#ifndef __GPU_H__
#define __GPU_H__

#define COLOUR_SCHEME_DEFAULT	0
#define COLOUR_SCHEME_AMBER		1
#define COLOUR_SCHEME_GREEN		2

extern	uint8	ws_gpu_scanline;
extern	uint8	ws_gpu_operatingInColor;
extern	uint8	ws_videoMode;
extern	int16	ws_palette[16*4];
extern	int8	ws_paletteColors[8];
extern	int16	wsc_palette[16*16];
extern  unsigned int ws_gpu_unknownPort;
extern const uint32 ws_colour_scheme_default[16];


void ws_gpu_init(void);
void ws_gpu_done(void);
void ws_gpu_reset(void);
void ws_gpu_renderScanline(uint8 *framebufferPtr);
void ws_gpu_latchSprites(void);
void ws_gpu_swapSpriteTable(void);
void ws_gpu_changeVideoMode(uint8 value);
void ws_gpu_write_byte(uint32_t offset, uint8_t value);
void ws_gpu_refresh_palette(void);
void ws_gpu_port_write(uint32_t port,uint8_t value);
uint8_t ws_gpu_port_read(uint8_t port);
void ws_gpu_set_colour_scheme(int scheme);
void ws_gpu_changeVideoMode(uint8 value);
void ws_gpu_forceColorSystem(void);
void ws_gpu_forceMonoSystem(void);
void ws_gpu_clearCache(void);

typedef struct {
    uint8 scanline, operating_in_color, video_mode, sprite_count_cache[2], sprite_table_active;
    uint8 reserved[2];
    uint32 sprite_table[2][0x80];
    int16 palette[16 * 4];
    int8 palette_colors[8];
    int16 color_palette[16 * 16];
    int32 force_color, force_mono;
} ws_gpu_snapshot_t;
void ws_gpu_snapshot_get(ws_gpu_snapshot_t *state);
void ws_gpu_snapshot_set(const ws_gpu_snapshot_t *state);

#endif

