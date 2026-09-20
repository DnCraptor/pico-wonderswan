#include <cstdio>
#include <cstring>
#include <hardware/flash.h>
#include <hardware/vreg.h>
#include <hardware/clocks.h>
#include <hardware/sync.h>
#include <hardware/structs/qmi.h>
#include <hardware/watchdog.h>
#include <pico/multicore.h>
#include <pico/flash.h>
#include <pico/stdlib.h>

#include <graphics.h>
#include "audio.h"

#include "nespad.h"
#include "ff.h"
#include "ps2kbd_mrmltr.h"
#include "psram_spi.h"
#include "sdcard.h"
#include "qspi_psram.h"

extern "C" {
#include "ws.h"
#include "ws_audio.h"
#include "io.h"
#include "gpu.h"
#include "memory.h"
#include "nec/necintrf.h"
}

#define HOME_DIR "\\WS"
extern char __flash_binary_end;
#define FLASH_TARGET_OFFSET (2u * 1024u * 1024u)
static uintptr_t rom = XIP_BASE + FLASH_TARGET_OFFSET;

static uint32_t detect_flash_size_bytes() {
    uint8_t tx[4] = { 0x9f, 0, 0, 0 };
    uint8_t rx[4] = { 0, 0, 0, 0 };
    multicore_lockout_start_blocking();
    flash_do_cmd(tx, rx, sizeof(tx));
    multicore_lockout_end_blocking();

    const uint8_t capacity_bits = rx[3];
    if (capacity_bits >= 20 && capacity_bits < 32)
        return 1u << capacity_bits;
    return PICO_FLASH_SIZE_BYTES;
}

struct flash_sector_write_t {
    uint32_t offset;
    const uint8_t *data;
};

static void program_flash_sector(void *param) {
    const flash_sector_write_t *write = (const flash_sector_write_t *)param;
    flash_range_erase(write->offset, FLASH_SECTOR_SIZE);
    flash_range_program(write->offset, write->data, FLASH_SECTOR_SIZE);
}

#define AUDIO_SAMPLE_RATE 24000
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60 + 1)


char __uninitialized_ram(filename[256]);
static uint32_t __uninitialized_ram(rom_size);

static FATFS fs;
bool reboot = false;
semaphore vga_start_semaphore;

alignas(4) uint8_t SCREEN1[144][224];
alignas(4) uint8_t SCREEN2[144][224];
alignas(4) uint8_t SCREEN3[144][224];
//alignas(4) int audio_buffer[AUDIO_BUFFER_LENGTH];
extern uint32_t	ws_shades[16];

struct input_bits_t {
    bool a: true;
    bool b: true;
    bool select: true;
    bool start: true;
    bool right: true;
    bool left: true;
    bool up: true;
    bool down: true;
};

static input_bits_t keyboard_bits = { false, false, false, false, false, false, false, false };
static input_bits_t gamepad1_bits = { false, false, false, false, false, false, false, false };
static input_bits_t gamepad2_bits = { false, false, false, false, false, false, false, false };

static bool swap_ab = false;

enum rotation_mode_t : uint8_t {
    ROTATION_AUTO = 0,
    ROTATION_LANDSCAPE,
    ROTATION_PORTRAIT,
    ROTATION_MANUAL,
};

static uint8_t rotation_mode = ROTATION_AUTO;
static bool manual_portrait = false;
static bool rotation_hotkey_override = false;
static bool keyboard_1 = false, keyboard_2 = false, keyboard_3 = false, keyboard_4 = false;
static bool keyboard_o = false, keyboard_p = false, keyboard_l = false, keyboard_semicolon = false;
static bool keyboard_9 = false, keyboard_0 = false;

static bool portrait_enabled() {
    bool portrait;
    switch (rotation_mode) {
        case ROTATION_LANDSCAPE: portrait = false; break;
        case ROTATION_PORTRAIT:  portrait = true; break;
        case ROTATION_MANUAL:    portrait = manual_portrait; break;
        case ROTATION_AUTO:
        default:                 portrait = ws_rotated() != 0; break;
    }
    return portrait ^ rotation_hotkey_override;
}

static void rotate_frame_90cw(const uint8_t *src, uint8_t *dst) {
    // Same clockwise transform used by Beetle WonderSwan's software rotation.
    for (unsigned x = 0; x < 224; ++x)
        for (unsigned y = 0; y < 144; ++y)
            dst[y + (223 - x) * 144] = src[x + y * 224];
}
extern	uint8	ws_key_start;
extern	uint8	ws_key_left;
extern	uint8	ws_key_right;
extern	uint8	ws_key_up;
extern	uint8	ws_key_down;
extern	uint8	ws_key_button_1;
extern	uint8	ws_key_button_2;
extern	uint8	ws_key_x1, ws_key_x2, ws_key_x3, ws_key_x4;
extern	uint8	ws_key_y1, ws_key_y2, ws_key_y3, ws_key_y4;

static void nespad_tick() {
    nespad_read();

    gamepad1_bits.a = (nespad_state & DPAD_A) != 0;
    gamepad1_bits.b = (nespad_state & DPAD_B) != 0;

    gamepad1_bits.select = keyboard_bits.select || (nespad_state & DPAD_SELECT) != 0;
    gamepad1_bits.start = keyboard_bits.start || (nespad_state & DPAD_START) != 0;
    gamepad1_bits.up = keyboard_bits.up || (nespad_state & DPAD_UP) != 0;
    gamepad1_bits.down = keyboard_bits.down || (nespad_state & DPAD_DOWN) != 0;
    gamepad1_bits.left = keyboard_bits.left || (nespad_state & DPAD_LEFT) != 0;
    gamepad1_bits.right = keyboard_bits.right || (nespad_state & DPAD_RIGHT) != 0;
}

static bool isInReport(hid_keyboard_report_t const* report, const unsigned char keycode) {
    for (unsigned char i: report->keycode) {
        if (i == keycode) {
            return true;
        }
    }
    return false;
}

static volatile bool altPressed = false;
static volatile bool ctrlPressed = false;
static volatile uint8_t fxPressedV = 0;

