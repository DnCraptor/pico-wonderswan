#include <pico.h>
#include <string.h>
#include "ws_audio.h"
#include "memory.h"
#include "audio.h"
#include "nec/necintrf.h"
#ifdef HWAY
#include "hway/ay_hway.h"
#endif

#define WS_AUDIO_CLOCK 3072000u
#define WS_AUDIO_RATE  24000u
#define WS_AUDIO_BLOCK 256u

extern uint8 internalRam[0x10000];
extern i2s_config_t i2s_config;

static uint16 period[4];
static uint8 volume[4];
static uint8 voice_volume;
static uint8 sweep_step, sweep_value;
static uint8 noise_control;
static uint8 control;
static uint8 output_control;
static uint8 sample_ram_pos;
static int32 period_counter[4];
static uint8 sample_pos[4];
static uint16 nreg;
static int32 sweep_divider;
static uint8 sweep_counter;
static uint16 sample_counter;
static uint32 audio_cpu_clock;
static bool audio_enabled = true;
static int32 dc_prev_in[2];
static int32 dc_prev_out[2];
static int16 pcm[WS_AUDIO_BLOCK * 2];
static uint16 pcm_frames;
static uint8 audio_rate_shift;
static uint32 audio_pending_cycles;
#ifdef HWAY
static uint8 hway_volume = 4;
static void hway_map_pitch(unsigned ch);
#endif

/* WonderSwan Color Hyper Voice (ports 64h..6Bh).  The fifth channel is a
 * signed 16-bit stereo path mixed after the four legacy channels. */
static int16 hyper_left;
static int16 hyper_right;
static uint8 hyper_input;
static uint8 hyper_control;
static uint8 hyper_channel_control;
static uint8 hyper_dma_left;
static uint8 hyper_manual_left;
static int16 hyper_pending_left;
static int16 hyper_pending_right;
static uint8 hyper_rate_counter;

static const uint8 hyper_rate_div[8] = { 1, 2, 3, 4, 5, 6, 8, 12 };

static int16 hyper_expand(uint8 value) {
    const unsigned shift = hyper_control & 3;
    int32 sample;

    switch ((hyper_control >> 2) & 3) {
        case 0:
            sample = (int32)value << (8 - shift);
            break;
        case 1:
            sample = ((int32)value - 256) << (8 - shift);
            break;
        case 2:
            sample = (int32)(int8)value << (8 - shift);
            break;
        default:
            /* Extension mode 3 ignores the volume/shift bits. */
            sample = (int32)value << 8;
            break;
    }
    return (int16)(uint16)sample;
}

static void hyper_latch_input(uint8 value, int from_dma) {
    const int16 sample = hyper_expand(value);
    hyper_input = value;
    if (from_dma) {
        const uint8 mode = (hyper_channel_control >> 5) & 3;
        if (mode == 0) {
            /* Stereo DMA alternates left/right input bytes. */
            if (hyper_dma_left) hyper_pending_left = sample;
            else                hyper_pending_right = sample;
            hyper_dma_left ^= 1;
        } else if (mode == 1) {
            hyper_pending_left = sample;
        } else if (mode == 2) {
            hyper_pending_right = sample;
        } else {
            hyper_pending_left = hyper_pending_right = sample;
        }
    } else {
        /* Manual writes to 69h always alternate as stereo. */
        if (hyper_manual_left) hyper_pending_left = sample;
        else                   hyper_pending_right = sample;
        hyper_manual_left ^= 1;
    }
}

static void hyper_clock(void) {
    if (!(hyper_control & 0x80)) return;
    if (hyper_rate_counter > 1) {
        --hyper_rate_counter;
        return;
    }
    hyper_rate_counter = hyper_rate_div[(hyper_control >> 4) & 7];
    hyper_left = hyper_pending_left;
    hyper_right = hyper_pending_right;
}

