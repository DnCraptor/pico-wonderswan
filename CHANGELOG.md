# v1.0.6

- Added RP2350 support and release targets for m1p2, m2p2, PCp2 and z0p2.
- Added HDMI and software-composite (SOFTTV) video outputs alongside VGA, with output-specific backplanes and OSD support.
- Added PWM, I2S and hardware AY-3-8910/PCM DAC (HWAY) audio backends.
- Reworked video buffering and timing for the WonderSwan 75.47 Hz refresh rate, including triple buffering and improved frame pacing.
- Added configurable frame skipping with Auto mode, including automatic fallback down to 9 fps for demanding games.
- Added WonderSwan ROM prefetching, faster internal-RAM access and cached QSPI-PSRAM ROM access for substantially improved emulation performance.
- Added support for 8 MiB ROMs in QSPI PSRAM and improved operation without external PSRAM.
- Added a dedicated 32 KiB internal cartridge-SRAM buffer; cartridges requiring up to 32 KiB SRAM now use RP2350 SRAM before external/file-backed storage.
- Fixed QSPI-PSRAM selection on m1p2 RP2350A (GP19) versus RP2350B (GP47), avoiding the legacy-PSRAM GPIO conflict.
- Added save states and eight quick-save/quick-load slots.
- Added Demo mode, Demo autostart support, configurable countdown, start-from-selected-file behavior and F10 return to the current ROM in the file browser.
- Added FPS OSD and Demo title/countdown overlays for HDMI, VGA and SOFTTV.
- Added PageUp/PageDown navigation and multiple file-browser usability fixes.
- Added monochrome palette presets, custom palette editing, per-game palette storage, random/shuffle modes and fast palette hotkeys.
- Added configurable sprite/background Z-index handling and palette inspection tools.
- Added optional WonderSwan-style backplanes for HDMI, VGA and SOFTTV, including portrait/rotated variants and runtime toggle.
- Expanded HDMI palette handling to keep WonderSwan colour indices separate from HDMI-private control indices.
- Added Hyper Voice emulation and improved WonderSwan audio timing/mixing.
- Reworked audio DMA, I2S streaming and HWAY queues; added dynamic peak limiting, soft clipping, volume and mute handling.
- Improved NEC V30 cycle accounting, timers and interrupt timing.
- Added USB keyboard/HID support improvements, including support for more keyboard devices, plus PS/2 keyboard mapping updates.
- Fixed sprite blinking, OPKL handling, menu access, reboot behavior, VGA scanout/OSD cleanup, SOFTTV PAL line sizing and multiple Demo/audio/palette edge cases.
- Moved persistent save data under `/.config` and improved PSRAM/EEPROM cleanup when switching games.
- Updated flash-size detection and z0p2 flash timing; repaired m2p2 builds with Pico SDK 2.3.1.

# v1.0.0

Initial