void process_kbd_report(hid_keyboard_report_t const* report, hid_keyboard_report_t const* prev_report) {
    /* printf("HID key report modifiers %2.2X report ", report->modifier);
    for (unsigned char i: report->keycode)
        printf("%2.2X", i);
    printf("\r\n");
     */
    keyboard_bits.start = isInReport(report, HID_KEY_ENTER) || isInReport(report, HID_KEY_KEYPAD_ENTER);
    keyboard_bits.select = isInReport(report, HID_KEY_BACKSPACE) || isInReport(report, HID_KEY_ESCAPE) || isInReport(report, HID_KEY_KEYPAD_ADD);

    // A/B keyboard bindings depend on presentation orientation and are
    // resolved in the emulation loop together with the native X/Y groups.
    keyboard_bits.b = isInReport(report, HID_KEY_Z);
    keyboard_bits.a = isInReport(report, HID_KEY_X);

    keyboard_o = isInReport(report, HID_KEY_O);
    keyboard_p = isInReport(report, HID_KEY_P);
    keyboard_l = isInReport(report, HID_KEY_L);
    keyboard_semicolon = isInReport(report, HID_KEY_SEMICOLON);
    keyboard_9 = isInReport(report, HID_KEY_9);
    keyboard_0 = isInReport(report, HID_KEY_0);
    keyboard_1 = isInReport(report, HID_KEY_1);
    keyboard_2 = isInReport(report, HID_KEY_2);
    keyboard_3 = isInReport(report, HID_KEY_3);
    keyboard_4 = isInReport(report, HID_KEY_4);

    bool b7 = isInReport(report, HID_KEY_KEYPAD_7);
    bool b9 = isInReport(report, HID_KEY_KEYPAD_9);
    bool b1 = isInReport(report, HID_KEY_KEYPAD_1);
    bool b3 = isInReport(report, HID_KEY_KEYPAD_3);

    keyboard_bits.up = b7 || b9 || isInReport(report, HID_KEY_ARROW_UP) || isInReport(report, HID_KEY_W) || isInReport(report, HID_KEY_KEYPAD_8);
    keyboard_bits.down = b1 || b3 || isInReport(report, HID_KEY_ARROW_DOWN) || isInReport(report, HID_KEY_S) || isInReport(report, HID_KEY_KEYPAD_2) || isInReport(report, HID_KEY_KEYPAD_5);
    keyboard_bits.left = b7 || b1 || isInReport(report, HID_KEY_ARROW_LEFT) || isInReport(report, HID_KEY_A) || isInReport(report, HID_KEY_KEYPAD_4);
    keyboard_bits.right = b9 || b3 || isInReport(report, HID_KEY_ARROW_RIGHT)  || isInReport(report, HID_KEY_D) || isInReport(report, HID_KEY_KEYPAD_6);

    altPressed = isInReport(report, HID_KEY_ALT_LEFT) || isInReport(report, HID_KEY_ALT_RIGHT);
    ctrlPressed = isInReport(report, HID_KEY_CONTROL_LEFT) || isInReport(report, HID_KEY_CONTROL_RIGHT);
    
    if (altPressed && ctrlPressed && isInReport(report, HID_KEY_DELETE)) {
        watchdog_enable(10, true);
        while(true) {
            tight_loop_contents();
        }
    }
    if (ctrlPressed || altPressed) {
        uint8_t fxPressed = 0;
        if (isInReport(report, HID_KEY_F1)) fxPressed = 1;
        else if (isInReport(report, HID_KEY_F2)) fxPressed = 2;
        else if (isInReport(report, HID_KEY_F3)) fxPressed = 3;
        else if (isInReport(report, HID_KEY_F4)) fxPressed = 4;
        else if (isInReport(report, HID_KEY_F5)) fxPressed = 5;
        else if (isInReport(report, HID_KEY_F6)) fxPressed = 6;
        else if (isInReport(report, HID_KEY_F7)) fxPressed = 7;
        else if (isInReport(report, HID_KEY_F8)) fxPressed = 8;
        fxPressedV = fxPressed;
    }
    //-------------------------------------------------------------------------
}

Ps2Kbd_Mrmltr ps2kbd(
        pio1,
        PS2KBD_GPIO_FIRST,
        process_kbd_report);



i2s_config_t i2s_config;

typedef struct __attribute__((__packed__)) {
    bool is_directory;
    bool is_executable;
    size_t size;
    char filename[79];
} file_item_t;

constexpr int max_files = 320;
file_item_t *fileItems = (file_item_t *) (&SCREEN1[0][0] + TEXTMODE_COLS * TEXTMODE_ROWS * 2);

int compareFileItems(const void *a, const void *b) {
    const auto *itemA = (file_item_t *) a;
    const auto *itemB = (file_item_t *) b;
    // Directories come first
    if (itemA->is_directory && !itemB->is_directory)
        return -1;
    if (!itemA->is_directory && itemB->is_directory)
        return 1;
    // Sort files alphabetically
    return strcmp(itemA->filename, itemB->filename);
}

bool isExecutable(const char pathname[255], const char *extensions) {
    char *pathCopy = strdup(pathname);
    const char *token = strrchr(pathCopy, '.');

    if (token == nullptr) {
        return false;
    }

    token++;

    while (token != NULL) {
        if (strstr(extensions, token) != NULL) {
            free(pathCopy);
            return true;
        }
        token = strtok(NULL, ",");
    }
    free(pathCopy);
    return false;
}

static bool temporary_flash_reclock(uint32_t target_khz);
static void menu(bool game_loaded);

bool filebrowser_loadfile(const char pathname[256]) {
    UINT bytes_read = 0;
    FIL file;

    constexpr int window_y = (TEXTMODE_ROWS - 5) / 2;
    constexpr int window_x = (TEXTMODE_COLS - 43) / 2;

    draw_window("Loading ROM", window_x, window_y, 43, 5);

    FILINFO fileinfo;
    if (FR_OK != f_stat(pathname, &fileinfo) || fileinfo.fsize == 0) {
        draw_text("ERROR: ROM not found or empty!", window_x + 1, window_y + 2, 13, 1);
        sleep_ms(5000);
        return false;
    }

    const uint32_t load_size = fileinfo.fsize;
    if (((16384 - 64) << 10) < load_size) {
        draw_text("ERROR: ROM too large! Canceled!!", window_x + 1, window_y + 2, 13, 1);
        sleep_ms(5000);
        return false;
    }

    draw_text("Loading...", window_x + 1, window_y + 2, 10, 1);

    bool load_ok = false;
    if (wonderswan_qspi_psram_available()) {
        const size_t capacity = wonderswan_qspi_rom_capacity();
        if (load_size > capacity) {
            draw_text("ERROR: ROM too large for PSRAM!", window_x + 1, window_y + 2, 13, 1);
            sleep_ms(5000);
            return false;
        }

        if (FR_OK == f_open(&file, pathname, FA_READ)) {
            uint8_t *dst = (uint8_t *)WONDERSWAN_QSPI_PSRAM_BASE;
            uint32_t total_read = 0;
            FRESULT read_result = FR_OK;
            do {
                read_result = f_read(&file, dst, 4096, &bytes_read);
                dst += bytes_read;
                total_read += bytes_read;
            } while (read_result == FR_OK && bytes_read != 0);
            load_ok = read_result == FR_OK && total_read == load_size;
            f_close(&file);
        }
    } else {
        const uint32_t firmware_end = (uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE);
        if (firmware_end > FLASH_TARGET_OFFSET) {
            draw_text("ERROR: Firmware overlaps ROM flash area!", window_x + 1, window_y + 2, 13, 1);
            sleep_ms(5000);
            return false;
        }

        const uint32_t flash_size = detect_flash_size_bytes();
        if (FLASH_TARGET_OFFSET >= flash_size || load_size > flash_size - FLASH_TARGET_OFFSET) {
            draw_text("ERROR: ROM too large for flash!", window_x + 1, window_y + 2, 13, 1);
            sleep_ms(5000);
            return false;
        }

        const uint32_t original_sys_khz = clock_get_hz(clk_sys) / 1000u;
        const bool need_clock_restore = original_sys_khz > 252000u;
        if (need_clock_restore && !temporary_flash_reclock(252000u)) {
            draw_text("ERROR: Cannot lower clock for flash!", window_x + 1, window_y + 2, 13, 1);
            sleep_ms(5000);
            return false;
        }

        auto flash_target_offset = FLASH_TARGET_OFFSET;
        uint32_t total_read = 0;
        FRESULT read_result = FR_OK;

        if (FR_OK == f_open(&file, pathname, FA_READ)) {
            static uint8_t buffer[FLASH_SECTOR_SIZE] __aligned(4);
            do {
                memset(buffer, 0xff, sizeof(buffer));
                read_result = f_read(&file, buffer, sizeof(buffer), &bytes_read);
                if (read_result != FR_OK || bytes_read == 0)
                    break;

                const uint8_t *flash_data =
                    (const uint8_t *)(XIP_BASE + flash_target_offset);
                if (memcmp(flash_data, buffer, sizeof(buffer)) != 0) {
                    flash_sector_write_t write = { flash_target_offset, buffer };
                    if (flash_safe_execute(program_flash_sector, &write, UINT32_MAX) != PICO_OK ||
                        memcmp(flash_data, buffer, sizeof(buffer)) != 0) {
                        read_result = FR_DISK_ERR;
                        break;
                    }
                }

                total_read += bytes_read;
                gpio_put(PICO_DEFAULT_LED_PIN, flash_target_offset >> 13 & 1);
                flash_target_offset += FLASH_SECTOR_SIZE;
            } while (bytes_read != 0);
            f_close(&file);
        }

        if (need_clock_restore && !temporary_flash_reclock(original_sys_khz))
            read_result = FR_DISK_ERR;

        gpio_put(PICO_DEFAULT_LED_PIN, true);
        load_ok = read_result == FR_OK && total_read == load_size;
    }

    if (!load_ok) {
        draw_text("ERROR: ROM load failed!", window_x + 1, window_y + 2, 13, 1);
        sleep_ms(5000);
        return false;
    }

    rom_size = load_size;
    strcpy(filename, fileinfo.fname);
    return true;
}