/* WonderSwan Color sound DMA (ports 4Ah..52h). The hardware clocks one
 * byte at 4/6/12/24 kHz from the 3.072 MHz CPU domain. */
static uint32 sound_dma_source;
static uint32 sound_dma_source_reload;
static uint32 sound_dma_size;
static uint32 sound_dma_size_reload;
static uint8 sound_dma_control;
static int32 sound_dma_counter;

static const uint16 sound_dma_period[4] = { 768, 512, 256, 128 };

static void __not_in_flash_func(sound_dma_tick)(void) {
    if (!(sound_dma_control & 0x80)) return;
    if (sound_dma_control & 0x04) {
        if (sound_dma_control & 0x10) hyper_latch_input(0, 1);
        else ws_audio_port_write(0x89, 0);
        return;
    }
    if (!sound_dma_size) {
        sound_dma_control &= 0x7f;
        return;
    }
    const uint8 sample = cpu_readmem20(sound_dma_source);
    if (sound_dma_control & 0x10) {
        hyper_latch_input(sample, 1);
    } else {
        ws_audio_port_write(0x89, sample);
    }
    sound_dma_size--;
    sound_dma_source = (sound_dma_source + ((sound_dma_control & 0x40) ? 0xfffffu : 1u)) & 0xfffffu;
    if (!sound_dma_size) {
        if (sound_dma_control & 0x08) {
            sound_dma_source = sound_dma_source_reload;
            sound_dma_size = sound_dma_size_reload;
        } else {
            sound_dma_control &= 0x7f;
        }
    }
}

static int16 clamp16(int32 v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16)v;
}

static int __not_in_flash_func(wave_sample)(unsigned ch) {
    const uint32 a = ((uint32)sample_ram_pos << 6) +
                     ((uint32)ch << 4) + (sample_pos[ch] >> 1);
    const uint8 b = internalRam[a & 0xffffu];
    return (b >> ((sample_pos[ch] & 1u) ? 4 : 0)) & 0x0f;
}

static void __not_in_flash_func(advance_channel)(unsigned ch, uint32 cycles) {
    if (!(control & (1u << ch))) return;

    if (ch == 1 && (control & 0x20))
        return; // Direct D/A channel has no wavetable phase to advance.

    if (ch == 2 && (control & 0x40) && sweep_value) {
        uint32 left = cycles;
        while (left) {
            uint32 step = left;
            if (step > (uint32)sweep_divider) step = (uint32)sweep_divider;
            sweep_divider -= (int32)step;
            if (sweep_divider <= 0) {
                sweep_divider += 8192;
                if (--sweep_counter == 0) {
                    sweep_counter = sweep_step + 1;
                    period[ch] = (period[ch] + (int8)sweep_value) & 0x7ff;
#ifdef HWAY
                    hway_map_pitch(ch);
#endif
                }
            }
            const uint32 pt = 2048u - period[ch];
            if (pt > 4) {
                period_counter[ch] -= (int32)step;
                while (period_counter[ch] <= 0) {
                    sample_pos[ch] = (sample_pos[ch] + 1) & 0x1f;
                    period_counter[ch] += (int32)pt;
                }
            }
            left -= step;
        }
        return;
    }

    const uint32 pt = 2048u - period[ch];
    if (pt <= 4) return;
    period_counter[ch] -= (int32)cycles;
    while (period_counter[ch] <= 0) {
        if (ch == 3 && (control & 0x80) && (noise_control & 0x10)) {
            static const uint8 stab[8] = { 14, 10, 13, 4, 8, 6, 9, 11 };
            nreg = ((nreg << 1) |
                    ((1 ^ (nreg >> 7) ^ (nreg >> stab[noise_control & 7])) & 1)) & 0x7fff;
        } else {
            sample_pos[ch] = (sample_pos[ch] + 1) & 0x1f;
        }
        period_counter[ch] += (int32)pt;
    }
}

