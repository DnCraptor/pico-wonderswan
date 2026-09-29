# pico-wonderswan v1.0.6

Version 1.0.6 is a major update over the previous release. It expands RP2350 board support, adds HDMI and software-composite video, introduces new audio backends and Demo/palette/backplane features, and contains substantial CPU, memory, video and audio performance work.

## Supported release targets

The v1.0.6 release set is intended for four RP2350 boards:

- `m1p2` — Murmulator M1 / RP2350
- `m2p2` — Murmulator M2 / RP2350
- `PCp2` — Olimex Pico-PC / RP2350
- `z0p2` — Waveshare RP2350 PiZero

Each board is built with three video backends and three audio backends:

- Video: `HDMI`, `VGA`, `TV-SOFT`
- Audio: `I2S`, `PWM`, `HWAY`

This gives 36 release firmware combinations.

| Board | Video | Audio variants |
| --- | --- | --- |
| m1p2 | HDMI | I2S / PWM / HWAY |
| m1p2 | VGA | I2S / PWM / HWAY |
| m1p2 | TV-SOFT | I2S / PWM / HWAY |
| m2p2 | HDMI | I2S / PWM / HWAY |
| m2p2 | VGA | I2S / PWM / HWAY |
| m2p2 | TV-SOFT | I2S / PWM / HWAY |
| PCp2 | HDMI | I2S / PWM / HWAY |
| PCp2 | VGA | I2S / PWM / HWAY |
| PCp2 | TV-SOFT | I2S / PWM / HWAY |
| z0p2 | HDMI | I2S / PWM / HWAY |
| z0p2 | VGA | I2S / PWM / HWAY |
| z0p2 | TV-SOFT | I2S / PWM / HWAY |

Firmware names follow the pattern:

```text
<board>-wonderswan-<video>-<audio>-1.0.6.uf2
```

For example:

```text
m1p2-wonderswan-HDMI-I2S-1.0.6.uf2
m2p2-wonderswan-VGA-PWM-1.0.6.uf2
PCp2-wonderswan-TV-SOFT-HWAY-1.0.6.uf2
z0p2-wonderswan-HDMI-PWM-1.0.6.uf2
```

## RP2350 and board support

The emulator now has first-class RP2350 support for the release boards above. Board-specific PSRAM, video, audio, keyboard and flash configuration has been separated so the same emulator core can be built for the different hardware layouts.

m1p2 supports the different QSPI-PSRAM CS1 routing used by the two RP2350 packages: GP19 on RP2350A and GP47 on RP2350B. The RP2350A path avoids initializing the conflicting legacy SPI-PSRAM interface when QSPI PSRAM is active.

z0p2 received additional startup/stability work and now uses the same conservative flash timing approach as the other RP2350 targets. m2p2 was also updated for Pico SDK 2.3.1.

## Video outputs and frame pacing

HDMI and SOFTTV are now supported in addition to VGA. The display path was reworked around the native WonderSwan refresh rate, including triple buffering and a 75.47 Hz frame limit to provide smoother motion and reduce visible pacing artifacts.

SOFTTV received a new software-composite pipeline, PAL line-size fixes, performance work, backplane composition and OSD support. VGA received scanout, palette, backplane and OSD fixes. HDMI received hot-path optimizations and higher interrupt priority for more stable scanout under load.

Frame Skip now includes `Auto`, `75 Hz`, `66 Hz`, `57 Hz`, `47 Hz`, `38 Hz`, `28 Hz`, `19 Hz` and `9 Hz`. Auto mode can progressively reduce the emulated frame rate when a game is too demanding instead of forcing the system to miss real-time deadlines unpredictably.

## Emulation performance

The NEC V30 core and memory path received several performance-oriented changes. Instruction ROM prefetching was added, frequently used state and hot functions were moved out of flash where appropriate, internal-RAM accesses were streamlined, and CPU cycle/timer accounting was refined.

The most important external-memory change is that runtime QSPI-PSRAM ROM reads now use the cached XIP mapping rather than the uncached alias. This removes a major bottleneck caused by large numbers of small ROM accesses and substantially improves demanding games.

