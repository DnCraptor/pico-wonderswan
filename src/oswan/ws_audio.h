#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void ws_audio_init(void);
void ws_audio_reset(void);
void ws_audio_process(uint32 cycles);
uint8 ws_audio_hyper_port_read(uint32 port);
void ws_audio_hyper_port_write(uint32 port, uint8 value);
uint8 ws_audio_dma_port_read(uint32 port);
void ws_audio_dma_port_write(uint32 port, uint8 value);
uint8 ws_audio_port_read(uint32 port);
void ws_audio_port_write(uint32 port, uint8 value);

#ifdef __cplusplus
}
#endif
