# Raspberry Pi Pico WonderSwan / WonderSwan Color emulator

WonderSwan and WonderSwan Color emulator for RP2350-based MURMULATOR-compatible boards.

The project is based on the Oswan WonderSwan emulator core and targets small standalone systems with SD/USB storage, keyboard/gamepad input and VGA, HDMI or software composite-video output.

## Release targets

The RP2350 release is built for four board families:

| Prefix | Board |
| --- | --- |
| `m1p2` | MURMULATOR 1.x / RP2350 |
| `m2p2` | MURMULATOR 2.0 / RP2350 |
| `PCp2` | Olimex RP2350-PICO-PC |
| `z0p2` | Waveshare RP2350-PiZero |

Each board is released with three video backends:

- `HDMI` - digital HDMI output;
- `VGA` - analog VGA output;
- `TV-SOFT` - software-generated composite TV output.

And three audio backends:

- `I2S` - digital I2S audio;
- `PWM` - PWM audio;
- `AY-3-8910` / HWAY - external hardware AY-3-8910-compatible PSG plus PCM DAC output.

This gives 36 standard RP2350 release combinations.

Firmware names follow the form:

```text
<board>-wonderswan-<video>-<audio>-<version>.uf2
```

For example:

```text
m1p2-wonderswan-HDMI-I2S-1.0.6.uf2
m2p2-wonderswan-VGA-PWM-1.0.6.uf2
z0p2-wonderswan-TV-SOFT-AY-3-8910-1.0.6.uf2
```

## Storage and ROMs

ROM images use the normal `.ws` and `.wsc` formats and are selected with the built-in file browser.

The emulator supports SD-card storage and USB mass-storage devices. QSPI PSRAM is used as fast cartridge-ROM storage where available. On supported RP2350 boards the implementation can use the RP2350 XIP/QSPI path directly, including cached XIP reads for substantially lower ROM-access overhead.

Cartridge save memory is handled separately from ROM storage. Small cartridge SRAM images (up to 32 KiB) use dedicated internal SRAM first; larger save memories fall back to the available external/backing-storage paths.

## Video

### HDMI

Digital HDMI output with the WonderSwan image composited into the selected backplane. HDMI builds support the same runtime palette, OSD and Demo features as the other video backends.

### VGA

Analog VGA output for MURMULATOR-compatible VGA hardware. The VGA renderer includes dedicated OSD scanline templates so overlays do not modify the emulated framebuffer.

### Software composite TV

`TV-SOFT` generates the composite signal in software. It has its own scanout path and supports the normal game image, backplane and OSD overlays.

## Audio

The emulator can be built for I2S, PWM or HWAY output. HWAY sends WonderSwan PSG register activity to an external AY-3-8910-compatible device and uses the hardware PCM DAC path for sampled audio.

## Input

USB HID keyboard and USB gamepads are supported. MURMULATOR-compatible PS/2 keyboard and gamepad inputs are also supported where provided by the board.

The normal keyboard mapping includes cursor/WASD-style direction controls, `Z`/`X` for B/A, Enter for Start and Backspace/Escape for the auxiliary Select/menu action used by the frontend.

## File browser and controls

The built-in browser can load `.ws` and `.wsc` images from the available storage devices. `F10` opens the browser and can return to the currently running game without unnecessarily resetting the live emulation state.

Useful runtime hotkeys:

| Key | Function |
| --- | --- |
| `F1..F5` | Next palette for the selected layer |
| `Shift+F1..F5` | Previous palette for the selected layer |
| `F6` | Shuffle colors without saving |
| `F7` | Shuffle colors and save |
| `F8` | Save colors for the current game |
| `F9` | Restore all defaults |
| `F10` | File manager |
| `F11` | Backplane on/off |
| `F12` | Custom palette editor |
| `Tab` | Change layer in the palette editor |
| `Ctrl+F1..F8` | Save state 1..8 |
| `Alt+F1..F8` | Load state 1..8 |

## Runtime options

The on-screen configuration includes, among other settings:

- FPS overlay;
- automatic or fixed frame skipping;
- backplane control;
- orientation and presentation options;
- audio/runtime settings appropriate to the selected build;
- Demo duration and Demo start;
- per-game palette controls.

Demo mode can cycle through ROMs automatically. The current game name is shown temporarily after a game starts, while a separate countdown below the FPS line shows the remaining Demo time.

## Color and backplane support

The emulator supports per-layer palette selection, random palette generation, per-game saved color files and an interactive palette editor. Backplane rendering is available on the supported video backends and can be toggled at runtime.

## Build configuration

The main CMake options are:

```text
PICO_PLATFORM=rp2350
PICO_BOARD=murmulator | murmulator2 | olimex-pico-pc | waveshare_rp2350_pizero
HDMI=ON/OFF
VGA=ON/OFF
SOFTTV=ON/OFF
I2S=ON/OFF
HWAY=ON/OFF
```

Only one release video backend and one release audio backend should be selected for a particular firmware image. PWM is selected when I2S and HWAY are disabled.

The project currently uses Raspberry Pi Pico SDK 2.3.1 and an Arm GNU 14.2 toolchain configuration.

## Hardware projects

[MURMULATOR classical scheme](https://github.com/AlexEkb4ever/MURMULATOR_classical_scheme)

## Release history

See `CHANGELOG.md` for the concise version history and the release notes supplied with each release for a detailed description of changes.