8 MiB ROM images can use the full QSPI-PSRAM ROM area. Cartridge save memory no longer has to consume a contiguous region immediately following the ROM. Cartridges requiring up to 32 KiB SRAM preferentially use a dedicated 32 KiB RP2350 SRAM buffer, leaving the external PSRAM available for ROM data. Larger save-memory configurations retain the existing external/file-backed paths.

The emulator also has improved fallback behavior for boards or configurations without external PSRAM.

## HDMI colour-index handling

HDMI palette handling was expanded to 248 normal colour indices. The HDMI-private indices at 248-255 are kept out of the guest palette space, with WonderSwan accesses to those values rolled to 0-7 where required. This prevents guest WSC palette data from colliding with HDMI control entries.

## Backplanes and colour controls

The release adds optional WonderSwan-style backplanes for HDMI, VGA and SOFTTV, including output-specific and rotated/portrait layouts. `F11` toggles the backplane at runtime.

Monochrome games gain a much larger colour-management system: built-in presets, custom colours, per-game palette storage, palette inspection, random generation and shuffle modes. The colour editor can also expose and adjust the emulator's custom Z-index ordering.

The final hotkey layout includes fast access to the main colour operations, while `F12` opens the colour panel. Palette settings can be linked to individual games and saved under the configuration area.

## Save states and file browser

Save-state support was added, including eight quick slots. Quick save/restore is available through the function-key combinations implemented by the emulator.

The file browser now supports PageUp/PageDown navigation and remembers enough context to return to the current ROM. Pressing `F10` pauses the current game and enters the file manager rather than unloading the emulator state; returning without selecting another ROM resumes the same live core. When re-entering the browser, the current ROM is scrolled into view.

Persistent save data was moved under `/.config`, and cartridge PSRAM/EEPROM workspace is cleaned appropriately when switching games.

## Demo mode

Demo mode can automatically cycle through ROMs and can be configured to start at boot on supported board configurations. Demo can begin from the file currently highlighted in the browser rather than always restarting from the first ROM.

The current game name is shown briefly after a Demo game starts and then disappears after 10 seconds. The Demo countdown is now a separate OSD element positioned directly below the FPS display and is visible from the beginning of the Demo interval. HDMI, VGA and SOFTTV all implement the separate title/countdown presentation.

`F10` can be used from Demo mode to enter the browser focused on the currently running ROM.

## Audio

The audio subsystem now supports three release backends: PWM, I2S and HWAY. HWAY drives an external AY-3-8910-compatible hardware path together with the PCM DAC output used by the project.

WonderSwan audio emulation gained Hyper Voice support, improved timing and more detailed channel handling. Output was moved to 24 kHz, and the DMA path was reworked to operate asynchronously. I2S buffering/DMA behavior was improved for heavy video loads.

The mixer now includes dynamic peak limiting and soft clipping. Volume and mute handling stop unnecessary sound emulation when appropriate. HWAY register/PCM queues and Demo shutdown behavior were also corrected.

## Input and controls

USB keyboard handling was fixed and broadened to accept more HID keyboard devices. PS/2 mappings and several function-key assignments were revised as the OSD, palette, Demo and file-browser controls evolved.

`F1` provides game-mode help, and the on-screen help reflects the current hotkey layout.

## Accuracy and fixes

NEC V30 cycle accounting and timer behavior were refined to better match the emulated hardware. Other fixes in this release include sprite blinking, OPKL behavior, menu access, reboot handling, VGA scanout/template cleanup, SOFTTV PAL sizing and colour handling, audio DMA edge cases, HWAY shutdown, Demo transitions, palette persistence and external-memory cleanup.

Flash-size detection was also made more conservative to avoid relying on unsupported or incorrectly reported flash capacity.

## Upgrade notes

Version 1.0.6 changes several persistent-storage and runtime-memory paths. Save/configuration files are expected under `/.config`; users upgrading an existing SD card should preserve their existing saves and configuration when replacing firmware.

The firmware variant must match the physical board, video connection and audio hardware. In particular, `HWAY` builds are intended for systems equipped with the corresponding external hardware audio interface; use `I2S` or `PWM` for the normal digital/PWM audio paths.