void filebrowser(const char pathname[256], const char executables[11]) {
    bool debounce = true;
    char basepath[256];
    char tmp[TEXTMODE_COLS + 1];
    strcpy(basepath, pathname);
    constexpr int per_page = TEXTMODE_ROWS - 3;

    DIR dir;
    FILINFO fileInfo;

    if (FR_OK != f_mount(&fs, "SD", 1)) {
        draw_text("SD Card not inserted or SD Card error!", 0, 0, 12, 0);
        while (true);
    }

    while (true) {
        memset(fileItems, 0, sizeof(file_item_t) * max_files);
        int total_files = 0;

        snprintf(tmp, TEXTMODE_COLS, "SD:\\%s", basepath);
        draw_window(tmp, 0, 0, TEXTMODE_COLS, TEXTMODE_ROWS - 1);
        memset(tmp, ' ', TEXTMODE_COLS);


        draw_text(tmp, 0, 29, 0, 0);
        auto off = 0;
        draw_text("START", off, 29, 7, 0);
        off += 5;
        draw_text(" Run at cursor ", off, 29, 0, 3);
        off += 16;
        draw_text("SELECT", off, 29, 7, 0);
        off += 6;
        draw_text(" Run previous  ", off, 29, 0, 3);
#ifndef TFT
        off += 16;
        draw_text("ARROWS", off, 29, 7, 0);
        off += 6;
        draw_text(" Navigation    ", off, 29, 0, 3);
        off += 16;
        draw_text("A/F10", off, 29, 7, 0);
        off += 5;
        draw_text(" USB DRV ", off, 29, 0, 3);
#endif

        if (FR_OK != f_opendir(&dir, basepath)) {
            draw_text("Failed to open directory", 1, 1, 4, 0);
            while (true);
        }

        if (strlen(basepath) > 0) {
            strcpy(fileItems[total_files].filename, "..\0");
            fileItems[total_files].is_directory = true;
            fileItems[total_files].size = 0;
            total_files++;
        }

        while (f_readdir(&dir, &fileInfo) == FR_OK &&
               fileInfo.fname[0] != '\0' &&
               total_files < max_files
                ) {
            // Set the file item properties
            fileItems[total_files].is_directory = fileInfo.fattrib & AM_DIR;
            fileItems[total_files].size = fileInfo.fsize;
            fileItems[total_files].is_executable = isExecutable(fileInfo.fname, executables);
            strncpy(fileItems[total_files].filename, fileInfo.fname, 78);
            total_files++;
        }
        f_closedir(&dir);

        qsort(fileItems, total_files, sizeof(file_item_t), compareFileItems);

        if (total_files > max_files) {
            draw_text(" Too many files!! ", TEXTMODE_COLS - 17, 0, 12, 3);
        }

        int offset = 0;
        int current_item = 0;

        while (true) {
            sleep_ms(100);

            if (!debounce) {
                debounce = !(nespad_state & DPAD_START || keyboard_bits.start);
            }

            // SELECT opens the emulator menu even before a cartridge is
            // loaded.  Returning from that menu must come back to the ROM
            // browser, not fall through into ws_init()/emulation.
            if (nespad_state & DPAD_SELECT || keyboard_bits.select) {
                menu(false);
                debounce = false;
                break;
            }

            if (nespad_state & DPAD_DOWN || keyboard_bits.down) {
                if (offset + (current_item + 1) < total_files) {
                    if (current_item + 1 < per_page) {
                        current_item++;
                    } else {
                        offset++;
                    }
                }
            }

            if (nespad_state & DPAD_UP || keyboard_bits.up) {
                if (current_item > 0) {
                    current_item--;
                } else if (offset > 0) {
                    offset--;
                }
            }

            if (nespad_state & DPAD_RIGHT || keyboard_bits.right) {
                offset += per_page;
                if (offset + (current_item + 1) > total_files) {
                    offset = total_files - (current_item + 1);
                }
            }

            if (nespad_state & DPAD_LEFT || keyboard_bits.left) {
                if (offset > per_page) {
                    offset -= per_page;
                } else {
                    offset = 0;
                    current_item = 0;
                }
            }

            if (debounce && (nespad_state & DPAD_START || keyboard_bits.start)) {
                auto file_at_cursor = fileItems[offset + current_item];

                if (file_at_cursor.is_directory) {
                    if (strcmp(file_at_cursor.filename, "..") == 0) {
                        const char *lastBackslash = strrchr(basepath, '\\');
                        if (lastBackslash != nullptr) {
                            const size_t length = lastBackslash - basepath;
                            basepath[length] = '\0';
                        }
                    } else {
                        sprintf(basepath, "%s\\%s", basepath, file_at_cursor.filename);
                    }
                    debounce = false;
                    break;
                }

                if (file_at_cursor.is_executable) {
                    sprintf(tmp, "%s\\%s", basepath, file_at_cursor.filename);

                    if (filebrowser_loadfile(tmp)) {
                        return;
                    }

                    // Keep the browser active after a failed load.  In
                    // particular, do not let main() start the previous or a
                    // partially loaded cartridge after a size/read error.
                    debounce = false;
                    continue;
                }
            }

            for (int i = 0; i < per_page; i++) {
                uint8_t color = 11;
                uint8_t bg_color = 1;

                if (offset + i < max_files) {
                    const auto item = fileItems[offset + i];


                    if (i == current_item) {
                        color = 0;
                        bg_color = 3;
                        memset(tmp, 0xCD, TEXTMODE_COLS - 2);
                        tmp[TEXTMODE_COLS - 2] = '\0';
                        draw_text(tmp, 1, per_page + 1, 11, 1);
                        snprintf(tmp, TEXTMODE_COLS - 2, " Size: %iKb, File %lu of %i ", item.size / 1024,
                                 offset + i + 1,
                                 total_files);
                        draw_text(tmp, 2, per_page + 1, 14, 3);
                    }

                    const auto len = strlen(item.filename);
                    color = item.is_directory ? 15 : color;
                    color = item.is_executable ? 10 : color;
                    //color = strstr((char *)rom_filename, item.filename) != nullptr ? 13 : color;

                    memset(tmp, ' ', TEXTMODE_COLS - 2);
                    tmp[TEXTMODE_COLS - 2] = '\0';
                    memcpy(&tmp, item.filename, len < TEXTMODE_COLS - 2 ? len : TEXTMODE_COLS - 2);
                } else {
                    memset(tmp, ' ', TEXTMODE_COLS - 2);
                }
                draw_text(tmp, 1, i + 1, color, bg_color);
            }
        }
    }
}

