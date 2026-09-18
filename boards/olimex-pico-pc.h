#pragma once
#if PICO_RP2350
#include "boards/pico2.h"
#else
#include "boards/pico.h"
#endif

#if PICO_RP2350
#define PSRAM_CS1_GPIO_RP2350A 8
#define PSRAM_CS1_GPIO_RP2350B 47
#endif

#define PICO_FLASH_SIZE_BYTES 16777216
#define PICO_PC 1
#define SDCARD_SPI_BUS spi0
#define SDCARD_PIN_SPI0_CS 22
#define SDCARD_PIN_SPI0_SCK 6
#define SDCARD_PIN_SPI0_MOSI 7
#define SDCARD_PIN_SPI0_MISO 4
#define PS2KBD_GPIO_FIRST 0
#define NES_GPIO_CLK 8
#define NES_GPIO_LAT 9
#define NES_GPIO_DATA 20
#define VGA_BASE_PIN 12
#define HDMI_BASE_PIN 12
#define TFT_CS_PIN 12
#define TFT_RST_PIN 14
#define TFT_LED_PIN 15
#define TFT_DC_PIN 16
#define TFT_DATA_PIN 18
#define TFT_CLK_PIN 19
#define AUDIO_PWM_PIN 26
#define AUDIO_DATA_PIN 26
#define AUDIO_CLOCK_PIN 27
#define PSRAM_PIN_CS 18
#define PSRAM_PIN_SCK 19
#define PSRAM_PIN_MOSI 20
#define PSRAM_PIN_MISO 21
