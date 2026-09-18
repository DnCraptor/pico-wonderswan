#pragma once
#include "boards/pico.h"

#define PICO_FLASH_SIZE_BYTES 16777216
#define ZERO 1
#define SDCARD_SPI_BUS spi0
#define SDCARD_PIN_SPI0_SCK 18
#define SDCARD_PIN_SPI0_MOSI 19
#define SDCARD_PIN_SPI0_MISO 20
#define SDCARD_PIN_SPI0_CS 21
#define PS2KBD_GPIO_FIRST 0
#define NES_GPIO_CLK 7
#define NES_GPIO_LAT 8
#define NES_GPIO_DATA 9
#define VGA_BASE_PIN 22
#define HDMI_BASE_PIN 22
#define TFT_CS_PIN 22
#define TFT_RST_PIN 24
#define TFT_LED_PIN 25
#define TFT_DC_PIN 26
#define TFT_DATA_PIN 28
#define TFT_CLK_PIN 29
#define AUDIO_PWM_PIN 11
#define AUDIO_DATA_PIN 11
#define AUDIO_CLOCK_PIN 12
#define PSRAM_PIN_CS 18
#define PSRAM_PIN_SCK 19
#define PSRAM_PIN_MOSI 20
#define PSRAM_PIN_MISO 21