enum menu_type_e {
    NONE,
    INT,
    TEXT,
    ARRAY,

    SAVE,
    LOAD,
    ROM_SELECT,
    SHOW_PALETTES,
    RETURN,
};

typedef bool (*menu_callback_t)();

typedef struct __attribute__((__packed__)) {
    const char *text;
    menu_type_e type;
    const void *value;
    menu_callback_t callback;
    uint8_t max_value;
    char value_list[15][10];
} MenuItem;

int save_slot = 0;
uint16_t frequencies[] = { 378, 396, 404, 408, 412, 416, 420, 424, 432, 444, 460, 504, 524, 528 };
uint8_t voltage_index = 0; // Auto, 1.50V, 1.60V, 1.65V, 1.70V
static volatile bool runtime_drivers_ready = false;
#if PICO_RP2040
uint8_t frequency_index = 0;
#else
uint8_t frequency_index = 0;
#endif

static enum vreg_voltage selected_voltage(uint16_t mhz) {
    switch (voltage_index) {
        case 1: return VREG_VOLTAGE_1_50;
        case 2: return VREG_VOLTAGE_1_60;
        case 3: return VREG_VOLTAGE_1_65;
        case 4: return VREG_VOLTAGE_1_70;
        default:
            if (mhz > 504) return VREG_VOLTAGE_1_65;
            if (mhz >= 378) return VREG_VOLTAGE_1_60;
            return VREG_VOLTAGE_1_50;
    }
}

#if PICO_RP2350
static void __no_inline_not_in_flash_func(set_flash_timing_for_clock)(uint32_t sys_hz) {
    const uint32_t max_flash_hz = 133000000u;
    uint32_t divisor = (sys_hz + max_flash_hz - (max_flash_hz >> 4) - 1) / max_flash_hz;
    if (divisor == 1 && sys_hz >= 166000000u) divisor = 2;
    uint32_t rxdelay = divisor;
    if (sys_hz / divisor > 100000000u && sys_hz >= 166000000u) ++rxdelay;
    qmi_hw->m[0].timing = 0x60007000u |
        (rxdelay << QMI_M0_TIMING_RXDELAY_LSB) |
        (divisor << QMI_M0_TIMING_CLKDIV_LSB);
}
#endif

static bool __no_inline_not_in_flash_func(set_target_sys_clock)(uint32_t target_khz) {
    if (set_sys_clock_khz(target_khz, false)) return true;
#if PICO_RP2350
    /* 526 MHz is not an integer-PLL result with the normal 12 MHz reference.
       Run PLL_SYS at 528 MHz (1584/3) and use RP2350's fractional clk_sys divider. */
    if (target_khz == 526000u) {
        const uint32_t pll_hz = 528000000u;
        set_sys_clock_pll(1584000000u, 3, 1);
        return clock_configure(clk_sys,
            CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
            CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
            pll_hz, target_khz * 1000u);
    }
#endif
    return false;
}

static void reclock_drivers(void) {
    i2s_reclock(&i2s_config);
    graphics_reclock();
    ps2kbd.reclock();
    nespad_reclock(clock_get_hz(clk_sys) / 1000);
    psram_reclock();
    sdcard_reclock();
}

static bool temporary_flash_reclock(uint32_t target_khz) {
    /* Flash programming uses a temporary CPU clock only.  Do not touch QMI,
       video/audio/SD dividers, voltage, or any other runtime driver here.
       Their settings remain valid again as soon as clk_sys is restored. */
    const uint32_t current_khz = clock_get_hz(clk_sys) / 1000u;
    if (current_khz == target_khz) return true;

    const uint32_t irq_state = save_and_disable_interrupts();
    if (runtime_drivers_ready) multicore_lockout_start_blocking();
    const bool res = set_target_sys_clock(target_khz);
    if (runtime_drivers_ready) multicore_lockout_end_blocking();
    restore_interrupts(irq_state);
    return res;
}

bool overclock() {
    const uint32_t target_khz = (uint32_t)frequencies[frequency_index] * 1000u;
#if PICO_RP2040
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(10);
    const bool res = set_sys_clock_khz(target_khz, true);
    if (runtime_drivers_ready) reclock_drivers();
    graphics_set_mode(TEXTMODE_DEFAULT);
    return res;
#else
    /*
     * Keep the proven boot clock sequence intact. At this point core 1 and the
     * video/audio/PIO drivers do not exist yet, so runtime reclocking is both
     * unnecessary and unsafe. In particular, these QMI timings are the values
     * used by the working pre-runtime-reclock implementation.
     */
    if (!runtime_drivers_ready) {
        volatile uint32_t *qmi_m0_timing = (uint32_t *)0x400d000c;
        vreg_disable_voltage_limit();
        vreg_set_voltage(VREG_VOLTAGE_1_60);
        sleep_ms(33);
        *qmi_m0_timing = 0x60007204;
        const bool res = set_sys_clock_khz(target_khz, false);
        *qmi_m0_timing = 0x60007303;
        graphics_set_mode(TEXTMODE_DEFAULT);
        return res;
    }

    const uint32_t current_hz = clock_get_hz(clk_sys);
    const uint32_t target_hz = target_khz * 1000u;
    const bool raising = target_hz > current_hz;
    const enum vreg_voltage target_voltage = selected_voltage(frequencies[frequency_index]);

    if (target_hz == current_hz) {
        vreg_disable_voltage_limit();
        vreg_set_voltage(target_voltage);
        return true;
    }

    if (raising) {
        vreg_disable_voltage_limit();
        vreg_set_voltage(target_voltage);
        sleep_ms(50);
    }

    const uint32_t irq_state = save_and_disable_interrupts();
    if (runtime_drivers_ready) multicore_lockout_start_blocking();

    if (raising) {
        set_flash_timing_for_clock(target_hz);
        wonderswan_qspi_psram_reclock(target_hz);
    }

    const bool res = set_target_sys_clock(target_khz);

    if (!raising && res) {
        set_flash_timing_for_clock(target_hz);
        wonderswan_qspi_psram_reclock(target_hz);
    }

    if (runtime_drivers_ready) {
        reclock_drivers();
        multicore_lockout_end_blocking();
    }
    restore_interrupts(irq_state);

    if (!raising && res) {
        sleep_ms(10);
        vreg_disable_voltage_limit();
        vreg_set_voltage(target_voltage);
    }
    graphics_set_mode(TEXTMODE_DEFAULT);
    return res;
#endif
}

#define WS_STATE_MAGIC 0x31535357u /* WSS1 */
#define WS_STATE_VERSION 1u

