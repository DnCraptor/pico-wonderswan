#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
 uint16 period[4]; uint8 volume[4], voice_volume, sweep_step, sweep_value, noise_control, control, output_control, sample_ram_pos;
 int32 period_counter[4]; uint8 sample_pos[4]; uint16 nreg; int32 sweep_divider; uint8 sweep_counter; uint16 sample_counter;
 int32 dc_prev_in[2], dc_prev_out[2]; uint32 audio_pending_cycles;
 int16 hyper_left, hyper_right; uint8 hyper_input, hyper_control, hyper_channel_control, hyper_dma_left, hyper_manual_left;
 int16 hyper_pending_left, hyper_pending_right; uint8 hyper_rate_counter;
 uint32 sound_dma_source, sound_dma_source_reload, sound_dma_size, sound_dma_size_reload; uint8 sound_dma_control; int32 sound_dma_counter;
} ws_audio_snapshot_t;

void ws_audio_init(void);
void ws_audio_reset(void);
void ws_audio_process(uint32 cycles);
void ws_audio_sync(void);
void ws_audio_set_enabled(int enabled);
#ifdef HWAY
void ws_audio_set_hway_volume(unsigned volume);
#endif
void ws_audio_set_rate_shift(unsigned shift);
uint8 ws_audio_hyper_port_read(uint32 port);
void ws_audio_hyper_port_write(uint32 port, uint8 value);
uint8 ws_audio_dma_port_read(uint32 port);
void ws_audio_dma_port_write(uint32 port, uint8 value);
uint8 ws_audio_port_read(uint32 port);
void ws_audio_port_write(uint32 port, uint8 value);
void ws_audio_snapshot_get(ws_audio_snapshot_t *state);
void ws_audio_snapshot_set(const ws_audio_snapshot_t *state);

#ifdef __cplusplus
}
#endif
