#include "ay_hway.h"
#include "pico/stdlib.h"
#include "pico/platform.h"
#include "hardware/clocks.h"

/* Same physical HWAY/TurboSound serial bus used by pico-gamate and murm386:
 * two cascaded 74HC595s, latch=AUDIO_DATA_PIN, clock=AUDIO_CLOCK_PIN,
 * serial data=AUDIO_CLOCK_PIN+1. */
#define HWAY_LATCH_PIN AUDIO_DATA_PIN
#define HWAY_CLOCK_PIN AUDIO_CLOCK_PIN
#define HWAY_DATA_PIN  (AUDIO_CLOCK_PIN + 1)

#define AY_CS_SAA1099 (1u << 15)
#define AY_ENABLE     (1u << 14)
#define AY_SAVE       (1u << 13)
#define AY_BEEPER     (1u << 12)
#define AY_CS1        (1u << 11)
#define AY_CS0        (1u << 10)
#define AY_BDIR       (1u << 9)
#define AY_BC1        (1u << 8)

static uint16_t control_bits;
static uint8_t last_pcm;
static bool last_pcm_valid;

static inline void hway_wait_to_adjust(uint32_t wait_nops) {
    for (uint32_t i = 0; i < wait_nops; ++i)
        __asm volatile("nop");
}

static void __not_in_flash_func(hway_shift16)(uint16_t data) {
    /* Same 74HC595 timing used by the working murm386 HWAY backend. */
    static uint32_t wait_nops;
    if (wait_nops == 0)
        wait_nops = clock_get_hz(clk_sys) / (30000000u * 5u);

    gpio_put(HWAY_CLOCK_PIN, 0);
    hway_wait_to_adjust(wait_nops);

    for (unsigned i = 0; i < 16; ++i) {
        gpio_put(HWAY_DATA_PIN, (data & 0x8000u) != 0);
        data <<= 1;
        gpio_put(HWAY_CLOCK_PIN, 1);
        hway_wait_to_adjust(wait_nops);
        gpio_put(HWAY_CLOCK_PIN, 0);
        hway_wait_to_adjust(wait_nops);
    }

    gpio_put(HWAY_LATCH_PIN, 1);
    hway_wait_to_adjust(wait_nops);
    gpio_put(HWAY_LATCH_PIN, 0);
}

static inline void control_high(uint16_t mask) { control_bits |= mask; }
static inline void control_low(uint16_t mask) { control_bits &= (uint16_t)~mask; }

static void select_chip(unsigned chip) {
    if (chip == 0) {
        control_low(AY_CS1);
        control_high(AY_CS0);
    } else {
        control_high(AY_CS1);
        control_low(AY_CS0);
    }
}

static void select_register(uint8_t reg) {
    control_high(AY_BDIR | AY_BC1);
    hway_shift16(control_bits | reg);
    control_low(AY_BDIR | AY_BC1);
    hway_shift16(control_bits | reg);
}

static void __not_in_flash_func(write_data)(uint8_t value) {
    control_low(AY_BDIR);
    hway_shift16(control_bits | value);
    control_high(AY_BDIR);
    hway_shift16(control_bits | value);
    control_low(AY_BDIR);
    hway_shift16(control_bits | value);
}

void hway_write_register(unsigned chip, uint8_t reg, uint8_t value) {
    select_chip(chip);
    select_register(reg);
    write_data(value);
}

void hway_init(void) {
    gpio_init(HWAY_LATCH_PIN);
    gpio_init(HWAY_CLOCK_PIN);
    gpio_init(HWAY_DATA_PIN);
    gpio_set_dir(HWAY_LATCH_PIN, GPIO_OUT);
    gpio_set_dir(HWAY_CLOCK_PIN, GPIO_OUT);
    gpio_set_dir(HWAY_DATA_PIN, GPIO_OUT);
    gpio_put(HWAY_LATCH_PIN, 0);
    gpio_put(HWAY_CLOCK_PIN, 0);
    gpio_put(HWAY_DATA_PIN, 0);

    /* Reference HWAY reset/idle state used by the existing Gamate/murm386
     * implementations. */
    control_bits = 0;
    control_low(AY_ENABLE);
    hway_shift16(control_bits);
    control_bits = AY_CS_SAA1099 | AY_ENABLE | AY_SAVE | AY_BEEPER |
                   AY_CS1 | AY_CS0 | AY_BDIR | AY_BC1;
    hway_shift16(control_bits);

    /* PCM-only stage: exactly like murm386, no AY master clock is needed
     * to drive the asynchronous register bus and port-B DAC. */
    last_pcm_valid = false;
}

void __not_in_flash_func(hway_write_pcm)(uint8_t sample) {
    if (last_pcm_valid && sample == last_pcm)
        return;
    last_pcm = sample;
    last_pcm_valid = true;

    /* Exact working pico-gamate COVOX bus sequence:
     * second AY -> R7=0x80 -> R15=sample. */
    select_chip(1);
    select_register(7);
    write_data(0x80);
    select_register(15);
    write_data(sample);
}