static int32 __not_in_flash_func(dc_block)(unsigned ch, int32 input) {
    /* Mednafen's reference core feeds the unsigned hardware DAC levels into
     * Blip_Buffer with a 20 Hz bass filter.  At the native 24 kHz output rate
     * this fixed-point one-pole blocker provides the same essential DC
     * removal without a resampler or a large intermediate buffer. */
    const int32 delta = input - dc_prev_in[ch];
    const int32 output = delta + ((dc_prev_out[ch] * 32700) >> 15);
    dc_prev_in[ch] = input;
    dc_prev_out[ch] = output;
    return output;
}

#ifdef HWAY
/* WS -> dual AY-3-8910 approximation.  The WonderSwan has four 32x4-bit
 * wavetable voices, not 32 voices.  We map their note/control semantics to
 * four AY tone channels and reserve PCM for modes AY cannot represent. */
static const uint8 hway_ay_chip[4] = { 0, 0, 0, 1 };
static const uint8 hway_ay_chan[4] = { 0, 1, 2, 0 };
static uint8 hway_mixer_shadow[2] = { 0x38, 0xb8 };

static uint8 hway_master_shift(void) {
    static const uint8 shift[5] = { 4, 3, 2, 1, 0 };
    return shift[hway_volume <= 4 ? hway_volume : 4];
}

static void hway_map_pitch(unsigned ch) {
    uint16 n = 2048u - (period[ch] & 0x07ffu);
    if (!n) n = 1;
    const unsigned chip = hway_ay_chip[ch], aych = hway_ay_chan[ch];
    hway_write_register(chip, (uint8)(aych * 2u), (uint8)n);
    hway_write_register(chip, (uint8)(aych * 2u + 1u), (uint8)((n >> 8) & 0x0f));

    if (ch == 3 && (control & 0x80) && (noise_control & 0x10)) {
        uint16 np = n >> 5; /* WS LFSR clocks 32x faster than wave fundamental. */
        if (np < 1) np = 1;
        if (np > 31) np = 31;
        hway_write_register(chip, 6, (uint8)np);
    }
}

static void hway_map_volume(unsigned ch) {
    const unsigned chip = hway_ay_chip[ch], aych = hway_ay_chan[ch];
    uint8 v = volume[ch];
    uint8 level = ((v >> 4) > (v & 0x0f)) ? (v >> 4) : (v & 0x0f);
    level >>= hway_master_shift();
    if (!(control & (1u << ch))) level = 0;
    /* Channel 2 voice mode is real PCM, not an AY tone. */
    if (ch == 1 && (control & 0x20)) level = 0;
    hway_write_register(chip, (uint8)(8u + aych), level & 0x0f);
}

static void hway_map_mode(unsigned ch) {
    const unsigned chip = hway_ay_chip[ch], aych = hway_ay_chan[ch];
    uint8 mix = hway_mixer_shadow[chip];
    const uint8 tone_bit = (uint8)(1u << aych);
    const uint8 noise_bit = (uint8)(1u << (3u + aych));

    if (ch == 3 && (control & 0x80) && (noise_control & 0x10)) {
        mix |= tone_bit;       /* noise only */
        mix &= (uint8)~noise_bit;
    } else {
        mix &= (uint8)~tone_bit; /* AY square-wave approximation */
        mix |= noise_bit;
    }
    if (chip) mix |= 0x80; /* AY #2 port B remains DAC output. */
    hway_mixer_shadow[chip] = mix;
    hway_write_register(chip, 7, mix);
    hway_map_pitch(ch);
    hway_map_volume(ch);
}

static void hway_map_all(void) {
    for (unsigned ch = 0; ch < 4; ++ch) hway_map_mode(ch);
}
#endif

