////////////////////////////////////////////////////////////////////////////////
// Memory
////////////////////////////////////////////////////////////////////////////////
// Notes: need to optimize cpu_writemem20
//
//
//
//
//
//////////////////////////////////////////////////////////////////////////////

#include <string.h>
#include <pico.h>
#include <io.h>
#include "rom.h"
#include "./nec/nec.h"
#include "gpu.h"
#include "psram_spi.h"

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
#define IO_ROM_BANK_BASE_SELECTOR    0xC0


static uint8 *ws_rom;
__aligned(4) uint8 internalRam[0x10000];


uint16 ws_rom_checksum;

uint32 sramAddressMask;
uint32 externalEepromAddressMask;
uint32 romAddressMask;
static uint32 romSize;
static uint32 cartSramSize;
static uint32 cartEepromSize;
static uint32 romBankBase[16];



////////////////////////////////////////////////////////////////////////////////
// Cartridge ROM bank map
////////////////////////////////////////////////////////////////////////////////
static void __not_in_flash_func(ws_memory_update_rom_banks)(void) {
    const uint32 bankMask = (romSize >> 16) - 1u;

    romBankBase[2] = ((uint32)(ws_ioRam[IO_ROM_BANK_BASE_SELECTOR + 2] & bankMask)) << 16;
    romBankBase[3] = ((uint32)(ws_ioRam[IO_ROM_BANK_BASE_SELECTOR + 3] & bankMask)) << 16;

    const uint32 group = (uint32)(ws_ioRam[IO_ROM_BANK_BASE_SELECTOR] & 0x0f) << 4;
    for (uint32 bank = 4; bank < 16; ++bank) {
        const uint32 romBank = 256u - (group | bank);
        /* Linear banks 4..15 must read exactly as they did before 6a25c00
           (unmasked). The & romAddressMask was added to keep flash-backed ROM
           inside the XIP window, but it aliases the PSRAM-backed mirror to the
           wrong bytes, hanging >1 MB carts (e.g. Guilty Gear Petit 2) on M2. */
        romBankBase[bank] = romSize - (romBank << 16);
    }
}

void __not_in_flash_func(ws_memory_rom_bank_changed)(uint32 port) {
    if (port == 0xc0 || port == 0xc2 || port == 0xc3)
        ws_memory_update_rom_banks();
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void __not_in_flash_func(cpu_writemem20)(uint32_t addr, uint8_t value) {
    uint32 offset = addr & 0xffff;
    uint32 bank = addr >> 16;

    // 0 - RAM - 16 KB (WS) / 64 KB (WSC) internal RAM
    if (!bank) {
        ws_gpu_write_byte(offset, value);
//		ws_audio_write_byte(offset,value);
    } else
        // 1 - SRAM (cart)
    if (bank == 1) {
        /* Cartridge SRAM is banked in 64 KiB pages by port C1.  The old
           accessor masked only the 16-bit CPU offset, so carts larger than
           64 KiB could never reach banks 1..n (Dicing Knight has 256 KiB). */
        const uint32 sramAddress = ((((uint32)ws_ioRam[0xc1]) << 16) | offset) & sramAddressMask;
        write8psram((1024 << 10) + sramAddress, value);
    }
//		ws_staticRam[offset&sramAddressMask]=value;

    // other banks are read-only
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
uint8_t __not_in_flash_func(cpu_readmem20)(uint32_t addr) {
    const uint32 offset = addr & 0xffffu;
    const uint32 bank = addr >> 16;

    if (bank == 0) {
        if (ws_gpu_operatingInColor || offset < 0x4000)
            return internalRam[offset];
        return 0xff;
    }
    if (bank == 1) {
        const uint32 sramAddress = ((((uint32)ws_ioRam[0xc1]) << 16) | offset) & sramAddressMask;
        return read8psram((1024 << 10) + sramAddress);
    }

    if (__builtin_expect(bank < 16, 1))
        return ws_rom[romBankBase[bank] + offset];

    const uint32 romBank = 256u - (((uint32)(ws_ioRam[IO_ROM_BANK_BASE_SELECTOR] & 0x0f) << 4) | (bank & 0x0f));
    return ws_rom[offset + romSize - (romBank << 16)];
}

/* Instruction and immediate fetches overwhelmingly come from cartridge ROM.
 * Keep that path to one bank test plus the precomputed bank base.  RAM/SRAM
 * execution remains valid through the general memory accessor. */
uint8_t __not_in_flash_func(cpu_readop20)(uint32_t addr) {
    const uint32 bank = addr >> 16;
    if (__builtin_expect(bank >= 2 && bank < 16, 1))
        return ws_rom[romBankBase[bank] + (addr & 0xffffu)];
    return cpu_readmem20(addr);
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
int ws_memory_init(uint8 *rom, uint32 wsRomSize) {
    ws_romHeaderStruct *ws_romHeader;

    ws_rom = rom;
    romSize = wsRomSize;
    ws_romHeader = ws_rom_getHeader(ws_rom, romSize);
    ws_rom_checksum = ws_romHeader->checksum;

    const uint32 sramSize = ws_rom_sramSize(ws_rom, romSize);
    const uint32 eepromSize = ws_rom_eepromSize(ws_rom, romSize);
    cartSramSize = sramSize; cartEepromSize = eepromSize;
    /* Bank 1 is still a 64 KiB cartridge RAM window when the header declares
       no SRAM.  Keep that legacy/work-RAM behaviour, but do not let C1 become
       an address extension in that case: size - 1 would be 0xFFFFFFFF, so the
       C1 banking added for >64 KiB SRAM would redirect accesses outside the
       64 KiB window (GunPey writes C1 although it declares EEPROM, not SRAM). */
    sramAddressMask = sramSize ? (sramSize - 1u) : 0xffffu;
    externalEepromAddressMask = eepromSize - 1u;
    romAddressMask = romSize - 1;
    ws_memory_update_rom_banks();

    if (ws_romHeader->minimumSupportSystem == WS_SYSTEM_COLOR)
        ws_gpu_operatingInColor = 1;

    return psram_configure_cart_storage(sramSize, eepromSize, romSize, ws_rom_checksum) ? 1 : 0;
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_memory_reset(void) {
    memset(internalRam, 0, 0x10000);
//	memset(ws_staticRam,0,0x10000);
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_memory_done(void) {
#if 0
    free(ws_rom);
    free(ws_staticRam);
    free(internalRam);
    free(externalEeprom);
#endif
}
////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
uint8 *memory_getRom(void) {
    return (ws_rom);
}
////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
uint32 memory_getRomSize(void) {
    return (romSize);
}
////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
uint16 memory_getRomCrc(void) {
    return (ws_rom_checksum);
}


uint32 ws_memory_get_sram_size(void) { return cartSramSize; }
uint32 ws_memory_get_eeprom_size(void) { return cartEepromSize; }
