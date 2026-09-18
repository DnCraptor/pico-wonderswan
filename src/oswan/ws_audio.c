#include <string.h>
#include "ws_audio.h"
#include "memory.h"
#include "audio.h"

#define WS_AUDIO_CLOCK 3072000u
#define WS_AUDIO_RATE  22050u
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
static uint32 sample_accum;
static int16 pcm[WS_AUDIO_BLOCK * 2];
static uint16 pcm_frames;

static int16 clamp16(int32 v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16)v;
}

static int wave_sample(unsigned ch) {
    const uint32 a = ((uint32)sample_ram_pos << 6) +
                     ((uint32)ch << 4) + (sample_pos[ch] >> 1);
    const uint8 b = internalRam[a & 0xffffu];
    return (b >> ((sample_pos[ch] & 1u) ? 4 : 0)) & 0x0f;
}

static void advance_channel(unsigned ch, uint32 cycles) {
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

static void emit_sample(void) {
    int32 left = 0, right = 0;

    for (unsigned ch = 0; ch < 4; ++ch) {
        if (!(control & (1u << ch))) continue;

        int s;
        if (ch == 1 && (control & 0x20)) {
            const int dac = (int)volume[ch] - 128;
            const int half = dac >> 1;
            left  += (voice_volume & 4) ? dac : (voice_volume & 8) ? half : 0;
            right += (voice_volume & 1) ? dac : (voice_volume & 2) ? half : 0;
            continue;
        }

        if (ch == 3 && (control & 0x80) && (noise_control & 0x10))
            s = (nreg & 1) ? 7 : -8;
        else
            s = wave_sample(ch) - 8;

        left  += s * ((volume[ch] >> 4) & 0x0f);
        right += s * (volume[ch] & 0x0f);
    }

    // Four wavetable channels peak at about +/-480.  A factor of 48 leaves
    // headroom for direct D/A while using most of the signed 16-bit range.
    pcm[pcm_frames * 2 + 0] = clamp16(left * 48);
    pcm[pcm_frames * 2 + 1] = clamp16(right * 48);
    if (++pcm_frames == WS_AUDIO_BLOCK) {
        i2s_dma_write(&i2s_config, pcm);
        pcm_frames = 0;
    }
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
    sample_accum = 0;
    pcm_frames = 0;
    for (unsigned ch = 0; ch < 4; ++ch) period_counter[ch] = 1;
}

void ws_audio_process(uint32 cycles) {
    for (unsigned ch = 0; ch < 4; ++ch)
        advance_channel(ch, cycles);

    sample_accum += cycles * WS_AUDIO_RATE;
    while (sample_accum >= WS_AUDIO_CLOCK) {
        sample_accum -= WS_AUDIO_CLOCK;
        emit_sample();
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
        return;
    }
    if (port >= 0x88 && port <= 0x8b) {
        volume[port - 0x88] = value;
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
            break;
        case 0x91: output_control = value & 0x0f; break;
        case 0x92: nreg = (nreg & 0x7f00) | value; break;
        case 0x93: nreg = (nreg & 0x00ff) | ((value & 0x7f) << 8); break;
        case 0x94: voice_volume = value & 0x0f; break;
        default: break;
    }
}