static void __not_in_flash_func(emit_sample)(void) {
    uint32 left = 0, right = 0;

    for (unsigned ch = 0; ch < 4; ++ch) {
        if (!(control & (1u << ch))) continue;

        if (ch == 1 && (control & 0x20)) {
            /* Reference Mednafen semantics: port 89h is an unsigned 8-bit
             * voice DAC value.  Do not reinterpret it as signed PCM. */
            const unsigned sample = volume[ch];
            const unsigned half = sample >> 1;
            left  += (voice_volume & 4) ? sample : (voice_volume & 8) ? half : 0;
            right += (voice_volume & 1) ? sample : (voice_volume & 2) ? half : 0;
            continue;
        }
#ifdef HWAY
        /* Normal wavetable/noise voices are produced by the physical AYs. */
        continue;
#endif

        const unsigned sample =
            (ch == 3 && (control & 0x80) && (noise_control & 0x10))
                ? ((nreg & 1) ? 15u : 0u)
                : (unsigned)wave_sample(ch);

        left  += sample * ((volume[ch] >> 4) & 0x0f);
        right += sample * (volume[ch] & 0x0f);
    }

    /* The headphone path is the 10-bit unsigned L/R legacy mix shifted by
     * five bits, then summed with signed 16-bit Hyper Voice. */
    int32 out_left = dc_block(0, (int32)(left << 5));
    int32 out_right = dc_block(1, (int32)(right << 5));

    hyper_clock();
    if (hyper_control & 0x80) {
        out_left += hyper_left;
        out_right += hyper_right;
    }

#ifndef HWAY
    const int16 sample_left = clamp16(out_left);
    const int16 sample_right = clamp16(out_right);
    const unsigned repeat = 1u << audio_rate_shift;

    /* The host DAC always stays at 24 kHz.  At lower emulation rates one
     * freshly generated stereo sample occupies 2/4/8 host sample slots. */
    for (unsigned i = 0; i < repeat; ++i) {
        pcm[pcm_frames * 2 + 0] = sample_left;
        pcm[pcm_frames * 2 + 1] = sample_right;
        if (++pcm_frames == WS_AUDIO_BLOCK) {
            i2s_dma_write(&i2s_config, pcm);
            pcm_frames = 0;
        }
    }
#else
    /* Match murm386 HWAY: the hardware DAC is the final PCM backend.  Feed it
     * continuously at the emulated audio sample clock, after the normal WS
     * mixer, instead of trying to infer PCM timing from guest port writes. */
    const int32 sample_left = clamp16(out_left);
    const int32 sample_right = clamp16(out_right);

    /* One full-volume WS wavetable channel is only about +/-3600 here.
     * Preserve one-sided stereo energy for the mono DAC, normalize that
     * channel close to full scale, and saturate louder combinations. */
    int32 mono = sample_left + sample_right;
    int32 scaled = (mono * 30000) / 3600;
    if (scaled < -32768) scaled = -32768;
    if (scaled >  32767) scaled =  32767;
    static const uint8 hway_gain_shift[5] = { 0, 3, 2, 1, 0 };
    scaled >>= hway_gain_shift[hway_volume];
    int32 dac = (scaled + 32768) >> 8;
    if (dac < 0) dac = 0;
    if (dac > 255) dac = 255;
    if (audio_enabled)
        hway_write_pcm((uint8)dac);
#endif
}

void ws_audio_init(void) {
    ws_audio_reset();
}

