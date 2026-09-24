#include "psram_spi.h"
#include "hardware/clocks.h"
#include <stdlib.h>
#include <string.h>
#include "ff.h"

#if PICO_RP2350
extern bool wonderswan_qspi_psram_available(void);
extern uintptr_t wonderswan_qspi_aux_base(void);
static inline volatile uint8_t *qspi_aux_ptr(uint32_t addr) {
    return (volatile uint8_t *)(wonderswan_qspi_aux_base() + addr);
}
#endif

static psram_spi_inst_t psram_spi;
static uint32_t psram_init_sys_hz;
static bool legacy_psram_available;
static uint32_t fallback_sram_size;
static uint32_t fallback_eeprom_size;

#define NVRAM_CACHE_SIZE 4096u
#define NVRAM_BACKING_FILE "/tmp/wonderswan.nvram"
static FIL nvram_file;
static bool nvram_file_open;
static uint8_t nvram_cache[NVRAM_CACHE_SIZE];
static uint32_t nvram_cache_base;
static uint32_t nvram_cache_valid;
static bool nvram_cache_loaded;
static bool nvram_cache_dirty;

static bool nvram_cache_flush(void) {
    if (!nvram_file_open || !nvram_cache_loaded || !nvram_cache_dirty) return true;
    if (f_lseek(&nvram_file, nvram_cache_base) != FR_OK) return false;
    UINT written = 0;
    if (f_write(&nvram_file, nvram_cache, nvram_cache_valid, &written) != FR_OK ||
        written != nvram_cache_valid) return false;
    nvram_cache_dirty = false;
    return true;
}

static bool nvram_cache_load(uint32_t offset) {
    const uint32_t total = fallback_eeprom_size + fallback_sram_size;
    const uint32_t base = offset & ~(NVRAM_CACHE_SIZE - 1u);
    if (nvram_cache_loaded && nvram_cache_base == base) return true;
    if (!nvram_cache_flush()) return false;
    nvram_cache_base = base;
    nvram_cache_valid = total - base;
    if (nvram_cache_valid > NVRAM_CACHE_SIZE) nvram_cache_valid = NVRAM_CACHE_SIZE;
    memset(nvram_cache, 0, sizeof(nvram_cache));
    if (f_lseek(&nvram_file, base) != FR_OK) return false;
    UINT read = 0;
    if (f_read(&nvram_file, nvram_cache, nvram_cache_valid, &read) != FR_OK) return false;
    nvram_cache_loaded = true;
    nvram_cache_dirty = false;
    return true;
}

static bool nvram_translate(uint32_t addr, uint32_t *offset) {
    if (addr >= (1u << 20)) {
        const uint32_t sram_offset = addr - (1u << 20);
        if (sram_offset >= fallback_sram_size) return false;
        *offset = fallback_eeprom_size + sram_offset;
        return true;
    }
    if (addr >= fallback_eeprom_size) return false;
    *offset = addr;
    return true;
}

#define ITE_PSRAM (1ul << 20)
#define MAX_PSRAM (512ul << 20)

static uint32_t _psram_size() {
#ifdef PSRAM    
    int32_t res = 0;
    for (res = ITE_PSRAM; res < MAX_PSRAM; res += ITE_PSRAM) {
        psram_write32(&psram_spi, res, res);
        if (res != psram_read32(&psram_spi, res)) {
            res -= ITE_PSRAM;
            return res;
        }
    }
    return res - psram_read32(&psram_spi, ITE_PSRAM) + ITE_PSRAM;
#else
    return 0;
#endif
}


uint32_t psram_size() {
    static int32_t _res = -1;
    int32_t res = 0;
    if (_res != -1) {
        return _res;
    }
    _res = _psram_size();
    return _res;
}

uint32_t init_psram() {
    psram_init_sys_hz = clock_get_hz(clk_sys);
    legacy_psram_available = false;
#if PICO_RP2350
    if (wonderswan_qspi_psram_available()) return 2u << 20;
#endif
#ifdef WONDERSWAN_LEGACY_SPI_PSRAM
    psram_spi = psram_spi_init_clkdiv(pio0, -1, 2.0, false);
    if (!_psram_size()) {
        psram_spi = psram_spi_init_clkdiv(pio0, -1, 2.0, true);
    }
    const uint32_t size = psram_size();
    legacy_psram_available = size != 0;
    return size;
#else
    return 0;
#endif
}

