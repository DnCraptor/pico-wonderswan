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

#ifndef __IO_H__
#define __IO_H__

#include "types.h"

extern	uint8	ws_ioRam[0x100];

extern	uint8	ws_key_start;
extern	uint8	ws_key_left;
extern	uint8	ws_key_right;
extern	uint8	ws_key_up;
extern	uint8	ws_key_down;
extern	uint8	ws_key_button_1;
extern	uint8	ws_key_button_2;
extern	uint8	ws_key_x1, ws_key_x2, ws_key_x3, ws_key_x4;
extern	uint8	ws_key_y1, ws_key_y2, ws_key_y3, ws_key_y4;

void ws_io_init(void);
void ws_io_reset(void);
void ws_io_flipControls(void);
void ws_io_setControlsFlipped(int flipped);
void ws_io_done(void);

typedef struct { uint8 io_ram[0x100]; uint8 internal_eeprom[1024]; int32 rtc_read_count; uint8 controls_flipped; } ws_io_snapshot_t;
void ws_io_snapshot_get(ws_io_snapshot_t *state);
void ws_io_snapshot_set(const ws_io_snapshot_t *state);

#endif