void ws_audio_reset(void) {
    memset(period, 0, sizeof(period));
    memset(volume, 0, sizeof(volume));
    memset(period_counter, 0, sizeof(period_counter));
    memset(sample_pos, 0, sizeof(sample_pos));
    voice_volume = 0;
    sweep_step = sweep_value = 0;
    noise_control = control = output_control = 0;
    sample_ram_pos = 0;
    nreg = 0;
    sweep_divider = 8192;
    sweep_counter = 1;
    sample_counter = (WS_AUDIO_CLOCK / WS_AUDIO_RATE) << audio_rate_shift;
    audio_cpu_clock = nec_get_clock();
    memset(dc_prev_in, 0, sizeof(dc_prev_in));
    memset(dc_prev_out, 0, sizeof(dc_prev_out));
    pcm_frames = 0;
    audio_pending_cycles = 0;
    hyper_left = hyper_right = 0;
    hyper_input = hyper_control = hyper_channel_control = 0;
    hyper_dma_left = hyper_manual_left = 1;
    hyper_pending_left = hyper_pending_right = 0;
    hyper_rate_counter = 1;
    sound_dma_source = sound_dma_source_reload = 0;
    sound_dma_size = sound_dma_size_reload = 0;
    sound_dma_control = 0;
    sound_dma_counter = 0;
    for (unsigned ch = 0; ch < 4; ++ch) period_counter[ch] = 1;
}

void __not_in_flash_func(ws_audio_process)(uint32 cycles) {
    /* 24 kHz is the reference path.  For the experimental lower rates keep
     * SDMA clocked at its original rate, but accumulate PSG time and advance
     * the four synthesis channels only at the selected 12/6/3 kHz boundary.
     * This intentionally trades timing precision for fewer advance_channel()
     * calls so the menu can measure their real performance cost. */
    while (cycles) {
        uint32 step = cycles;

        if (sound_dma_control & 0x80) {
            if (sound_dma_counter <= 0) {
                sound_dma_tick();
                if (sound_dma_control & 0x80)
                    sound_dma_counter += sound_dma_period[sound_dma_control & 3];
                continue;
            }
            if ((uint32)sound_dma_counter < step)
                step = (uint32)sound_dma_counter;
        }

        if (sample_counter < step)
            step = sample_counter;

        if (audio_rate_shift == 0) {
            for (unsigned ch = 0; ch < 4; ++ch)
                advance_channel(ch, step);
        } else {
            audio_pending_cycles += step;
        }

        sample_counter -= (uint16)step;
        if (sound_dma_control & 0x80)
            sound_dma_counter -= (int32)step;
        cycles -= step;

        /* A DMA write that lands on an output edge is visible to that sample. */
        if ((sound_dma_control & 0x80) && sound_dma_counter <= 0) {
            sound_dma_tick();
            if (sound_dma_control & 0x80)
                sound_dma_counter += sound_dma_period[sound_dma_control & 3];
        }

        if (!sample_counter) {
            if (audio_rate_shift != 0) {
                const uint32 pending = audio_pending_cycles;
                audio_pending_cycles = 0;
                for (unsigned ch = 0; ch < 4; ++ch)
                    advance_channel(ch, pending);
            }
            sample_counter = (WS_AUDIO_CLOCK / WS_AUDIO_RATE) << audio_rate_shift;
            emit_sample();
        }
    }
}

void __not_in_flash_func(ws_audio_sync)(void) {
    /* This function is on the scanline hot path.  In mute mode return before
     * even reading the emulated CPU clock: no channel stepping, sample
     * generation, filters, sound DMA or host PCM work is performed. */
    if (!audio_enabled) return;

    /* Keep the audio DMA continuously fed. i2s_dma_write() alone runs only
     * ~1.25x per frame (one 256-sample block), far less often than the PIO
     * FIFO drains, so pump here too (called ~twice per scanline). */
#ifndef HWAY
    i2s_dma_pump(&i2s_config);
#endif

    const uint32 now = nec_get_clock();
    const uint32 elapsed = now - audio_cpu_clock;
    if (elapsed) {
        ws_audio_process(elapsed);
        audio_cpu_clock = now;
    }
}

void ws_audio_set_enabled(int enabled) {
    const bool new_enabled = enabled != 0;
    if (audio_enabled == new_enabled) return;
    audio_enabled = new_enabled;
    audio_cpu_clock = nec_get_clock();
    if (!audio_enabled) {
        pcm_frames = 0;
#ifdef HWAY
        hway_write_pcm(0);
#endif
    }
}

