#include "ay_hway.h"
#include "pico/stdlib.h"
#include "pico/platform.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"

/* Same physical HWAY/TurboSound serial bus used by pico-gamate and murm386:
 * two cascaded 74HC595s, latch=AUDIO_DATA_PIN, clock=AUDIO_CLOCK_PIN,
 * serial data=AUDIO_CLOCK_PIN+1. */
#define HWAY_LATCH_PIN AUDIO_DATA_PIN
#define HWAY_CLOCK_PIN AUDIO_CLOCK_PIN
#define HWAY_DATA_PIN  (AUDIO_CLOCK_PIN + 1)
#define CLK_AY_PIN      21

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

#define HWAY_PCM_QUEUE_SIZE 256u
#define HWAY_PCM_QUEUE_MASK (HWAY_PCM_QUEUE_SIZE - 1u)
static uint8_t pcm_queue[HWAY_PCM_QUEUE_SIZE];
static volatile uint32_t pcm_write_pos;
static volatile uint32_t pcm_read_pos;
static uint64_t pcm_next_us;
static uint32_t pcm_frac;

#define HWAY_REG_QUEUE_SIZE 256u
#define HWAY_REG_QUEUE_MASK (HWAY_REG_QUEUE_SIZE - 1u)
typedef struct { uint8_t chip, reg, value; } hway_reg_cmd_t;
static hway_reg_cmd_t reg_queue[HWAY_REG_QUEUE_SIZE];
static volatile uint32_t reg_write_pos;
static volatile uint32_t reg_read_pos;
/* R7 shadows. Chip 1 bit 7 must stay set because its port B is the PCM DAC. */
static uint8_t ay_mixer[2] = { 0x38, 0xb8 };

static inline void hway_wait_to_adjust(uint32_t wait_nops) {
    for (uint32_t i = 0; i < wait_nops; ++i)
        __asm volatile("nop");
}

static void __not_in_flash_func(hway_shift16)(uint16_t data) {
    /* Final working murm386 74HC595 timing: about 30 MHz maximum shift
     * clock, with explicit setup/hold time around every edge. */
    static uint32_t wait_nops;
    if (wait_nops == 0)
        wait_nops = clock_get_hz(clk_sys) / (30000000u * 5u);

    gpio_put(HWAY_CLOCK_PIN, 0);
    hway_wait_to_adjust(wait_nops);

    for (int i = 0; i < 16; ++i) {
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
    chip &= 1u;
    reg &= 0x0fu;
    if (reg == 7) {
        if (chip) value |= 0x80u;
        ay_mixer[chip] = value;
    }

    /* Core0 never touches the physical 595 bus.  Register changes share the
     * same core1 ownership model as PCM. */
    const uint32_t write = __atomic_load_n(&reg_write_pos, __ATOMIC_RELAXED);
    const uint32_t read = __atomic_load_n(&reg_read_pos, __ATOMIC_ACQUIRE);
    if (write - read >= HWAY_REG_QUEUE_SIZE) return;
    reg_queue[write & HWAY_REG_QUEUE_MASK] = (hway_reg_cmd_t){ (uint8_t)chip, reg, value };
    __atomic_store_n(&reg_write_pos, write + 1u, __ATOMIC_RELEASE);
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
    control_bits = AY_CS_SAA1099 | AY_ENABLE | AY_SAVE |
                   AY_CS1 | AY_CS0 | AY_BDIR | AY_BC1;
    hway_shift16(control_bits);

    /* PCM-only stage: exactly like murm386, no AY master clock is needed
     * to drive the asynchronous register bus and port-B DAC. */
    /* 1.536 MHz makes AY tone period N exactly match WonderSwan's
     * (2048 - divisor): 1.536 MHz/(16*N) == 96 kHz/N. */
    gpio_set_function(CLK_AY_PIN, GPIO_FUNC_PWM);
    pwm_config ay_clock = pwm_get_default_config();
    pwm_config_set_wrap(&ay_clock, 1);
    pwm_config_set_clkdiv(&ay_clock, (float)clock_get_hz(clk_sys) / 3072000.0f);
    pwm_init(pwm_gpio_to_slice_num(CLK_AY_PIN), &ay_clock, true);
    pwm_set_gpio_level(CLK_AY_PIN, 1);

    last_pcm_valid = false;
    ay_mixer[0] = 0x38;
    ay_mixer[1] = 0xb8;
    __atomic_store_n(&reg_write_pos, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&reg_read_pos, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&pcm_write_pos, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&pcm_read_pos, 0, __ATOMIC_RELAXED);
    pcm_next_us = time_us_64();
    pcm_frac = 0;
}

void __not_in_flash_func(hway_write_pcm)(uint8_t sample) {
    /* SPSC queue: core0 produces complete 24 kHz PCM samples; core1 consumes
     * them at the physical sample clock.  Never collapse the stream to the
     * newest byte: that destroys PCM timing whenever emulation runs in bursts. */
    const uint32_t write = __atomic_load_n(&pcm_write_pos, __ATOMIC_RELAXED);
    const uint32_t read = __atomic_load_n(&pcm_read_pos, __ATOMIC_ACQUIRE);
    if (write - read >= HWAY_PCM_QUEUE_SIZE)
        return;
    pcm_queue[write & HWAY_PCM_QUEUE_MASK] = sample;
    __atomic_store_n(&pcm_write_pos, write + 1u, __ATOMIC_RELEASE);
}

void __not_in_flash_func(hway_poll)(void) {
    /* Apply all pending musical register changes before servicing the next
     * DAC slot.  Only core1 executes hway_shift16(). */
    for (;;) {
        const uint32_t read = __atomic_load_n(&reg_read_pos, __ATOMIC_RELAXED);
        const uint32_t write = __atomic_load_n(&reg_write_pos, __ATOMIC_ACQUIRE);
        if (read == write) break;
        const hway_reg_cmd_t cmd = reg_queue[read & HWAY_REG_QUEUE_MASK];
        __atomic_store_n(&reg_read_pos, read + 1u, __ATOMIC_RELEASE);
        select_chip(cmd.chip);
        select_register(cmd.reg);
        write_data(cmd.value);
    }

    uint64_t now = time_us_64();
    while ((int64_t)(now - pcm_next_us) >= 0) {
        /* Exact 24 kHz pacing: 41 + 2/3 us per sample. */
        pcm_next_us += 41u;
        pcm_frac += 2u;
        if (pcm_frac >= 3u) {
            pcm_frac -= 3u;
            ++pcm_next_us;
        }

        const uint32_t read = __atomic_load_n(&pcm_read_pos, __ATOMIC_RELAXED);
        const uint32_t write = __atomic_load_n(&pcm_write_pos, __ATOMIC_ACQUIRE);
        if (read == write) {
            /* No backlog means there is nothing to catch up later. */
            pcm_next_us = now + 41u;
            pcm_frac = 2u;
            break;
        }

        const uint8_t sample = pcm_queue[read & HWAY_PCM_QUEUE_MASK];
        __atomic_store_n(&pcm_read_pos, read + 1u, __ATOMIC_RELEASE);

        if (last_pcm_valid && sample == last_pcm)
            continue;
        last_pcm = sample;
        last_pcm_valid = true;

        select_chip(1);
        select_register(7);
        write_data(ay_mixer[1]);
        select_register(15);
        write_data(sample);

        now = time_us_64();
    }
}