typedef struct {
    nec_snapshot_t cpu;
    ws_io_snapshot_t io;
    ws_gpu_snapshot_t gpu;
    ws_audio_snapshot_t audio;
} ws_state_core_t;

typedef struct __attribute__((packed)) {
    uint32_t magic, version, rom_size;
    uint16_t rom_crc, reserved;
    uint32_t internal_ram_size, sram_size, eeprom_size;
    uint32_t cpu_size, io_size, gpu_size, audio_size;
    uint32_t ws_cycles, ws_skip, ws_cycles_by_line;
} ws_state_header_t;

static bool state_write(FIL *fd, const void *data, UINT size) {
    UINT done = 0;
    return f_write(fd, data, size, &done) == FR_OK && done == size;
}
static bool state_read(FIL *fd, void *data, UINT size) {
    UINT done = 0;
    return f_read(fd, data, size, &done) == FR_OK && done == size;
}
static bool state_write_cart(FIL *fd, uint32_t base, uint32_t size) {
    uint8_t buf[256];
    while (size) { const uint32_t n = size > sizeof(buf) ? sizeof(buf) : size; readpsram(buf, base, n); if (!state_write(fd, buf, n)) return false; base += n; size -= n; }
    return true;
}
static bool state_read_cart(FIL *fd, uint32_t base, uint32_t size) {
    uint8_t buf[256];
    while (size) { const uint32_t n = size > sizeof(buf) ? sizeof(buf) : size; if (!state_read(fd, buf, n)) return false; writepsram(base, buf, n); base += n; size -= n; }
    return true;
}