#ifdef HWAY
void ws_audio_set_hway_volume(unsigned volume) {
    hway_volume = volume <= 4 ? (uint8)volume : 4;
    hway_map_all();
}
#endif

void ws_audio_set_rate_shift(unsigned shift) {
    if (shift > 3) shift = 3;
    if (audio_rate_shift == shift) return;
    audio_rate_shift = (uint8)shift;
    audio_pending_cycles = 0;
    /* Start a fresh interval at the selected emulation rate. */
    sample_counter = (WS_AUDIO_CLOCK / WS_AUDIO_RATE) << audio_rate_shift;
}

uint8 ws_audio_hyper_port_read(uint32 port) {
    switch (port) {
        case 0x64: return (uint8)hyper_left;
        case 0x65: return (uint8)((uint16)hyper_left >> 8);
        case 0x66: return (uint8)hyper_right;
        case 0x67: return (uint8)((uint16)hyper_right >> 8);
        case 0x69: return hyper_input;
        case 0x6a: return hyper_control;
        case 0x6b: return hyper_channel_control;
        default: return 0xff;
    }
}

void ws_audio_hyper_port_write(uint32 port, uint8 value) {
    switch (port) {
        case 0x64:
            hyper_left = hyper_pending_left =
                (int16)(((uint16)hyper_left & 0xff00u) | value);
            break;
        case 0x65:
            hyper_left = hyper_pending_left =
                (int16)(((uint16)value << 8) | ((uint16)hyper_left & 0x00ffu));
            break;
        case 0x66:
            hyper_right = hyper_pending_right =
                (int16)(((uint16)hyper_right & 0xff00u) | value);
            break;
        case 0x67:
            hyper_right = hyper_pending_right =
                (int16)(((uint16)value << 8) | ((uint16)hyper_right & 0x00ffu));
            break;
        case 0x69:
            hyper_latch_input(value, 0);
            break;
        case 0x6a:
            hyper_control = value;
            hyper_rate_counter = 1;
            break;
        case 0x6b:
            hyper_channel_control = value & 0x6f;
            if (value & 0x10) hyper_dma_left = 1;
            break;
        default:
            break;
    }
}

uint8 ws_audio_dma_port_read(uint32 port) {
    switch (port) {
        case 0x4a: return (uint8)sound_dma_source;
        case 0x4b: return (uint8)(sound_dma_source >> 8);
        case 0x4c: return (uint8)(sound_dma_source >> 16);
        case 0x4d: return 0;
        case 0x4e: return (uint8)sound_dma_size;
        case 0x4f: return (uint8)(sound_dma_size >> 8);
        case 0x50: return (uint8)(sound_dma_size >> 16);
        case 0x51: return 0;
        case 0x52: return sound_dma_control;
        case 0x53: return 0;
        default: return 0xff;
    }
}

