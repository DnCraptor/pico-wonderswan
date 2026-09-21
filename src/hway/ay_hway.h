#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void hway_init(void);
void hway_write_pcm(uint8_t sample);
void hway_poll(void);
void hway_write_register(unsigned chip, uint8_t reg, uint8_t value);
#ifdef __cplusplus
}
#endif