#define CART_IDENTITY_ADDR ((1u << 20) - 16u)
#define CART_IDENTITY_MAGIC 0x57534349u /* "WSCI" */

static uint8_t physical_psram_read8(uint32_t addr) {
#if PICO_RP2350
    if (wonderswan_qspi_psram_available()) return *qspi_aux_ptr(addr);
#endif
    return legacy_psram_available ? psram_read8(&psram_spi, addr) : 0xff;
}

static void physical_psram_write8(uint32_t addr, uint8_t value) {
#if PICO_RP2350
    if (wonderswan_qspi_psram_available()) { *qspi_aux_ptr(addr) = value; return; }
#endif
    if (legacy_psram_available) psram_write8(&psram_spi, addr, value);
}

static uint32_t physical_psram_read32(uint32_t addr) {
    return (uint32_t)physical_psram_read8(addr) |
           ((uint32_t)physical_psram_read8(addr + 1u) << 8) |
           ((uint32_t)physical_psram_read8(addr + 2u) << 16) |
           ((uint32_t)physical_psram_read8(addr + 3u) << 24);
}

static void physical_psram_write32(uint32_t addr, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        physical_psram_write8(addr + i, (uint8_t)(value >> (i * 8)));
}

bool psram_configure_cart_storage(uint32_t sram_size, uint32_t eeprom_size,
                                  uint32_t rom_size, uint16_t rom_checksum) {
#if PICO_RP2350
    const bool physical_psram = wonderswan_qspi_psram_available() || legacy_psram_available;
#else
    const bool physical_psram = legacy_psram_available;
#endif
    if (physical_psram) {
        const uint32_t old_magic = physical_psram_read32(CART_IDENTITY_ADDR);
        const uint32_t old_size = physical_psram_read32(CART_IDENTITY_ADDR + 4u);
        const uint32_t old_checksum = physical_psram_read32(CART_IDENTITY_ADDR + 8u);
        const bool same_cart = old_magic == CART_IDENTITY_MAGIC &&
                               old_size == rom_size && old_checksum == rom_checksum;

        if (!same_cart) {
            /* Physical PSRAM survives a watchdog reset.  Its cartridge area
               must therefore be treated like inserted-cartridge state, not as
               global emulator state shared by every ROM.  A different ROM gets
               erased EEPROM/SRAM; restarting the same ROM keeps its contents. */
            for (uint32_t i = 0; i < eeprom_size; ++i)
                physical_psram_write8(i, 0xff);
            const uint32_t ram_bytes = sram_size ? sram_size : 0x10000u;
            for (uint32_t i = 0; i < ram_bytes; ++i)
                physical_psram_write8((1u << 20) + i, 0);

            physical_psram_write32(CART_IDENTITY_ADDR + 4u, rom_size);
            physical_psram_write32(CART_IDENTITY_ADDR + 8u, rom_checksum);
            physical_psram_write32(CART_IDENTITY_ADDR, CART_IDENTITY_MAGIC);
        }
        return true;
    }

    if (nvram_file_open) {
        nvram_cache_flush();
        f_close(&nvram_file);
        nvram_file_open = false;
    }
    fallback_sram_size = sram_size;
    fallback_eeprom_size = eeprom_size;
    nvram_cache_loaded = false;
    nvram_cache_dirty = false;

    const uint32_t total = sram_size + eeprom_size;
    if (!total) return true;
    const FRESULT mkdir_result = f_mkdir("/tmp");
    if (mkdir_result != FR_OK && mkdir_result != FR_EXIST) return false;
    if (f_open(&nvram_file, NVRAM_BACKING_FILE, FA_READ | FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    nvram_file_open = true;

    /* Pre-size the temporary backing file without consuming cartridge-sized RAM. */
    if (f_lseek(&nvram_file, total - 1u) != FR_OK) return false;
    const uint8_t zero = 0;
    UINT written = 0;
    if (f_write(&nvram_file, &zero, 1, &written) != FR_OK || written != 1) return false;
    if (f_sync(&nvram_file) != FR_OK) return false;
    return true;
}

void psram_reclock() {
#if PICO_RP2350
    if (wonderswan_qspi_psram_available()) return;
#endif
    if (legacy_psram_available && psram_init_sys_hz && psram_spi.sm >= 0) {
        pio_sm_set_clkdiv(psram_spi.pio, psram_spi.sm,
            2.0f * (float)clock_get_hz(clk_sys) / (float)psram_init_sys_hz);
    }
}

void psram_cleanup() {
    if (!legacy_psram_available) return;
    //logMsg("PSRAM cleanup"); // TODO: block mode, ensure diapason
    for (uint32_t addr32 = (1ul << 20); addr32 < (2ul << 20); addr32 += 4) {
        psram_write32(&psram_spi, addr32, 0);
    }
}

void write8psram(uint32_t addr32, uint8_t v) {
#if PICO_RP2350
    if (wonderswan_qspi_psram_available()) { *qspi_aux_ptr(addr32) = v; return; }
#endif
    if (legacy_psram_available) { psram_write8(&psram_spi, addr32, v); return; }
    uint32_t offset;
    if (nvram_file_open && nvram_translate(addr32, &offset) && nvram_cache_load(offset)) {
        nvram_cache[offset - nvram_cache_base] = v;
        nvram_cache_dirty = true;
    }
}

void write16psram(uint32_t addr32, uint16_t v) {
    write8psram(addr32, (uint8_t)v);
    write8psram(addr32 + 1, (uint8_t)(v >> 8));
}

void write32psram(uint32_t addr32, uint32_t v) {
    write16psram(addr32, (uint16_t)v);
    write16psram(addr32 + 2, (uint16_t)(v >> 16));
}

void writepsram(uint32_t addr32, uint8_t* b, size_t sz) {
    while (sz--) write8psram(addr32++, *b++);
}

void readpsram(uint8_t* b, uint32_t addr32, size_t sz) {
    while (sz--) *b++ = read8psram(addr32++);
}

uint8_t read8psram(uint32_t addr32) {
#if PICO_RP2350
    if (wonderswan_qspi_psram_available()) return *qspi_aux_ptr(addr32);
#endif
    if (legacy_psram_available) return psram_read8(&psram_spi, addr32);
    uint32_t offset;
    if (nvram_file_open && nvram_translate(addr32, &offset) && nvram_cache_load(offset))
        return nvram_cache[offset - nvram_cache_base];
    return 0xff;
}

uint16_t read16psram(uint32_t addr32) {
    return (uint16_t)read8psram(addr32) | ((uint16_t)read8psram(addr32 + 1) << 8);
}

uint32_t read32psram(uint32_t addr32) {
    return (uint32_t)read16psram(addr32) | ((uint32_t)read16psram(addr32 + 2) << 16);
}

#include <stdio.h>

#if defined(PSRAM_ASYNC) && defined(PSRAM_ASYNC_SYNCHRONIZE)
void __isr psram_dma_complete_handler() {
#if PSRAM_ASYNC_DMA_IRQ == 0
    dma_hw->ints0 = 1u << async_spi_inst->async_dma_chan;
#elif PSRAM_ASYNC_DMA_IRQ == 1
    dma_hw->ints1 = 1u << async_spi_inst->async_dma_chan;
#else
#error "PSRAM_ASYNC defined without PSRAM_ASYNC_DMA_IRQ set to 0 or 1"
#endif
    /* putchar('@'); */
#if defined(PSRAM_MUTEX)
    mutex_exit(&async_spi_inst->mtx);
#elif defined(PSRAM_SPINLOCK)
    spin_unlock(async_spi_inst->spinlock, async_spi_inst->spin_irq_state);
#endif
}
#endif // defined(PSRAM_ASYNC) && defined(PSRAM_ASYNC_SYNCHRONIZE)

static inline void pio_spi_psram_cs_init(PIO pio, uint sm, uint prog_offs, uint n_bits, float clkdiv, bool fudge, uint pin_cs, uint pin_mosi, uint pin_miso) {
    pio_sm_config c;
    if (fudge) {
        c = spi_psram_fudge_program_get_default_config(prog_offs);
    } else {
        c = spi_psram_program_get_default_config(prog_offs);
    }
    sm_config_set_out_pins(&c, pin_mosi, 1);
    sm_config_set_in_pins(&c, pin_miso);
    sm_config_set_sideset_pins(&c, pin_cs);
    sm_config_set_out_shift(&c, false, true, n_bits);
    sm_config_set_in_shift(&c, false, true, n_bits);
    sm_config_set_clkdiv(&c, clkdiv);

    pio_sm_set_consecutive_pindirs(pio, sm, pin_cs, 2, true);
    pio_sm_set_consecutive_pindirs(pio, sm, pin_mosi, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, pin_miso, 1, false);
    pio_gpio_init(pio, pin_miso); // MISSING this initialisation of the incoming PIN!
    pio_gpio_init(pio, pin_mosi);
    pio_gpio_init(pio, pin_cs);
    pio_gpio_init(pio, pin_cs + 1);

    hw_set_bits(&pio->input_sync_bypass, 1u << pin_miso);

    pio_sm_init(pio, sm, prog_offs, &c);
    pio_sm_set_enabled(pio, sm, true);
}

static inline void pio_qspi_psram_cs_init(PIO pio, uint sm, uint prog_offs, uint n_bits, float clkdiv, uint pin_cs, uint pin_sio0) {
    pio_sm_config c = qspi_psram_program_get_default_config(prog_offs);
    sm_config_set_out_pins(&c, pin_sio0, 4);
    sm_config_set_in_pins(&c, pin_sio0);
    sm_config_set_set_pins(&c, pin_sio0, 4);
    sm_config_set_sideset_pins(&c, pin_cs);
    sm_config_set_out_shift(&c, false, true, n_bits);
    sm_config_set_in_shift(&c, false, true, n_bits);
    sm_config_set_clkdiv(&c, clkdiv);

    pio_sm_set_consecutive_pindirs(pio, sm, pin_cs, 2, true);
    pio_sm_set_consecutive_pindirs(pio, sm, pin_sio0, 4, true);
    pio_gpio_init(pio, pin_sio0);
    pio_gpio_init(pio, pin_sio0 + 1);
    pio_gpio_init(pio, pin_sio0 + 2);
    pio_gpio_init(pio, pin_sio0 + 3);
    pio_gpio_init(pio, pin_cs);
    pio_gpio_init(pio, pin_cs + 1);

    hw_set_bits(&pio->input_sync_bypass, 0xfu << pin_sio0);

    pio_sm_init(pio, sm, prog_offs, &c);
    pio_sm_set_enabled(pio, sm, true);
}

psram_spi_inst_t psram_spi_init_clkdiv(PIO pio, int sm, float clkdiv, bool fudge) {
    psram_spi_inst_t spi;
    spi.pio = pio;
    spi.offset = pio_add_program(spi.pio, fudge ? &spi_psram_fudge_program : &spi_psram_program);
    if (sm == -1) {
        spi.sm = pio_claim_unused_sm(spi.pio, true);
    } else {
        spi.sm = sm;
    }
#if defined(PSRAM_MUTEX)
    mutex_init(&spi.mtx);
#elif defined(PSRAM_SPINLOCK)
    int spin_id = spin_lock_claim_unused(true);
    spi.spinlock = spin_lock_init(spin_id);
#endif

    gpio_set_drive_strength(PSRAM_PIN_CS, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(PSRAM_PIN_SCK, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(PSRAM_PIN_MOSI, GPIO_DRIVE_STRENGTH_4MA);
    /* gpio_set_slew_rate(PSRAM_PIN_CS, GPIO_SLEW_RATE_FAST); */
    /* gpio_set_slew_rate(PSRAM_PIN_SCK, GPIO_SLEW_RATE_FAST); */
    /* gpio_set_slew_rate(PSRAM_PIN_MOSI, GPIO_SLEW_RATE_FAST); */

    pio_spi_psram_cs_init(spi.pio, spi.sm, spi.offset, 8 /*n_bits*/, clkdiv, fudge, PSRAM_PIN_CS, PSRAM_PIN_MOSI, PSRAM_PIN_MISO);

    // Write DMA channel setup
    spi.write_dma_chan = dma_claim_unused_channel(true);
    spi.write_dma_chan_config = dma_channel_get_default_config(spi.write_dma_chan);
    channel_config_set_transfer_data_size(&spi.write_dma_chan_config, DMA_SIZE_8);
    channel_config_set_read_increment(&spi.write_dma_chan_config, true);
    channel_config_set_write_increment(&spi.write_dma_chan_config, false);
    channel_config_set_dreq(&spi.write_dma_chan_config, pio_get_dreq(spi.pio, spi.sm, true));
    dma_channel_set_write_addr(spi.write_dma_chan, &spi.pio->txf[spi.sm], false);
    dma_channel_set_config(spi.write_dma_chan, &spi.write_dma_chan_config, false);

    // Read DMA channel setup
    spi.read_dma_chan = dma_claim_unused_channel(true);
    spi.read_dma_chan_config = dma_channel_get_default_config(spi.read_dma_chan);
    channel_config_set_transfer_data_size(&spi.read_dma_chan_config, DMA_SIZE_8);
    channel_config_set_read_increment(&spi.read_dma_chan_config, false);
    channel_config_set_write_increment(&spi.read_dma_chan_config, true);
    channel_config_set_dreq(&spi.read_dma_chan_config, pio_get_dreq(spi.pio, spi.sm, false));
    dma_channel_set_read_addr(spi.read_dma_chan, &spi.pio->rxf[spi.sm], false);
    dma_channel_set_config(spi.read_dma_chan, &spi.read_dma_chan_config, false);

#if defined(PSRAM_ASYNC)
    // Asynchronous DMA channel setup
    spi.async_dma_chan = dma_claim_unused_channel(true);
    spi.async_dma_chan_config = dma_channel_get_default_config(spi.async_dma_chan);
    channel_config_set_transfer_data_size(&spi.async_dma_chan_config, DMA_SIZE_8);
    channel_config_set_read_increment(&spi.async_dma_chan_config, true);
    channel_config_set_write_increment(&spi.async_dma_chan_config, false);
    channel_config_set_dreq(&spi.async_dma_chan_config, pio_get_dreq(spi.pio, spi.sm, true));
    dma_channel_set_write_addr(spi.async_dma_chan, &spi.pio->txf[spi.sm], false);
    dma_channel_set_config(spi.async_dma_chan, &spi.async_dma_chan_config, false);

#if defined(PSRAM_ASYNC_COMPLETE)
    irq_set_exclusive_handler(DMA_IRQ_0 + PSRAM_ASYNC_DMA_IRQ, psram_dma_complete_handler);
    dma_irqn_set_channel_enabled(PSRAM_ASYNC_DMA_IRQ, spi.async_dma_chan, true);
    irq_set_enabled(DMA_IRQ_0 + PSRAM_ASYNC_DMA_IRQ, true);
#endif // defined(PSRAM_ASYNC_COMPLETE)
#endif // defined(PSRAM_ASYNC)

    uint8_t psram_reset_en_cmd[] = {
        8,      // 8 bits to write
        0,      // 0 bits to read
        0x66u   // Reset enable command
    };
    pio_spi_write_read_dma_blocking(&spi, psram_reset_en_cmd, 3, 0, 0);
    busy_wait_us(50);
    uint8_t psram_reset_cmd[] = {
        8,      // 8 bits to write
        0,      // 0 bits to read
        0x99u   // Reset command
    };
    pio_spi_write_read_dma_blocking(&spi, psram_reset_cmd, 3, 0, 0);
    busy_wait_us(100);
    
    return spi;
};

psram_spi_inst_t psram_spi_init(PIO pio, int sm) {
    return psram_spi_init_clkdiv(pio, sm, 1.8, true);
}

void psram_spi_uninit(psram_spi_inst_t spi, bool fudge) {
#if defined(PSRAM_ASYNC)
    // Asynchronous DMA channel teardown
    dma_channel_unclaim(spi.async_dma_chan);
#if defined(PSRAM_ASYNC_COMPLETE)
    irq_set_enabled(DMA_IRQ_0 + PSRAM_ASYNC_DMA_IRQ, false);
    dma_irqn_set_channel_enabled(PSRAM_ASYNC_DMA_IRQ, spi.async_dma_chan, false);
    irq_remove_handler(DMA_IRQ_0 + PSRAM_ASYNC_DMA_IRQ, psram_dma_complete_handler);
#endif // defined(PSRAM_ASYNC_COMPLETE)
#endif // defined(PSRAM_ASYNC)

    // Write DMA channel teardown
    dma_channel_unclaim(spi.write_dma_chan);

    // Read DMA channel teardown
    dma_channel_unclaim(spi.read_dma_chan);

#if defined(PSRAM_SPINLOCK)
    int spin_id = spin_lock_get_num(spi.spinlock);
    spin_lock_unclaim(spin_id);
#endif

    pio_sm_unclaim(spi.pio, spi.sm);
    pio_remove_program(spi.pio, fudge ? &spi_psram_fudge_program : &spi_psram_program, spi.offset);
}

const static uint8_t read_id_command[] = {
    32,         // 32 bits write
    64,         // 64 bits read
    0x9fu,      // command
    0, 0, 0     // Address
};

void psram_id(uint8_t rx[8]) {
    pio_spi_write_read_dma_blocking(&psram_spi, read_id_command, sizeof(read_id_command), rx, 8);
}