void ws_audio_dma_port_write(uint32 port, uint8 value) {
    switch (port) {
        case 0x4a:
            sound_dma_source = (sound_dma_source & 0x0fff00u) | value;
            sound_dma_source_reload = (sound_dma_source_reload & 0x0fff00u) | value;
            break;
        case 0x4b:
            sound_dma_source = (sound_dma_source & 0x0f00ffu) | ((uint32)value << 8);
            sound_dma_source_reload = (sound_dma_source_reload & 0x0f00ffu) | ((uint32)value << 8);
            break;
        case 0x4c:
            sound_dma_source = (sound_dma_source & 0x00ffffu) | ((uint32)(value & 0x0f) << 16);
            sound_dma_source_reload = (sound_dma_source_reload & 0x00ffffu) | ((uint32)(value & 0x0f) << 16);
            break;
        case 0x4e:
            sound_dma_size = (sound_dma_size & 0x0fff00u) | value;
            sound_dma_size_reload = (sound_dma_size_reload & 0x0fff00u) | value;
            break;
        case 0x4f:
            sound_dma_size = (sound_dma_size & 0x0f00ffu) | ((uint32)value << 8);
            sound_dma_size_reload = (sound_dma_size_reload & 0x0f00ffu) | ((uint32)value << 8);
            break;
        case 0x50:
            sound_dma_size = (sound_dma_size & 0x00ffffu) | ((uint32)(value & 0x0f) << 16);
            sound_dma_size_reload = (sound_dma_size_reload & 0x00ffffu) | ((uint32)(value & 0x0f) << 16);
            break;
        case 0x52: {
            const uint8 old_control = sound_dma_control;
            sound_dma_control = value;
            if ((value & 0x80) && !sound_dma_size) {
                /* Hardware refuses to start SDMA with a zero transfer length. */
                sound_dma_control &= 0x7f;
            } else if ((value & 0x80) && !(old_control & 0x80)) {
                /* Starting DMA arms the selected divider.  Writes while it is
                 * already running change control bits without restarting phase. */
                sound_dma_counter = sound_dma_period[value & 3];
            }
            break;
        }
        default:
            break;
    }
}

uint8 ws_audio_port_read(uint32 port) {
    if (port >= 0x80 && port <= 0x87) {
        const unsigned ch = (port - 0x80) >> 1;
        return (port & 1) ? (uint8)(period[ch] >> 8) : (uint8)period[ch];
    }
    if (port >= 0x88 && port <= 0x8b)
        return volume[port - 0x88];
    switch (port) {
        case 0x8c: return sweep_value;
        case 0x8d: return sweep_step;
        case 0x8e: return noise_control;
        case 0x8f: return sample_ram_pos;
        case 0x90: return control;
        case 0x91: return output_control | 0x80;
        case 0x92: return (uint8)nreg;
        case 0x93: return (uint8)(nreg >> 8);
        case 0x94: return voice_volume;
        default: return 0;
    }
}

void ws_audio_port_write(uint32 port, uint8 value) {
    if (port >= 0x80 && port <= 0x87) {
        const unsigned ch = (port - 0x80) >> 1;
        if (port & 1)
            period[ch] = (period[ch] & 0x00ff) | ((value & 7) << 8);
        else
            period[ch] = (period[ch] & 0x0700) | value;
#ifdef HWAY
        hway_map_pitch(ch);
#endif
        return;
    }
    if (port >= 0x88 && port <= 0x8b) {
        const unsigned ch = port - 0x88;
        volume[ch] = value;
#ifdef HWAY
        hway_map_volume(ch);
#endif
        return;
    }
    switch (port) {
        case 0x8c: sweep_value = value; break;
        case 0x8d:
            sweep_step = value;
            sweep_counter = sweep_step + 1;
            sweep_divider = 8192;
            break;
        case 0x8e:
            if (value & 8) nreg = 0;
            noise_control = value & 0x17;
#ifdef HWAY
            hway_map_mode(3);
#endif
            break;
        case 0x8f: sample_ram_pos = value; break;
        case 0x90:
            for (unsigned ch = 0; ch < 4; ++ch) {
                if (!(control & (1u << ch)) && (value & (1u << ch))) {
                    period_counter[ch] = 1;
                    sample_pos[ch] = 0x1f;
                }
            }
            control = value;
#ifdef HWAY
            hway_map_all();
#endif
            break;
        case 0x91: output_control = value & 0x0f; break;
        case 0x92: nreg = (nreg & 0x7f00) | value; break;
        case 0x93: nreg = (nreg & 0x00ff) | ((value & 0x7f) << 8); break;
        case 0x94: voice_volume = value & 0x0f; break;
        default: break;
    }
}


#ifdef HWAY
void ws_audio_hway_sync(void) {
    hway_mixer_shadow[0] = 0x38;
    hway_mixer_shadow[1] = 0xb8;
    hway_map_all();
}