bool save() {
    if (!rom_size || save_slot < 1 || save_slot > 8) return false;
    char pathname[255];
    snprintf(pathname, sizeof(pathname), "%s\\%s_%d.save", HOME_DIR, filename, save_slot);
    ws_state_core_t *state = (ws_state_core_t *)malloc(sizeof(*state));
    if (!state) return false;
    nec_snapshot_get(&state->cpu); ws_io_snapshot_get(&state->io); ws_gpu_snapshot_get(&state->gpu); ws_audio_snapshot_get(&state->audio);
    ws_state_header_t h = { WS_STATE_MAGIC, WS_STATE_VERSION, (uint32_t)rom_size, memory_getRomCrc(), 0,
        sizeof(internalRam), ws_memory_get_sram_size(), ws_memory_get_eeprom_size(), sizeof(state->cpu), sizeof(state->io), sizeof(state->gpu), sizeof(state->audio), ws_cycles, ws_skip, ws_cyclesByLine };
    if (f_mount(&fs, "", 1) != FR_OK) { free(state); return false; }
    FIL fd; if (f_open(&fd, pathname, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) { free(state); return false; }
    bool ok = state_write(&fd, &h, sizeof(h)) && state_write(&fd, &state->cpu, sizeof(state->cpu)) && state_write(&fd, &state->io, sizeof(state->io)) && state_write(&fd, &state->gpu, sizeof(state->gpu)) && state_write(&fd, &state->audio, sizeof(state->audio)) && state_write(&fd, internalRam, sizeof(internalRam)) && state_write_cart(&fd, 1u << 20, h.sram_size) && state_write_cart(&fd, 0, h.eeprom_size);
    if (ok) ok = f_sync(&fd) == FR_OK;
    f_close(&fd);
    free(state);
    if (!ok) f_unlink(pathname);
    return ok;
}

bool load() {
    if (!rom_size || save_slot < 1 || save_slot > 8) return false;
    char pathname[255];
    snprintf(pathname, sizeof(pathname), "%s\\%s_%d.save", HOME_DIR, filename, save_slot);
    if (f_mount(&fs, "", 1) != FR_OK) return false;
    FIL fd; if (f_open(&fd, pathname, FA_READ) != FR_OK) return false;
    ws_state_header_t h; bool ok = state_read(&fd, &h, sizeof(h));
    ok = ok && h.magic == WS_STATE_MAGIC && h.version == WS_STATE_VERSION && h.rom_size == rom_size && h.rom_crc == memory_getRomCrc() && h.internal_ram_size == sizeof(internalRam) && h.sram_size == ws_memory_get_sram_size() && h.eeprom_size == ws_memory_get_eeprom_size() && h.cpu_size == sizeof(nec_snapshot_t) && h.io_size == sizeof(ws_io_snapshot_t) && h.gpu_size == sizeof(ws_gpu_snapshot_t) && h.audio_size == sizeof(ws_audio_snapshot_t);
    ws_state_core_t *state = ok ? (ws_state_core_t *)malloc(sizeof(*state)) : nullptr;
    if (ok && !state) ok = false;
    if (ok) ok = state_read(&fd, &state->cpu, sizeof(state->cpu)) && state_read(&fd, &state->io, sizeof(state->io)) && state_read(&fd, &state->gpu, sizeof(state->gpu)) && state_read(&fd, &state->audio, sizeof(state->audio)) && state_read(&fd, internalRam, sizeof(internalRam)) && state_read_cart(&fd, 1u << 20, h.sram_size) && state_read_cart(&fd, 0, h.eeprom_size);
    f_close(&fd);
    if (!ok) { free(state); return false; }
    ws_cycles = h.ws_cycles; ws_skip = h.ws_skip; ws_cyclesByLine = h.ws_cycles_by_line;
    nec_snapshot_set(&state->cpu); ws_io_snapshot_set(&state->io); ws_gpu_snapshot_set(&state->gpu); ws_audio_snapshot_set(&state->audio);
    free(state);
    return true;
}
#if SOFTTV
typedef struct tv_out_mode_t {
    // double color_freq;
    float color_index;
    COLOR_FREQ_t c_freq;
    enum graphics_mode_t mode_bpp;
    g_out_TV_t tv_system;
    NUM_TV_LINES_t N_lines;
    bool cb_sync_PI_shift_lines;
    bool cb_sync_PI_shift_half_frame;
} tv_out_mode_t;
extern tv_out_mode_t tv_out_mode;

bool color_mode=true;
bool toggle_color() {
    color_mode=!color_mode;
    if(color_mode) {
        tv_out_mode.color_index= 1.0f;
    } else {
        tv_out_mode.color_index= 0.0f;
    }

    return true;
}
#endif
int palette_index = 0;
bool show_fps = false;
uint8_t audio_volume = 4;
uint8_t audio_rate_shift = 0;

static bool apply_audio_volume() {
    ws_audio_set_enabled(audio_volume != 0);
    if (audio_volume != 0) {
        static const uint8_t attenuation[] = { 0, 3, 2, 1, 0 };
        i2s_volume(&i2s_config, attenuation[audio_volume]);
    }
    return false;
}

static bool apply_audio_rate() {
    ws_audio_set_rate_shift(audio_rate_shift);
    return false;
}

static bool mono_ws_rom_loaded(bool game_loaded) {
    if (!game_loaded) return false;
    const char *dot = strrchr(filename, '.');
    return dot && (dot[1] == 'w' || dot[1] == 'W') &&
           (dot[2] == 's' || dot[2] == 'S') && dot[3] == '\0';
}

static void palette_preview_hex_digit(uint8_t *buffer, int x, int y, unsigned digit, uint8_t color) {
    static const uint8_t digits[16][5] = {
        { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 },
        { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 },
        { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 },
        { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 },
        { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
        { 7, 5, 7, 5, 5 }, { 6, 5, 6, 5, 6 },
        { 7, 4, 4, 4, 7 }, { 6, 5, 5, 5, 6 },
        { 7, 4, 7, 4, 7 }, { 7, 4, 7, 4, 4 }
    };
    if (digit > 15) return;
    for (int row = 0; row < 5; ++row)
        for (int col = 0; col < 3; ++col)
            if (digits[digit][row] & (4u >> col))
                buffer[(y + row) * 224 + x + col] = color;
}

static void palette_preview_rgb(uint8_t *buffer, int x, int y, uint32_t rgb, uint8_t color) {
    for (int digit = 0; digit < 6; ++digit) {
        const unsigned shift = (unsigned)(5 - digit) * 4u;
        palette_preview_hex_digit(buffer, x + digit * 4, y, (rgb >> shift) & 0x0fu, color);
    }
}

static uint8_t palette_preview_contrast(unsigned shade) {
    const uint32_t bg = ws_shades[shade] & 0x00ffffffu;
    const uint32_t c0 = ws_shades[0] & 0x00ffffffu;
    const uint32_t c15 = ws_shades[15] & 0x00ffffffu;
    const int br = (bg >> 16) & 0xff, bgc = (bg >> 8) & 0xff, bb = bg & 0xff;
    const int r0 = (c0 >> 16) & 0xff, g0 = (c0 >> 8) & 0xff, b0 = c0 & 0xff;
    const int r15 = (c15 >> 16) & 0xff, g15 = (c15 >> 8) & 0xff, b15 = c15 & 0xff;
    const unsigned d0 = (unsigned)((br-r0)*(br-r0) + (bgc-g0)*(bgc-g0) + (bb-b0)*(bb-b0));
    const unsigned d15 = (unsigned)((br-r15)*(br-r15) + (bgc-g15)*(bgc-g15) + (bb-b15)*(bb-b15));
    return d0 >= d15 ? 0 : 15;
}

static void show_current_palettes(void) {
    uint8_t *buffer = (uint8_t *)SCREEN3;

    /* Mono WS framebuffer colours are the sixteen current shade indices. */
    for (unsigned shade = 0; shade < 16; ++shade) {
        const int x0 = (int)(shade & 3u) * 56;
        const int y0 = (int)(shade >> 2) * 36;
        for (int y = y0; y < y0 + 36; ++y)
            memset(buffer + y * 224 + x0, (int)shade, 56);
    }

    graphics_set_buffer(buffer, 224, 144);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    ws_gpu_refresh_palette();

    /* Each swatch contains its shade index and the actual RRGGBB value. */
    for (unsigned shade = 0; shade < 16; ++shade) {
        const int x0 = (int)(shade & 3u) * 56;
        const int y0 = (int)(shade >> 2) * 36;
        const uint8_t text = palette_preview_contrast(shade);
        palette_preview_hex_digit(buffer, x0 + 13, y0 + 15, shade, text);
        palette_preview_rgb(buffer, x0 + 21, y0 + 15, ws_shades[shade] & 0x00ffffffu, text);
    }

    /* Display-only page. START, B or SELECT returns to the normal menu. */
    while (gamepad1_bits.start || gamepad1_bits.b || gamepad1_bits.select)
        sleep_ms(20);
    while (!(gamepad1_bits.start || gamepad1_bits.b || gamepad1_bits.select))
        sleep_ms(20);
    while (gamepad1_bits.start || gamepad1_bits.b || gamepad1_bits.select)
        sleep_ms(20);

    graphics_set_mode(TEXTMODE_DEFAULT);
}

const MenuItem menu_items[] = {
        { "Swap AB <> BA: %s", ARRAY, &swap_ab, nullptr, 1, { "NO ", "YES" }},
        { "Screen rotation: %s", ARRAY, &rotation_mode, nullptr, 3, { "Auto", "Landscape", "Portrait ", "Manual   " }},
        { "FPS overlay: %s", ARRAY, &show_fps, nullptr, 1, { "OFF", "ON " }},
        { "Volume: %s", ARRAY, &audio_volume, &apply_audio_volume, 4, { "Mute", "12% ", "25% ", "50% ", "100%" }},
        { "Emulate Sound: %s", ARRAY, &audio_rate_shift, &apply_audio_rate, 3, { "24 kHz", "12 kHz", "6 kHz ", "3 kHz " }},
        {},
        //{ "Player 1: %s",        ARRAY, &player_1_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
        //{ "Player 2: %s",        ARRAY, &player_2_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
//        {},
//        { "Save state: %i", INT, &save_slot, &save, 5 },
//        { "Load state: %i", INT, &save_slot, &load, 5 },
        // Palette selection is hidden until WonderSwan colour handling is
        // accurate enough for UAT. Keep the implementation for later work.
//        { "Palette: %s", ARRAY, &palette_index, nullptr, 2,
//          {
//                  "default",
//                  "amber  ",
//                  "green  "
//          }
//        },
        {},
#if SOFTTV
        { "TV system %s", ARRAY, &tv_out_mode.tv_system, nullptr, 1, { "PAL ", "NTSC" } },
        { "TV Lines %s", ARRAY, &tv_out_mode.N_lines, nullptr, 3, { "624", "625", "524", "525" } },
        { "Freq %s", ARRAY, &tv_out_mode.c_freq, nullptr, 1, { "3.579545", "4.433619" } },
        { "Colors: %s", ARRAY, &color_mode, &toggle_color, 1, { "NO ", "YES" } },
        { "Shift lines %s", ARRAY, &tv_out_mode.cb_sync_PI_shift_lines, nullptr, 1, { "NO ", "YES" } },
        { "Shift half frame %s", ARRAY, &tv_out_mode.cb_sync_PI_shift_half_frame, nullptr, 1, { "NO ", "YES" } },
#endif
        {
                "Overclocking: %s MHz", ARRAY, &frequency_index, &overclock, count_of(frequencies) - 1,
                { "378", "396", "404", "408", "412", "416", "420", "424", "432", "444", "460", "504", "524", "528" }
        },
#if PICO_RP2350
        { "Voltage: %s", ARRAY, &voltage_index, &overclock, 4, { "Auto", "1.50V", "1.60V", "1.65V", "1.70V" } },
#endif
        { "Press START / Enter to apply", NONE },
        { "Show current palettes", SHOW_PALETTES },
        { "Reset to ROM select", ROM_SELECT },
        { "Return to game", RETURN }
};
#define MENU_ITEMS_NUMBER (sizeof(menu_items) / sizeof (MenuItem))

static bool menu_item_selectable(uint index, bool game_loaded) {
    const menu_type_e type = menu_items[index].type;
    return type != NONE &&
           (type != RETURN || game_loaded) &&
           (type != SHOW_PALETTES || mono_ws_rom_loaded(game_loaded));
}

static void menu(bool game_loaded) {
    bool exit = false;
    memset((uint8_t*)SCREEN1, 0, 144 * 224);
    memset((uint8_t*)SCREEN2, 0, 144 * 224);
    memset((uint8_t*)SCREEN3, 0, 144 * 224);

    graphics_set_mode(TEXTMODE_DEFAULT);
    char footer[TEXTMODE_COLS];
    snprintf(footer, TEXTMODE_COLS, ":: %s ::", PICO_PROGRAM_NAME);
    draw_text(footer, TEXTMODE_COLS / 2 - strlen(footer) / 2, 0, 11, 1);
    snprintf(footer, TEXTMODE_COLS, ":: %s build %s %s ::", PICO_PROGRAM_VERSION_STRING, __DATE__,
             __TIME__);
    draw_text(footer, TEXTMODE_COLS / 2 - strlen(footer) / 2, TEXTMODE_ROWS - 1, 11, 1);
    uint current_item = 0;

    while (!exit) {
        for (int i = 0; i < MENU_ITEMS_NUMBER; i++) {
            uint8_t y = i + (TEXTMODE_ROWS - MENU_ITEMS_NUMBER >> 1);
            uint8_t x = TEXTMODE_COLS / 2 - 10;
            uint8_t color = 0xFF;
            uint8_t bg_color = 0x00;
            if (current_item == i) {
                color = 0x01;
                bg_color = 0xFF;
            }
            const MenuItem *item = &menu_items[i];
            if (i == current_item) {
                switch (item->type) {
                    case INT:
                    case ARRAY:
                        if (item->max_value != 0) {
                            auto *value = (uint8_t *) item->value;
                            bool changed = false;
                            if (gamepad1_bits.right && *value < item->max_value) {
                                (*value)++;
                                changed = true;
                            }
                            if (gamepad1_bits.left && *value > 0) {
                                (*value)--;
                                changed = true;
                            }
                            if (changed && item->value == &audio_volume)
                                apply_audio_volume();
                            else if (changed && item->value == &audio_rate_shift)
                                apply_audio_rate();
                        }
                        break;
                    case RETURN:
                        if (game_loaded && gamepad1_bits.start)
                            exit = true;
                        break;

                    case SHOW_PALETTES:
                        if (gamepad1_bits.start && mono_ws_rom_loaded(game_loaded))
                            show_current_palettes();
                        break;

                    case ROM_SELECT:
                        if (gamepad1_bits.start) {
                            if (game_loaded)
                                reboot = true;
                            return;
                        }
                        break;
                    default:
                        break;
                }

                if (nullptr != item->callback && gamepad1_bits.start) {
                    exit = item->callback();
                }
            }
            static char result[TEXTMODE_COLS];
            switch (item->type) {
                case INT:
                    snprintf(result, TEXTMODE_COLS, item->text, *(uint8_t *) item->value);
                    break;
                case ARRAY:
                    snprintf(result, TEXTMODE_COLS, item->text, item->value_list[*(uint8_t *) item->value]);
                    break;
                case TEXT:
                    snprintf(result, TEXTMODE_COLS, item->text, item->value);
                    break;
                case NONE:
                    color = 6;
                default:
                    snprintf(result, TEXTMODE_COLS, "%s", item->text);
            }
            if ((!game_loaded && item->type == RETURN) ||
                (item->type == SHOW_PALETTES && !mono_ws_rom_loaded(game_loaded))) {
                color = 6;
                bg_color = 0;
            }
            draw_text(result, x, y, color, bg_color);
        }

        if (gamepad1_bits.down) {
            do {
                current_item = (current_item + 1) % MENU_ITEMS_NUMBER;
            } while (!menu_item_selectable(current_item, game_loaded));
        }
        if (gamepad1_bits.up) {
            do {
                current_item = (current_item - 1 + MENU_ITEMS_NUMBER) % MENU_ITEMS_NUMBER;
            } while (!menu_item_selectable(current_item, game_loaded));
        }

        sleep_ms(125);
    }

    ws_set_colour_scheme(palette_index);
    for (int i = 0; i < 16; ++i) {
        graphics_set_palette(i, ws_shades[i]);
    }

    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    ws_gpu_refresh_palette();

}

/* Renderer loop on Pico's second core */
void __time_critical_func(render_core)() {
    flash_safe_execute_core_init();

    i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = 256;
    i2s_volume(&i2s_config, 0);
    i2s_init(&i2s_config);
    apply_audio_volume();
    apply_audio_rate();

    ps2kbd.init_gpio();
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);

    graphics_init();

    const auto buffer = (uint8_t *) SCREEN1;
    graphics_set_buffer(buffer, 224, 144);
    graphics_set_textbuffer(buffer);
    graphics_set_bgcolor(0x000000);

    graphics_set_offset(48,48);

    graphics_set_flashmode(true, true);
    sem_acquire_blocking(&vga_start_semaphore);
    runtime_drivers_ready = true;

    // 60 FPS loop
#define frame_tick (16666)
    uint64_t tick = time_us_64();
    uint64_t last_frame_tick = tick;

    while (true) {

        if (tick >= last_frame_tick + frame_tick) {
#ifdef TFT
            refresh_lcd();
#endif
            ps2kbd.tick();
            nespad_tick();

            last_frame_tick = tick;
        }

        tick = time_us_64();

        // tuh_task();
        // hid_app_task();
        tight_loop_contents();
    }

    __unreachable();
}

int frame;
bool PSRAM_AVAILABLE = true;

int main() {
    overclock();

//    stdio_init_all();

    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }
#if PICO_RP2350
    if (wonderswan_qspi_psram_init()) {
        rom = WONDERSWAN_QSPI_PSRAM_BASE;
    } else
#endif
    {
        // Keep the legacy SPI PSRAM path for cartridge SRAM/EEPROM on boards
        // without memory-mapped QSPI PSRAM.
        init_psram();
    }
    /* Ensure the temporary backing directory exists before cartridge startup. */
    if (f_mount(&fs, "", 1) == FR_OK) f_mkdir("/tmp");

    while (true) {
        graphics_set_mode(TEXTMODE_DEFAULT);

        // rom_size lives in .uninitialized_data so it is intentionally not
        // zeroed by the C runtime. Before a cartridge has been selected it
        // must not be used as evidence that a ROM exists: leaving the browser
        // would otherwise feed an arbitrary size/address range into ws_init()
        // and the emulator core.
        rom_size = 0;
        filebrowser(HOME_DIR, "ws,wsc");
        if (rom_size == 0)
            continue;

        if (!ws_init((uint8_t *)rom, rom_size)) {
            graphics_set_mode(TEXTMODE_DEFAULT);
            draw_text("ERROR: not enough RAM for cartridge save memory!", 0, 0, 13, 0);
            sleep_ms(5000);
            continue;
        }
        if (filename[strlen(filename)-1]=='c'|| filename[strlen(filename)-1]=='C') {
            ws_set_system(WS_SYSTEM_COLOR);
        } else {
            ws_set_system(WS_SYSTEM_MONO);
        }

        ws_set_colour_scheme(0);
        ws_reset();
        for (int i = 0; i < 16; ++i) {
            graphics_set_palette(i, ws_shades[i]);
        }

        graphics_set_mode(GRAPHICSMODE_DEFAULT);

        frame = 0;
        int odd = 0;
        bool select_pressed_last_frame = false;
        bool portrait = portrait_enabled();
        ws_io_setControlsFlipped(portrait);
        uint8_t* buffer = portrait ? (uint8_t*)SCREEN1 : (uint8_t*)SCREEN2;
        uint64_t fps_started = time_us_64();
        uint32_t fps_frames = 0;
        graphics_set_fps_overlay(show_fps, 0);
#if defined(VGA) || defined(HDMI)
        // The WonderSwan video timing is 3.072 MHz / (256 cycles * 159 lines),
        // i.e. one emulated frame every 13250 us (~75.47 Hz). VGA is normally
        // slower, so use a third buffer and let VGA latch the newest completed
        // frame instead of throttling emulation to the physical video refresh.
        uint64_t next_ws_frame = time_us_64() + 13250;
#endif
        while (!reboot) {
            if (fxPressedV) {
                const uint8_t slot = fxPressedV;
                fxPressedV = 0;
                save_slot = slot;
                if (altPressed) load();
                else if (ctrlPressed) save();
            }
            ws_key_start = gamepad1_bits.start;

            ws_key_up = gamepad1_bits.up;
            ws_key_down = gamepad1_bits.down;
            ws_key_left = gamepad1_bits.left;
            ws_key_right = gamepad1_bits.right;

            if (gamepad1_bits.start && gamepad1_bits.select) {
                menu(true);
                /* Do not count time spent in the menu as an emulator slowdown. */
                fps_started = time_us_64();
                fps_frames = 0;
                graphics_set_fps_overlay(show_fps, 0);
            }

            // WonderSwan has no Select button. NES Select / keyboard
            // Backspace is an emulator hotkey that flips the presentation.
            if (!gamepad1_bits.start && gamepad1_bits.select && !select_pressed_last_frame)
                rotation_hotkey_override = !rotation_hotkey_override;
            select_pressed_last_frame = gamepad1_bits.select;

            portrait = portrait_enabled();

            const bool move_up    = keyboard_bits.up    || (nespad_state & DPAD_UP);
            const bool move_right = keyboard_bits.right || (nespad_state & DPAD_RIGHT);
            const bool move_down  = keyboard_bits.down  || (nespad_state & DPAD_DOWN);
            const bool move_left  = keyboard_bits.left  || (nespad_state & DPAD_LEFT);

            bool key_b;
            bool key_a;
            if (!portrait) {
                // Landscape: X is the movement cross, Y is available on 1..4.
                ws_key_x1 = move_up;
                ws_key_x2 = move_right;
                ws_key_x3 = move_down;
                ws_key_x4 = move_left;
                ws_key_y1 = keyboard_1;
                ws_key_y2 = keyboard_4;
                ws_key_y3 = keyboard_2;
                ws_key_y4 = keyboard_3;

                key_b = keyboard_o || keyboard_bits.b;       // O / Z
                key_a = keyboard_p || keyboard_bits.a;       // P / X
            } else {
                // Portrait follows the physical WonderSwan layout requested
                // by UAT rather than merely rotating the landscape mapping.
                ws_key_y1 = move_left;
                ws_key_y2 = move_up;
                ws_key_y3 = move_right;
                ws_key_y4 = move_down;
                ws_key_x1 = keyboard_o;
                ws_key_x2 = keyboard_p;
                ws_key_x3 = keyboard_semicolon || keyboard_bits.a; // ; / X
                ws_key_x4 = keyboard_l || keyboard_bits.b;         // L / Z

                key_b = keyboard_l || keyboard_bits.b || keyboard_9;
                key_a = keyboard_semicolon || keyboard_bits.a || keyboard_0;
            }

            // Swap only the physical A/B pair and keyboard keys currently
            // assigned to that pair. Native X/Y cursor mappings are untouched.
            if (swap_ab) {
                ws_key_button_1 = gamepad1_bits.b || key_b;
                ws_key_button_2 = gamepad1_bits.a || key_a;
            } else {
                ws_key_button_1 = gamepad1_bits.a || key_a;
                ws_key_button_2 = gamepad1_bits.b || key_b;
            }
            // Center the native image in the 320x240 VGA viewport. Landscape
            // is 224x144 -> (48,48); portrait is 144x224 -> (88,8).
            graphics_set_offset(portrait ? 88 : 48, portrait ? 8 : 48);

            // Portrait mode renders the native 224x144 frame into SCREEN1, then
            // rotates it to a 144x224 presentation buffer.
            if (portrait)
                buffer = (uint8_t*)SCREEN1;

            while(!ws_executeLine(buffer, 1)) ;
            uint8_t *present_buffer = buffer;
            if (portrait) {
#if defined(VGA)
                // SCREEN2/3 are presentation buffers. The pending (not active)
                // buffer may be replaced before scanout latches it at frame boundary.
                present_buffer = vga_is_buffer_active((uint8_t*)SCREEN2)
                               ? (uint8_t*)SCREEN3 : (uint8_t*)SCREEN2;
#elif defined(HDMI)
                present_buffer = hdmi_is_buffer_active((uint8_t*)SCREEN2)
                               ? (uint8_t*)SCREEN3 : (uint8_t*)SCREEN2;
#else
                present_buffer = (frame & 1) ? (uint8_t*)SCREEN2 : (uint8_t*)SCREEN3;
#endif
                rotate_frame_90cw((uint8_t*)SCREEN1, present_buffer);
                graphics_set_buffer(present_buffer, 144, 224);
            } else {
                graphics_set_buffer(present_buffer, 224, 144);
            }
            frame++;
            ++fps_frames;
            const uint64_t fps_now = time_us_64();
            const uint64_t fps_elapsed = fps_now - fps_started;
            if (fps_elapsed >= 1000000u) {
                const uint16_t fps_x10 = (uint16_t)(((uint64_t)fps_frames * 10000000u +
                                                     fps_elapsed / 2u) / fps_elapsed);
                graphics_set_fps_overlay(show_fps, fps_x10);
                fps_started = fps_now;
                fps_frames = 0;
            }
#if defined(VGA) || defined(HDMI)
            // Never render into either the buffer currently scanned out or
            // the newest completed frame waiting for the next physical frame boundary.
            // If emulation outruns scanout, replacing the pending frame is safe: the
            // dropped frame was never scanned out.
            if (!portrait) {
                uint8_t* const candidates[] = {
                    (uint8_t*)SCREEN1, (uint8_t*)SCREEN2, (uint8_t*)SCREEN3
                };
                do {
                    buffer = NULL;
                    for (unsigned i = 0; i < 3; ++i) {
#ifdef VGA
                        const bool in_use = vga_is_buffer_in_use(candidates[i]);
#else
                        const bool in_use = hdmi_is_buffer_in_use(candidates[i]);
#endif
                        if (!in_use) {
                            buffer = candidates[i];
                            break;
                        }
                    }
                    if (!buffer) tight_loop_contents();
                } while (!buffer);
            } else {
                buffer = (uint8_t*)SCREEN1;
            }

            while ((int64_t)(time_us_64() - next_ws_frame) < 0)
                i2s_dma_pump(&i2s_config);   // feed audio DMA while pacing the frame
            next_ws_frame += 13250;
            // Do not accumulate a large delay after menus or other long pauses.
            const uint64_t now = time_us_64();
            if ((int64_t)(now - next_ws_frame) > 13250)
                next_ws_frame = now + 13250;
#else
            if (!portrait) {
                odd = frame & 1;
                buffer = (uint8_t*)(odd ? SCREEN1 : SCREEN2);
            } else {
                buffer = (uint8_t*)SCREEN1;
            }

            // Keep the existing 60 Hz pacing for outputs whose drivers do not
            // yet provide frame-boundary buffer ownership.
            static uint8_t frame_cnt = 0;
            static uint64_t frame_timer_start = 0;
            if (++frame_cnt == 6) {
                while (time_us_64() - frame_timer_start < 16666 * 6)
                    tight_loop_contents();
                frame_timer_start = time_us_64();
                frame_cnt = 0;
            }
#endif

            tight_loop_contents();
        }

        reboot = false;
    }
    __unreachable();
}
