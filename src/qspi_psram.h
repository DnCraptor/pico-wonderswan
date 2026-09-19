#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WONDERSWAN_QSPI_PSRAM_BASE ((uintptr_t)0x11000000u)
#define WONDERSWAN_QSPI_PSRAM_UNCACHED_BASE ((uintptr_t)0x15000000u)
#define WONDERSWAN_QSPI_PSRAM_MAX_SIZE (16u * 1024u * 1024u)
#define WONDERSWAN_QSPI_AUX_SIZE (2u * 1024u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

bool wonderswan_qspi_psram_init(void);
bool wonderswan_qspi_psram_available(void);
size_t wonderswan_qspi_psram_size(void);
size_t wonderswan_qspi_rom_capacity(void);
uintptr_t wonderswan_qspi_aux_base(void);
void wonderswan_qspi_psram_reclock(uint32_t sys_hz);

#ifdef __cplusplus
}
#endif