void ws_audio_hway_silence(void) {
    /* Silence only the AY tone/noise generators. AY #2 port B remains DAC. */
    hway_write_register(0, 8, 0);
    hway_write_register(0, 9, 0);
    hway_write_register(0, 10, 0);
    hway_write_register(1, 8, 0);
    hway_write_register(1, 9, 0);
    hway_write_register(1, 10, 0);
}
#endif

void ws_audio_snapshot_get(ws_audio_snapshot_t *s) {
 memcpy(s->period,period,sizeof(period)); memcpy(s->volume,volume,sizeof(volume)); s->voice_volume=voice_volume; s->sweep_step=sweep_step; s->sweep_value=sweep_value; s->noise_control=noise_control; s->control=control; s->output_control=output_control; s->sample_ram_pos=sample_ram_pos; memcpy(s->period_counter,period_counter,sizeof(period_counter)); memcpy(s->sample_pos,sample_pos,sizeof(sample_pos)); s->nreg=nreg; s->sweep_divider=sweep_divider; s->sweep_counter=sweep_counter; s->sample_counter=sample_counter; memcpy(s->dc_prev_in,dc_prev_in,sizeof(dc_prev_in)); memcpy(s->dc_prev_out,dc_prev_out,sizeof(dc_prev_out)); s->audio_pending_cycles=audio_pending_cycles; s->hyper_left=hyper_left; s->hyper_right=hyper_right; s->hyper_input=hyper_input; s->hyper_control=hyper_control; s->hyper_channel_control=hyper_channel_control; s->hyper_dma_left=hyper_dma_left; s->hyper_manual_left=hyper_manual_left; s->hyper_pending_left=hyper_pending_left; s->hyper_pending_right=hyper_pending_right; s->hyper_rate_counter=hyper_rate_counter; s->sound_dma_source=sound_dma_source; s->sound_dma_source_reload=sound_dma_source_reload; s->sound_dma_size=sound_dma_size; s->sound_dma_size_reload=sound_dma_size_reload; s->sound_dma_control=sound_dma_control; s->sound_dma_counter=sound_dma_counter;
}
void ws_audio_snapshot_set(const ws_audio_snapshot_t *s) {
 memcpy(period,s->period,sizeof(period)); memcpy(volume,s->volume,sizeof(volume)); voice_volume=s->voice_volume; sweep_step=s->sweep_step; sweep_value=s->sweep_value; noise_control=s->noise_control; control=s->control; output_control=s->output_control; sample_ram_pos=s->sample_ram_pos; memcpy(period_counter,s->period_counter,sizeof(period_counter)); memcpy(sample_pos,s->sample_pos,sizeof(sample_pos)); nreg=s->nreg; sweep_divider=s->sweep_divider; sweep_counter=s->sweep_counter; sample_counter=s->sample_counter; memcpy(dc_prev_in,s->dc_prev_in,sizeof(dc_prev_in)); memcpy(dc_prev_out,s->dc_prev_out,sizeof(dc_prev_out)); audio_pending_cycles=s->audio_pending_cycles; hyper_left=s->hyper_left; hyper_right=s->hyper_right; hyper_input=s->hyper_input; hyper_control=s->hyper_control; hyper_channel_control=s->hyper_channel_control; hyper_dma_left=s->hyper_dma_left; hyper_manual_left=s->hyper_manual_left; hyper_pending_left=s->hyper_pending_left; hyper_pending_right=s->hyper_pending_right; hyper_rate_counter=s->hyper_rate_counter; sound_dma_source=s->sound_dma_source; sound_dma_source_reload=s->sound_dma_source_reload; sound_dma_size=s->sound_dma_size; sound_dma_size_reload=s->sound_dma_size_reload; sound_dma_control=s->sound_dma_control; sound_dma_counter=s->sound_dma_counter; audio_cpu_clock=nec_get_clock(); pcm_frames=0;
#ifdef HWAY
 hway_map_all();
#endif
}
