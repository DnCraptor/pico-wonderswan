#include <cstdio>
#include <cstring>
#include <cstdlib>
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
#include "wonderswan_backplane.h"
#include "audio.h"

#include "nespad.h"
#include "usbhid.h"
#include "ff.h"
#include "ps2kbd_mrmltr.h"
#include "psram_spi.h"
#include "sdcard.h"
#include "qspi_psram.h"

extern "C" {
#include "ws.h"
#include "ws_audio.h"
#ifdef HWAY
#include "hway/ay_hway.h"
#endif
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

volatile bool ws_backplane_enabled = false;
volatile bool ws_backplane_portrait = false;
uint8_t backplane_mode = 0;   // 0=Auto (.ws only), 1=Off

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

/* Palette editor owns hexadecimal key presses while it is open. */
static volatile bool palette_editor_active = false;
static volatile int8_t palette_hex_key = -1;
static volatile bool palette_tab_requested = false;
static volatile bool palette_f12_requested = false;
static volatile bool backplane_toggle_requested = false;
static volatile int8_t palette_layer_cycle_requested = 0;
static volatile int8_t palette_all_set_requested = -1;
static volatile bool palette_shuffle_requested = false; // F7
static volatile bool palette_shuffle_requested_no_save = false; // F6
static volatile bool game_palette_save_requested = false; // F8
static volatile bool filebrowser_direct_requested = false; // F10

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

    const uint32_t usbpad = usbhid_gamepad_state();
    const uint32_t pad = nespad_state | usbpad;

    gamepad1_bits.a = (pad & DPAD_A) != 0;
    gamepad1_bits.b = (pad & DPAD_B) != 0;

    gamepad1_bits.select = keyboard_bits.select || (pad & DPAD_SELECT) != 0;
    gamepad1_bits.start = keyboard_bits.start || (pad & DPAD_START) != 0;
    gamepad1_bits.up = keyboard_bits.up || (pad & DPAD_UP) != 0;
    gamepad1_bits.down = keyboard_bits.down || (pad & DPAD_DOWN) != 0;
    gamepad1_bits.left = keyboard_bits.left || (pad & DPAD_LEFT) != 0;
    gamepad1_bits.right = keyboard_bits.right || (pad & DPAD_RIGHT) != 0;
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
static volatile bool filebrowser_active = false;
static volatile bool filebrowser_page_up_requested = false;
static volatile bool filebrowser_page_down_requested = false;
static volatile bool filebrowser_help_requested = false;
static volatile bool filebrowser_help_close_requested = false;
static volatile bool filebrowser_return_requested = false;
static volatile bool filebrowser_can_return_to_game = false;
static volatile bool filebrowser_resumed_game = false;

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

    if (filebrowser_active) {
        if (isInReport(report, HID_KEY_PAGE_UP) && !isInReport(prev_report, HID_KEY_PAGE_UP))
            filebrowser_page_up_requested = true;
        if (isInReport(report, HID_KEY_PAGE_DOWN) && !isInReport(prev_report, HID_KEY_PAGE_DOWN))
            filebrowser_page_down_requested = true;
        if (isInReport(report, HID_KEY_F1) && !isInReport(prev_report, HID_KEY_F1))
            filebrowser_help_requested = true;
        if (isInReport(report, HID_KEY_ESCAPE) && !isInReport(prev_report, HID_KEY_ESCAPE))
            filebrowser_help_close_requested = true;
    }

    /* Palette hotkeys are edge-triggered. F1..F5 cycle one layer forward,
       or backward while Shift is held. Ctrl/Alt + F1..F8 remain reserved
       for quick-state operations. */
    const bool palette_shift = (report->modifier &
            (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
    const bool palette_modifier_conflict = (report->modifier &
            (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL |
             KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) != 0;
    if (!palette_modifier_conflict) {
        const uint8_t palette_layer_keys[5] = {
            HID_KEY_F1, HID_KEY_F2, HID_KEY_F3, HID_KEY_F4, HID_KEY_F5
        };
        for (unsigned layer = 0; layer < 5; ++layer) {
            const uint8_t key = palette_layer_keys[layer];
            if (isInReport(report, key) && !isInReport(prev_report, key)) {
                /* F1 belongs to browser Help while the ROM browser is active. */
                if (!(filebrowser_active && layer == 0))
                    palette_layer_cycle_requested = palette_shift ? -(int8_t)(layer + 1) : (int8_t)(layer + 1);
                break;
            }
        }
        if (isInReport(report, HID_KEY_F6) && !isInReport(prev_report, HID_KEY_F6))
            palette_shuffle_requested_no_save = true;
        if (isInReport(report, HID_KEY_F7) && !isInReport(prev_report, HID_KEY_F7))
            palette_shuffle_requested = true;
        if (isInReport(report, HID_KEY_F8) && !isInReport(prev_report, HID_KEY_F8))
            game_palette_save_requested = true;
        if (isInReport(report, HID_KEY_F9) && !isInReport(prev_report, HID_KEY_F9))
            palette_all_set_requested = 0;
        if (isInReport(report, HID_KEY_F10) && !isInReport(prev_report, HID_KEY_F10)) {
            if (filebrowser_active && filebrowser_can_return_to_game)
                filebrowser_return_requested = true;
            else if (!filebrowser_active)
                filebrowser_direct_requested = true;
        }
    }

    /* F11 toggles Backplane; F12 toggles the palette editor. */
    if (isInReport(report, HID_KEY_F11) && !isInReport(prev_report, HID_KEY_F11))
        backplane_toggle_requested = true;

    /* F12 is an edge-triggered direct palette-editor toggle. */
    if (isInReport(report, HID_KEY_F12) && !isInReport(prev_report, HID_KEY_F12))
        palette_f12_requested = true;

    if (palette_editor_active && isInReport(report, HID_KEY_TAB) && !isInReport(prev_report, HID_KEY_TAB))
        palette_tab_requested = true;

    if (palette_editor_active) {
        int8_t hex = -1;
#define PALETTE_HEX_KEY(key, value) \
        if (hex < 0 && isInReport(report, key) && !isInReport(prev_report, key)) hex = value
        PALETTE_HEX_KEY(HID_KEY_0, 0);
        PALETTE_HEX_KEY(HID_KEY_1, 1);
        PALETTE_HEX_KEY(HID_KEY_2, 2);
        PALETTE_HEX_KEY(HID_KEY_3, 3);
        PALETTE_HEX_KEY(HID_KEY_4, 4);
        PALETTE_HEX_KEY(HID_KEY_5, 5);
        PALETTE_HEX_KEY(HID_KEY_6, 6);
        PALETTE_HEX_KEY(HID_KEY_7, 7);
        PALETTE_HEX_KEY(HID_KEY_8, 8);
        PALETTE_HEX_KEY(HID_KEY_9, 9);
        PALETTE_HEX_KEY(HID_KEY_A, 10);
        PALETTE_HEX_KEY(HID_KEY_B, 11);
        PALETTE_HEX_KEY(HID_KEY_C, 12);
        PALETTE_HEX_KEY(HID_KEY_D, 13);
        PALETTE_HEX_KEY(HID_KEY_E, 14);
        PALETTE_HEX_KEY(HID_KEY_F, 15);
#undef PALETTE_HEX_KEY
        if (hex >= 0) palette_hex_key = hex;
    }

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
    keyboard_bits.left = b7 || b1 || isInReport(report, HID_KEY_ARROW_LEFT) ||
                         (!palette_editor_active && isInReport(report, HID_KEY_A)) || isInReport(report, HID_KEY_KEYPAD_4);
    keyboard_bits.right = b9 || b3 || isInReport(report, HID_KEY_ARROW_RIGHT) ||
                          (!palette_editor_active && isInReport(report, HID_KEY_D)) || isInReport(report, HID_KEY_KEYPAD_6);

    altPressed = isInReport(report, HID_KEY_ALT_LEFT) || isInReport(report, HID_KEY_ALT_RIGHT);
    ctrlPressed = isInReport(report, HID_KEY_CONTROL_LEFT) || isInReport(report, HID_KEY_CONTROL_RIGHT);

    if (altPressed && ctrlPressed && isInReport(report, HID_KEY_DELETE)) {
        *(uint32_t *)0x400d000c = 0x60007204;
        set_sys_clock_khz(150000, false);
        sleep_ms(100);
        vreg_set_voltage(VREG_VOLTAGE_1_10);
        watchdog_reboot(0, 0, 0);
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
static FIL file;

static bool demo_requested = false;
static bool demo_active = false;
static bool demo_advance_pending = false;
static uint64_t demo_game_started_at = 0;
static char demo_current_name[79] = { 0 };
static uint8_t demo_duration = 0;
static const uint16_t demo_seconds[] = { 15, 30, 45, 60, 120, 180, 300, 600 };
static uint64_t palette_overlay_until = 0;

static void demo_update_title(void) {
    const bool visible = demo_active && demo_current_name[0] &&
        time_us_64() - demo_game_started_at < 10000000ull;
    if (!visible) {
        graphics_set_demo_overlay(false, nullptr);
        return;
    }

    char title[53];
    const char *dot = strrchr(demo_current_name, '.');
    size_t len = dot ? (size_t)(dot - demo_current_name) : strlen(demo_current_name);
    if (len > sizeof(title) - 1) len = sizeof(title) - 1;
    for (size_t i = 0; i < len; ++i)
        title[i] = demo_current_name[i] == '_' ? ' ' : demo_current_name[i];
    title[len] = '\0';
    graphics_set_demo_overlay(true, title);
}

static void demo_stop(void) {
    demo_active = false;
    demo_requested = false;
    demo_advance_pending = false;
    demo_game_started_at = 0;
    demo_current_name[0] = '\0';
    graphics_set_demo_overlay(false, nullptr);
}

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

#ifdef HWAY
    ws_audio_hway_silence();
#endif

    constexpr int window_y = (TEXTMODE_ROWS - 5) / 2;
    constexpr int window_x = (TEXTMODE_COLS - 43) / 2;

    draw_window("Loading ROM", window_x, window_y, 43, 5);

    FILINFO fileinfo;
    if (FR_OK != f_stat(pathname, &fileinfo) || fileinfo.fsize == 0) {
        draw_text("ERROR: ROM not found or empty!", window_x + 1, window_y + 2, 13, 1);
        sleep_ms(demo_active ? 1500 : 5000);
        return false;
    }

    const uint32_t load_size = fileinfo.fsize;
    if (((16384 - 64) << 10) < load_size) {
        draw_text("ERROR: ROM too large! Canceled!!", window_x + 1, window_y + 2, 13, 1);
        sleep_ms(demo_active ? 1500 : 5000);
        return false;
    }

    draw_text("Loading...", window_x + 1, window_y + 2, 10, 1);

    const bool psram_available = wonderswan_qspi_psram_available();
    const size_t psram_capacity = psram_available ? wonderswan_qspi_rom_capacity() : 0;
    const bool use_psram = psram_available && load_size <= psram_capacity;
    bool load_ok = false;

    if (use_psram) {
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
        if (load_ok)
            rom = WONDERSWAN_QSPI_PSRAM_BASE;
    } else {
        const uint32_t firmware_end = (uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE);
        if (firmware_end > FLASH_TARGET_OFFSET) {
            draw_text("ERROR: Firmware overlaps ROM flash area!", window_x + 1, window_y + 2, 13, 1);
            sleep_ms(demo_active ? 1500 : 5000);
            return false;
        }

        const uint32_t flash_size = detect_flash_size_bytes();
        if (FLASH_TARGET_OFFSET >= flash_size || load_size > flash_size - FLASH_TARGET_OFFSET) {
            draw_text(psram_available ? "ERROR: ROM too large for PSRAM/flash!"
                                      : "ERROR: ROM too large for flash!",
                      window_x + 1, window_y + 2, 13, 1);
            sleep_ms(demo_active ? 1500 : 5000);
            return false;
        }

        const uint32_t original_sys_khz = clock_get_hz(clk_sys) / 1000u;
        const bool need_clock_restore = original_sys_khz > 252000u;
        if (need_clock_restore && !temporary_flash_reclock(252000u)) {
            draw_text("ERROR: Cannot lower clock for flash!", window_x + 1, window_y + 2, 13, 1);
            sleep_ms(demo_active ? 1500 : 5000);
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
        } else {
            read_result = FR_NO_FILE;
        }

        if (need_clock_restore && !temporary_flash_reclock(original_sys_khz))
            read_result = FR_DISK_ERR;

        gpio_put(PICO_DEFAULT_LED_PIN, true);
        load_ok = read_result == FR_OK && total_read == load_size;
        if (load_ok)
            rom = XIP_BASE + FLASH_TARGET_OFFSET;
    }

    if (!load_ok) {
        draw_text("ERROR: ROM load failed!", window_x + 1, window_y + 2, 13, 1);
        sleep_ms(demo_active ? 1500 : 5000);
        return false;
    }

    /* Commit cartridge identity only after the selected backing store contains
       the complete ROM. A failed attempt leaves the next browser selection clean. */
    rom_size = load_size;
    strcpy(filename, fileinfo.fname);
    return true;
}


static bool demo_load_next_rom(const char *after_name) {
    if (FR_OK != f_mount(&fs, "SD", 1))
        return false;

    char after[79] = { 0 };
    if (after_name) {
        strncpy(after, after_name, sizeof(after) - 1);
        after[sizeof(after) - 1] = '\0';
    }

    /* Find the next name alphabetically. If a ROM fails to load, advance
       past it instead of dropping out of Demo mode. */
    for (;;) {
        DIR dir;
        FILINFO info;
        if (FR_OK != f_opendir(&dir, HOME_DIR))
            return false;

        char best[79] = { 0 };
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
            if (info.fattrib & AM_DIR)
                continue;
            if (!isExecutable(info.fname, "ws,wsc"))
                continue;
            if (after[0] && strcmp(info.fname, after) <= 0)
                continue;
            if (!best[0] || strcmp(info.fname, best) < 0) {
                strncpy(best, info.fname, sizeof(best) - 1);
                best[sizeof(best) - 1] = '\0';
            }
        }
        f_closedir(&dir);
        if (!best[0])
            return false;

        char pathname[256];
        snprintf(pathname, sizeof(pathname), "%s\\%s", HOME_DIR, best);
        if (filebrowser_loadfile(pathname)) {
            strncpy(demo_current_name, best, sizeof(demo_current_name) - 1);
            demo_current_name[sizeof(demo_current_name) - 1] = '\0';
            demo_game_started_at = time_us_64();
            return true;
        }

        strncpy(after, best, sizeof(after) - 1);
        after[sizeof(after) - 1] = '\0';
    }
}

static void filebrowser_show_help(void) {
    static const char *const lines[] = {
        "Arrows/WASD/NumPad - movement",
        "Enter - START",
        "X/P - A,  Z/O - B (landscape)",
        ";/X/0 - A,  L/Z/9 - B (portrait)",
        "1/4/2/3 - Y1/Y2/Y3/Y4 (landscape)",
        "O/P/;/L - X1/X2/X3/X4 (portrait)",
        "Backspace/Esc/Num+ - rotate presentation",
        "START+Select - menu",
        "",
        "F1..F5 - next layer palette",
        "Shift+F1..F5 - previous layer palette",
        "F6 - Shuffle and not save",
        "F7 - Shuffle and save",
        "F8 - Save colors for this game",
        "F9 - All Defaults",
        "F10 - File manager",
        "F11 - Backplane on/off",
        "F12 - Custom palette editor; Tab - layer",
        "Ctrl+F1..F8 - save state 1..8",
        "Alt+F1..F8 - load state 1..8",
        "Ctrl+Alt+Del - reboot",
        "",
        "Esc - close Help"
    };
    /* HDMI text output has only 53 visible columns even though the shared
       text buffer is wider. Keep Help inside the actually visible viewport
       and centre it against that viewport, not TEXTMODE_COLS. */
    constexpr uint32_t width = 44;
    constexpr uint32_t height = count_of(lines) + 2;
#ifdef HDMI
    constexpr uint32_t visible_cols = 53;
#else
    constexpr uint32_t visible_cols = TEXTMODE_COLS;
#endif
    const uint32_t x = (visible_cols - width) / 2;
    const uint32_t y = (TEXTMODE_ROWS - height) / 2;

    draw_window("Game mode hot-keys", x, y, width, height);
    for (unsigned i = 0; i < count_of(lines); ++i)
        draw_text(lines[i], x + 2, y + 1 + i, 15, 1);

    filebrowser_help_requested = false;
    filebrowser_help_close_requested = false;
    while (!filebrowser_help_close_requested)
        sleep_ms(10);
    filebrowser_help_close_requested = false;
}

void filebrowser(const char pathname[256], const char executables[11]) {
    struct filebrowser_active_guard_t {
        filebrowser_active_guard_t() { filebrowser_active = true; }
        ~filebrowser_active_guard_t() {
            filebrowser_active = false;
            filebrowser_page_up_requested = false;
            filebrowser_page_down_requested = false;
            filebrowser_help_requested = false;
            filebrowser_help_close_requested = false;
            filebrowser_return_requested = false;
            filebrowser_can_return_to_game = false;
        }
    } filebrowser_active_guard;
    bool debounce = true;
    bool demo_debounce = false;
    /* filebrowser() remains on the stack while menu(false) is open.  Keep its
       sizeable work buffers out of that nested call chain. */
    static char basepath[256];
    static char tmp[TEXTMODE_COLS + 1];
    strcpy(basepath, pathname);
    constexpr int per_page = TEXTMODE_ROWS - 3;

    static DIR dir;
    static FILINFO fileInfo;

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
        draw_text("A", off, 29, 7, 0);
        off += 1;
        draw_text(" USB DRV ", off, 29, 0, 3);
        if (filebrowser_can_return_to_game) {
            off += 9;
            draw_text("F10", off, 29, 7, 0);
            off += 3;
            draw_text(" Return ", off, 29, 0, 3);
        }
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
                debounce = !(gamepad1_bits.start);
            }

            if (filebrowser_help_requested) {
                filebrowser_show_help();
                /* Help overwrites the browser; rebuild this directory page. */
                break;
            }

            if (filebrowser_return_requested && filebrowser_can_return_to_game) {
                filebrowser_return_requested = false;
                filebrowser_resumed_game = true;
                return;
            }

            constexpr int half_page = per_page / 2;
            if (filebrowser_page_down_requested) {
                filebrowser_page_down_requested = false;
                int selected = offset + current_item + half_page;
                if (selected >= total_files) selected = total_files - 1;
                if (selected < offset + per_page) {
                    current_item = selected - offset;
                } else {
                    current_item = per_page - 1;
                    offset = selected - current_item;
                }
            }
            if (filebrowser_page_up_requested) {
                filebrowser_page_up_requested = false;
                int selected = offset + current_item - half_page;
                if (selected < 0) selected = 0;
                if (selected >= offset) {
                    current_item = selected - offset;
                } else {
                    current_item = 0;
                    offset = selected;
                }
            }

            // SELECT opens the emulator menu even before a cartridge is
            // loaded.  Returning from that menu must come back to the ROM
            // browser, not fall through into ws_init()/emulation.
            if (gamepad1_bits.select) {
                menu(false);
                if (demo_requested)
                    return;
                debounce = false;
                break;
            }

            const bool demo_button = gamepad1_bits.b;
            if (!demo_button)
                demo_debounce = true;
            if (demo_debounce && demo_button) {
                demo_requested = true;
                return;
            }

            if (gamepad1_bits.down) {
                if (offset + (current_item + 1) < total_files) {
                    if (current_item + 1 < per_page) {
                        current_item++;
                    } else {
                        offset++;
                    }
                }
            }

            if (gamepad1_bits.up) {
                if (current_item > 0) {
                    current_item--;
                } else if (offset > 0) {
                    offset--;
                }
            }

            if (gamepad1_bits.right) {
                offset += per_page;
                if (offset + (current_item + 1) > total_files) {
                    offset = total_files - (current_item + 1);
                }
            }

            if (gamepad1_bits.left) {
                if (offset > per_page) {
                    offset -= per_page;
                } else {
                    offset = 0;
                    current_item = 0;
                }
            }

            if (debounce && (gamepad1_bits.start)) {
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
    START_DEMO,
    DEFAULTS,
    ROM_SELECT,
    SHOW_PALETTES,
    GAME_PALETTE,
    RETURN,
};

typedef bool (*menu_callback_t)();

typedef struct __attribute__((__packed__)) {
    const char *text;
    menu_type_e type;
    const void *value;
    menu_callback_t callback;
    uint8_t max_value;
    char value_list[18][10];
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
#ifndef HWAY
    i2s_reclock(&i2s_config);
#endif
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
        vreg_set_voltage(VREG_VOLTAGE_1_50);
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
    if (f_open(&file, pathname, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) { free(state); return false; }
    bool ok = state_write(&file, &h, sizeof(h)) && state_write(&file, &state->cpu, sizeof(state->cpu)) && state_write(&file, &state->io, sizeof(state->io)) && state_write(&file, &state->gpu, sizeof(state->gpu)) && state_write(&file, &state->audio, sizeof(state->audio)) && state_write(&file, internalRam, sizeof(internalRam)) && state_write_cart(&file, 1u << 20, h.sram_size) && state_write_cart(&file, 0, h.eeprom_size);
    if (ok) ok = f_sync(&file) == FR_OK;
    f_close(&file);
    free(state);
    if (!ok) f_unlink(pathname);
    return ok;
}

bool load() {
    if (!rom_size || save_slot < 1 || save_slot > 8) return false;
    char pathname[255];
    snprintf(pathname, sizeof(pathname), "%s\\%s_%d.save", HOME_DIR, filename, save_slot);
    if (f_mount(&fs, "", 1) != FR_OK) return false;
    if (f_open(&file, pathname, FA_READ) != FR_OK) return false;
    ws_state_header_t h; bool ok = state_read(&file, &h, sizeof(h));
    ok = ok && h.magic == WS_STATE_MAGIC && h.version == WS_STATE_VERSION && h.rom_size == rom_size && h.rom_crc == memory_getRomCrc() && h.internal_ram_size == sizeof(internalRam) && h.sram_size == ws_memory_get_sram_size() && h.eeprom_size == ws_memory_get_eeprom_size() && h.cpu_size == sizeof(nec_snapshot_t) && h.io_size == sizeof(ws_io_snapshot_t) && h.gpu_size == sizeof(ws_gpu_snapshot_t) && h.audio_size == sizeof(ws_audio_snapshot_t);
    ws_state_core_t *state = ok ? (ws_state_core_t *)malloc(sizeof(*state)) : nullptr;
    if (ok && !state) ok = false;
    if (ok) ok = state_read(&file, &state->cpu, sizeof(state->cpu)) && state_read(&file, &state->io, sizeof(state->io)) && state_read(&file, &state->gpu, sizeof(state->gpu)) && state_read(&file, &state->audio, sizeof(state->audio)) && state_read(&file, internalRam, sizeof(internalRam)) && state_read_cart(&file, 1u << 20, h.sram_size) && state_read_cart(&file, 0, h.eeprom_size);
    f_close(&file);
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

/* Only PAL vs NTSC is user-selectable; line count, colour subcarrier and
   interlace shifts are derived automatically (like modern soft-composite
   drivers) - which is also why the picture no longer destabilises on toggles. */
uint8_t ws_tv_system = 0;   /* 0 = PAL, 1 = NTSC */
static void tv_apply_system(void) {
    if (ws_tv_system) {
        tv_out_mode.tv_system = g_TV_OUT_NTSC;
        tv_out_mode.N_lines   = _525_lines;
        tv_out_mode.c_freq    = _3579545;
    } else {
        tv_out_mode.tv_system = g_TV_OUT_PAL;
        tv_out_mode.N_lines   = _625_lines;
        tv_out_mode.c_freq    = _4433619;
    }
    tv_out_mode.cb_sync_PI_shift_lines = false;
    tv_out_mode.cb_sync_PI_shift_half_frame = false;
}
static bool apply_tv_system(void) {
    tv_apply_system();
    return true;
}
#endif
enum palette_mode_e : uint8_t {
    PALETTE_DEFAULT = 0,
    PALETTE_RED,
    PALETTE_ORANGE,
    PALETTE_GREEN,
    PALETTE_BLUE,
    PALETTE_PURPLE,
    PALETTE_OGBP,
    PALETTE_OGBR,
    PALETTE_GBPR,
    PALETTE_GBPO,
    PALETTE_BPRO,
    PALETTE_BROG,
    PALETTE_PROG,
    PALETTE_POGR,
    PALETTE_ROGB,
    PALETTE_ROGP,
    PALETTE_RANDOM,
    PALETTE_CUSTOM
};
enum palette_layer_e : uint8_t {
    PALETTE_BACK = 0, PALETTE_SCREEN0, PALETTE_SPRITES0, PALETTE_SCREEN1, PALETTE_SPRITES1, PALETTE_LAYER_COUNT
};
uint8_t palette_index[PALETTE_LAYER_COUNT] = { PALETTE_DEFAULT, PALETTE_DEFAULT, PALETTE_DEFAULT, PALETTE_DEFAULT, PALETTE_DEFAULT };
bool show_fps = false;
uint8_t audio_volume = 4;
uint8_t audio_rate_shift = 0;
uint8_t frame_skip = 0;   // 0=75Hz (render every frame) 1=50Hz 2=25Hz 3=Auto

static bool apply_audio_volume() {
    ws_audio_set_enabled(audio_volume != 0);
    if (audio_volume != 0) {
        static const uint8_t attenuation[] = { 0, 3, 2, 1, 0 };
#ifndef HWAY
        i2s_volume(&i2s_config, attenuation[audio_volume]);
#else
        ws_audio_set_hway_volume(audio_volume);
#endif
    }
    return false;
}

static bool apply_audio_rate() {
    ws_audio_set_rate_shift(audio_rate_shift);
    return false;
}

static bool mono_ws_rom_loaded(bool game_loaded);

#define WS_CONFIG_MAGIC 0x31434657u /* WFC1 */
#define WS_CONFIG_VERSION 10u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t swap_ab;
    uint8_t rotation_mode;
    uint8_t show_fps;
    uint8_t audio_volume;
    uint8_t audio_rate_shift;
    uint8_t frame_skip;
    uint8_t demo_duration;
    uint8_t palette_mode; /* Back; retained in place for v8-compatible prefix */
    uint8_t backplane_mode;
    uint8_t palette_modes[4]; /* Screen1, Sprites0, Screen2, Sprites1 */
    uint32_t custom_shades[16];
} ws_config_t;

static bool game_palette_linked = false;
static uint32_t global_ws_shades[16];
static bool global_palette_valid = false;

static const uint32_t default_ws_shades[16] = {
    0xf0f0f0, 0xe0e0e0, 0xd0d0d0, 0xc0c0c0,
    0xb0b0b0, 0xa0a0a0, 0x909090, 0x808080,
    0x707070, 0x606060, 0x505050, 0x404040,
    0x303030, 0x202020, 0x101010, 0x000000
};
static const uint32_t red_ws_shades[16] = {
    0xfff1f1, 0xffd6d6, 0xffbebe, 0xfda2a2,
    0xff8989, 0xff6a6a, 0xff5353, 0xef3c3c,
    0xd72a2a, 0xb71919, 0x950e0e, 0x7f0404,
    0x750909, 0x560202, 0x3f0101, 0x0c0422
};
static const uint32_t orange_ws_shades[16] = {
    0xfffbf1, 0xfdefca, 0xffd871, 0xffbf16,
    0xdba30f, 0xc69209, 0xb58504, 0xa17602,
    0x926a00, 0x846208, 0x755707, 0x735302,
    0x5b4202, 0x5b4202, 0x443100, 0x231900
};
static const uint32_t green_ws_shades[16] = {
    0xf1fff8, 0xc3ffe1, 0x86f8bf, 0x54e99e,
    0x41e291, 0x2acd7b, 0x27bb70, 0x20af66,
    0x20ab64, 0x1c9e5c, 0x159051, 0x15824c,
    0x117142, 0x0e6439, 0x085b32, 0x04361d
};
static const uint32_t blue_ws_shades[16] = {
    0xf1fcff, 0xcff5ff, 0xb5f0ff, 0x87e3fa,
    0x72dcf6, 0x61cfea, 0x56cae7, 0x44bddb,
    0x3ab1cf, 0x2aa1c0, 0x2493b0, 0x1e8aa6,
    0x19809a, 0x127088, 0x0a5062, 0x03232c
};
static const uint32_t purple_ws_shades[16] = {
    0xebeef4, 0xd3d9e5, 0xc9d2e3, 0xbcc8de,
    0xa5b2ca, 0x89a1cc, 0x6b8dca, 0x4c79c9,
    0x396ac3, 0x2a5ebc, 0x2355b0, 0x1b489c,
    0x123f95, 0x0d388b, 0x042460, 0x011334
};
static const uint32_t ogbp_ws_shades[16] = {
    0xfffbf1, 0xfdefca, 0xffd871, 0xffbf16,
    0x41e291, 0x2acd7b, 0x27bb70, 0x20af66,
    0x3ab1cf, 0x2aa1c0, 0x2493b0, 0x1e8aa6,
    0x123f95, 0x0d388b, 0x042460, 0x011334
};
static const uint32_t ogbr_ws_shades[16] = {
    0xfffbf1, 0xfdefca, 0xffd871, 0xffbf16,
    0x41e291, 0x2acd7b, 0x27bb70, 0x20af66,
    0x3ab1cf, 0x2aa1c0, 0x2493b0, 0x1e8aa6,
    0x750909, 0x560202, 0x3f0101, 0x0c0422
};
static const uint32_t gbpr_ws_shades[16] = {
    0xf1fff8, 0xc3ffe1, 0x86f8bf, 0x54e99e,
    0x72dcf6, 0x61cfea, 0x56cae7, 0x44bddb,
    0x396ac3, 0x2a5ebc, 0x2355b0, 0x1b489c,
    0x750909, 0x560202, 0x3f0101, 0x0c0422
};
static const uint32_t gbpo_ws_shades[16] = {
    0xf1fff8, 0xc3ffe1, 0x86f8bf, 0x54e99e,
    0x72dcf6, 0x61cfea, 0x56cae7, 0x44bddb,
    0x396ac3, 0x2a5ebc, 0x2355b0, 0x1b489c,
    0x5b4202, 0x5b4202, 0x443100, 0x231900
};
static const uint32_t bpro_ws_shades[16] = {
    0xf1fcff, 0xcff5ff, 0xb5f0ff, 0x87e3fa,
    0xa5b2ca, 0x89a1cc, 0x6b8dca, 0x4c79c9,
    0xd72a2a, 0xb71919, 0x950e0e, 0x7f0404,
    0x5b4202, 0x5b4202, 0x443100, 0x231900
};
static const uint32_t brog_ws_shades[16] = {
    0xf1fcff, 0xcff5ff, 0xb5f0ff, 0x87e3fa,
    0xff8989, 0xff6a6a, 0xff5353, 0xef3c3c,
    0x926a00, 0x846208, 0x755707, 0x735302,
    0x117142, 0x0e6439, 0x085b32, 0x04361d
};
static const uint32_t prog_ws_shades[16] = {
    0xebeef4, 0xd3d9e5, 0xc9d2e3, 0xbcc8de,
    0xff8989, 0xff6a6a, 0xff5353, 0xef3c3c,
    0x926a00, 0x846208, 0x755707, 0x735302,
    0x117142, 0x0e6439, 0x085b32, 0x04361d
};
static const uint32_t pogr_ws_shades[16] = {
    0xebeef4, 0xd3d9e5, 0xc9d2e3, 0xbcc8de,
    0xdba30f, 0xc69209, 0xb58504, 0xa17602,
    0x20ab64, 0x1c9e5c, 0x159051, 0x15824c,
    0x19809a, 0x127088, 0x0a5062, 0x03232c
};
static const uint32_t rogb_ws_shades[16] = {
    0xfff1f1, 0xffd6d6, 0xffbebe, 0xfda2a2,
    0xdba30f, 0xc69209, 0xb58504, 0xa17602,
    0x20ab64, 0x1c9e5c, 0x159051, 0x15824c,
    0x19809a, 0x127088, 0x0a5062, 0x03232c
};
static const uint32_t rogp_ws_shades[16] = {
    0xfff1f1, 0xffd6d6, 0xffbebe, 0xfda2a2,
    0xdba30f, 0xc69209, 0xb58504, 0xa17602,
    0x20ab64, 0x1c9e5c, 0x159051, 0x15824c,
    0x123f95, 0x0d388b, 0x042460, 0x011334
};
static uint32_t random_ws_shades[16];
static uint32_t random_palette_state = 0x6d2b79f5u;

static uint32_t random_palette_next(void) {
    uint32_t x = random_palette_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    random_palette_state = x ? x : 0x6d2b79f5u;
    return random_palette_state;
}

static void init_custom_palette_from_default(void) {
    for (unsigned i = 0; i < 16; ++i)
        global_ws_shades[i] = default_ws_shades[i];
    global_palette_valid = true;
}

static void capture_global_palette(void) {
    for (unsigned i = 0; i < 16; ++i)
        global_ws_shades[i] = ws_shades[i] & 0x00ffffffu;
    global_palette_valid = true;
}

static void apply_global_palette(void) {
    if (!global_palette_valid)
        init_custom_palette_from_default();
    for (unsigned i = 0; i < 16; ++i)
        ws_shades[i] = global_ws_shades[i];
}

static const char *const palette_mode_names[] = {
    "Default", "Red", "Orange", "Green", "Blue", "Purple",
    "OGBP", "OGBR", "GBPR", "GBPO", "BPRO", "BROG", "PROG", "POGR", "ROGB", "ROGP",
    "Random", "Custom"
};

static const char *const palette_layer_names[PALETTE_LAYER_COUNT] = {
    "Back", "Screen 0", "Sprites 0", "Screen 1", "Sprites 1"
};

static const uint32_t *palette_colors_for_mode(uint8_t mode);

static void generate_random_palette(void) {
    const uint64_t seed = time_us_64();
    random_palette_state ^= (uint32_t)seed ^ (uint32_t)(seed >> 32);
    if (!random_palette_state) random_palette_state = 0x6d2b79f5u;

    for (unsigned row = 0; row < 4; ++row) {
        uint8_t sources[PALETTE_RANDOM];
        for (unsigned i = 0; i < PALETTE_RANDOM; ++i) sources[i] = (uint8_t)i;

        for (unsigned column = 0; column < 4; ++column) {
            const unsigned remaining = PALETTE_RANDOM - column;
            const unsigned pick = column + random_palette_next() % remaining;
            const uint8_t source = sources[pick];
            sources[pick] = sources[column];
            sources[column] = source;

            const unsigned shade = row * 4u + column;
            random_ws_shades[shade] = palette_colors_for_mode(source)[shade];
        }
    }
}

static const uint32_t *palette_colors_for_mode(uint8_t mode) {
    switch (mode) {
        case PALETTE_RED: return red_ws_shades;
        case PALETTE_ORANGE: return orange_ws_shades;
        case PALETTE_GREEN: return green_ws_shades;
        case PALETTE_BLUE: return blue_ws_shades;
        case PALETTE_PURPLE: return purple_ws_shades;
        case PALETTE_OGBP: return ogbp_ws_shades;
        case PALETTE_OGBR: return ogbr_ws_shades;
        case PALETTE_GBPR: return gbpr_ws_shades;
        case PALETTE_GBPO: return gbpo_ws_shades;
        case PALETTE_BPRO: return bpro_ws_shades;
        case PALETTE_BROG: return brog_ws_shades;
        case PALETTE_PROG: return prog_ws_shades;
        case PALETTE_POGR: return pogr_ws_shades;
        case PALETTE_ROGB: return rogb_ws_shades;
        case PALETTE_ROGP: return rogp_ws_shades;
        case PALETTE_RANDOM: return random_ws_shades;
        case PALETTE_CUSTOM:
            if (!global_palette_valid) init_custom_palette_from_default();
            return global_ws_shades;
        case PALETTE_DEFAULT:
        default: return default_ws_shades;
    }
}

static void apply_selected_palettes(void) {
    /* WonderSwan Color keeps the original 256-entry hardware palette path. */
    if (ws_gpu_operatingInColor) {
        ws_gpu_refresh_palette();
        return;
    }

    /* Mono .ws renderer emits five independent 16-entry index banks. */
    for (unsigned layer = 0; layer < PALETTE_LAYER_COUNT; ++layer) {
        const uint32_t *colors = palette_colors_for_mode(palette_index[layer]);
        for (unsigned i = 0; i < 16; ++i)
            graphics_set_palette((uint8_t)(layer * 16u + i), colors[i] & 0x00ffffffu);
    }

    /* Keep ws_shades as the editable 16-colour palette used by the existing
       palette editor and per-game Custom INI. */
    const uint32_t *back = palette_colors_for_mode(palette_index[PALETTE_BACK]);
    for (unsigned i = 0; i < 16; ++i) ws_shades[i] = back[i] & 0x00ffffffu;

}

static void show_palette_overlay(unsigned layer) {
    if (layer >= PALETTE_LAYER_COUNT) return;

    char title[53];
    snprintf(title, sizeof(title), "%s -> %s",
             palette_layer_names[layer],
             palette_mode_names[palette_index[layer]]);
    graphics_set_demo_overlay(true, title);
    palette_overlay_until = time_us_64() + 2000000ull;
}

static void update_palette_overlay(void) {
    if (!palette_overlay_until || time_us_64() < palette_overlay_until) return;
    palette_overlay_until = 0;
    demo_update_title();
}

static void apply_current_palette_to_video(void) {
    apply_selected_palettes();
}

static void apply_selected_palette(void) {
    apply_selected_palettes();
}

static void ensure_custom_palette_for_edit(unsigned layer) {
    if (layer >= PALETTE_LAYER_COUNT || palette_index[layer] == PALETTE_CUSTOM)
        return;

    /* The first edit turns the currently displayed source into Custom and
       assigns Custom to that source layer.  The other four selectors remain
       untouched. */
    capture_global_palette();
    palette_index[layer] = PALETTE_CUSTOM;
}

static void palette_editor_load_layer(unsigned layer) {
    if (layer >= PALETTE_LAYER_COUNT) return;
    const uint32_t *colors = palette_colors_for_mode(palette_index[layer]);
    for (unsigned i = 0; i < 16; ++i)
        ws_shades[i] = colors[i] & 0x00ffffffu;
}

static void palette_editor_update_title(unsigned layer) {
    if (layer >= PALETTE_LAYER_COUNT) return;
    char title[53];
    snprintf(title, sizeof(title), "%s: %s",
             palette_layer_names[layer], palette_mode_names[palette_index[layer]]);
    graphics_set_demo_overlay(true, title);
}


static const char* config_video_name(void) {
#if HDMI
    return "hdmi";
#elif VGA
    return "vga";
#elif SOFTTV
    return "softtv";
#elif TV
    return "tv";
#elif TFT
    return "tft";
#else
    return "unknown";
#endif
}

/* Per-video-type config so backends with different settings never share one
   file. Text .ini (key=value): a new key never invalidates an existing file -
   no magic/version/size gate, no migrations. */
static void config_path(char* path, size_t size) {
    snprintf(path, size, "/.config/wonderswan/%s/config.ini", config_video_name());
}

static void config_mkdirs(void) {
    char path[96];
    f_mkdir("/.config");
    f_mkdir("/.config/wonderswan");
    snprintf(path, sizeof(path), "/.config/wonderswan/%s", config_video_name());
    f_mkdir(path);
}

/* Palette selectors remain uint8_t fields in config v10; the expanded preset
   set fits without changing the config layout or version. */
static uint8_t palette_mode_from_config(uint8_t stored) {
    return stored <= PALETTE_CUSTOM ? stored : PALETTE_DEFAULT;
}

static uint8_t palette_mode_to_config(uint8_t mode) {
    return mode <= PALETTE_CUSTOM ? mode : PALETTE_DEFAULT;
}

static bool load_config(void) {
    /* main() loads config before core1 initializes the video backend, so touch
       only plain state here (no palette/video functions). Missing file and
       unknown keys are non-fatal: whatever is absent keeps its default. */
    if (f_mount(&fs, "", 1) != FR_OK) return false;
    char path[128];
    config_path(path, sizeof(path));
    if (f_open(&file, path, FA_READ) != FR_OK) return false;
    const FSIZE_t fsz = f_size(&file);
    if (fsz == 0 || fsz > 8192) { f_close(&file); return false; }
    char* text = (char*)malloc((size_t)fsz + 1);
    if (!text) { f_close(&file); return false; }
    UINT br = 0;
    const FRESULT fr = f_read(&file, text, (UINT)fsz, &br);
    f_close(&file);
    if (fr != FR_OK) { free(text); return false; }
    text[br] = '\0';

    char* p = text;
    while (*p) {
        char* key = p;
        while (*p && *p != '\n' && *p != '\r' && *p != '=') ++p;
        if (*p != '=') {
            while (*p && *p != '\n' && *p != '\r') ++p;
            while (*p == '\n' || *p == '\r') ++p;
            continue;
        }
        *p++ = '\0';
        char* val = p;
        while (*p && *p != '\n' && *p != '\r') ++p;
        while (*p == '\n' || *p == '\r') { *p = '\0'; ++p; }

        const unsigned long d = strtoul(val, NULL, 10);
        if      (!strcmp(key, "swap_ab"))    swap_ab = d != 0;
        else if (!strcmp(key, "rotation"))   rotation_mode = d <= ROTATION_MANUAL ? (uint8_t)d : ROTATION_AUTO;
        else if (!strcmp(key, "show_fps"))   show_fps = d != 0;
        else if (!strcmp(key, "volume"))     audio_volume = d <= 4 ? (uint8_t)d : 4;
        else if (!strcmp(key, "audio_rate")) audio_rate_shift = d <= 3 ? (uint8_t)d : 0;
        else if (!strcmp(key, "frame_skip")) frame_skip = d <= 3 ? (uint8_t)d : 0;
        else if (!strcmp(key, "demo"))       demo_duration = d < count_of(demo_seconds) ? (uint8_t)d : 0;
        else if (!strcmp(key, "backplane"))  backplane_mode = d <= 1 ? (uint8_t)d : 0;
        else if (!strncmp(key, "palette", 7)) {
            const unsigned layer = (unsigned)strtoul(key + 7, NULL, 10);
            if (layer < PALETTE_LAYER_COUNT) palette_index[layer] = d <= PALETTE_CUSTOM ? (uint8_t)d : PALETTE_DEFAULT;
        }
        else if (!strncmp(key, "shade", 5)) {
            const unsigned idx = (unsigned)strtoul(key + 5, NULL, 10);
            if (idx < 16) global_ws_shades[idx] = (uint32_t)strtoul(val, NULL, 16) & 0x00ffffffu;
        }
#if SOFTTV
        else if (!strcmp(key, "tv_system")) ws_tv_system = d != 0;
        else if (!strcmp(key, "color")) { color_mode = d != 0; tv_out_mode.color_index = color_mode ? 1.0f : 0.0f; }
#endif
    }
    free(text);
    global_palette_valid = true;
    return true;
}
static bool save_config(void) {
    if (f_mount(&fs, "", 1) != FR_OK) return false;
    config_mkdirs();
    static char path[128];
    config_path(path, sizeof(path));
    if (f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    if (!global_palette_valid) init_custom_palette_from_default();

    static char buf[64];
    UINT bw;
    bool ok = true;
    #define WCFG(...) do { const int _n = snprintf(buf, sizeof(buf), __VA_ARGS__); \
        if (_n <= 0 || (unsigned)_n >= sizeof(buf) || f_write(&file, buf, (UINT)_n, &bw) != FR_OK || bw != (UINT)_n) ok = false; } while (0)
    WCFG("swap_ab=%u\n", (unsigned)swap_ab);
    WCFG("rotation=%u\n", (unsigned)rotation_mode);
    WCFG("show_fps=%u\n", (unsigned)show_fps);
    WCFG("volume=%u\n", (unsigned)audio_volume);
    WCFG("audio_rate=%u\n", (unsigned)audio_rate_shift);
    WCFG("frame_skip=%u\n", (unsigned)frame_skip);
    WCFG("demo=%u\n", (unsigned)demo_duration);
    WCFG("backplane=%u\n", (unsigned)backplane_mode);
    for (unsigned i = 0; i < PALETTE_LAYER_COUNT; ++i)
        WCFG("palette%u=%u\n", i, (unsigned)palette_index[i]);
    for (unsigned i = 0; i < 16; ++i)
        WCFG("shade%u=%06lx\n", i, (unsigned long)(global_ws_shades[i] & 0x00ffffffu));
#if SOFTTV
    WCFG("tv_system=%u\n", (unsigned)ws_tv_system);
    WCFG("color=%u\n", color_mode ? 1u : 0u);
#endif
    #undef WCFG
    const FRESULT close_fr = f_close(&file);
    return ok && close_fr == FR_OK;
}

static bool game_palette_ini_path(char *path, size_t size) {
    if (!rom_size || !filename[0]) return false;
    char base[128];
    snprintf(base, sizeof(base), "%s", filename);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = '\0';
    return snprintf(path, size, "/.config/wonderswan/%s.ini", base) > 0;
}

static bool game_palette_exists(void) {
    char path[256];
    FILINFO info;
    return game_palette_ini_path(path, sizeof(path)) && f_stat(path, &info) == FR_OK;
}

static bool game_palette_write(void) {
    char path[256];
    if (!game_palette_ini_path(path, sizeof(path))) return false;
    config_mkdirs();
    if (!global_palette_valid) init_custom_palette_from_default();
    char data[768];
    int len = snprintf(data, sizeof(data),
        "[palette]\r\n"
        "rgb0=%06lX\r\nrgb1=%06lX\r\nrgb2=%06lX\r\nrgb3=%06lX\r\n"
        "rgb4=%06lX\r\nrgb5=%06lX\r\nrgb6=%06lX\r\nrgb7=%06lX\r\n"
        "rgb8=%06lX\r\nrgb9=%06lX\r\nrgb10=%06lX\r\nrgb11=%06lX\r\n"
        "rgb12=%06lX\r\nrgb13=%06lX\r\nrgb14=%06lX\r\nrgb15=%06lX\r\n"
        "[layers]\r\n"
        "back=%u\r\nscreen1=%u\r\nsprites0=%u\r\nscreen2=%u\r\nsprites1=%u\r\n",
        (unsigned long)(global_ws_shades[0] & 0xffffffu), (unsigned long)(global_ws_shades[1] & 0xffffffu),
        (unsigned long)(global_ws_shades[2] & 0xffffffu), (unsigned long)(global_ws_shades[3] & 0xffffffu),
        (unsigned long)(global_ws_shades[4] & 0xffffffu), (unsigned long)(global_ws_shades[5] & 0xffffffu),
        (unsigned long)(global_ws_shades[6] & 0xffffffu), (unsigned long)(global_ws_shades[7] & 0xffffffu),
        (unsigned long)(global_ws_shades[8] & 0xffffffu), (unsigned long)(global_ws_shades[9] & 0xffffffu),
        (unsigned long)(global_ws_shades[10] & 0xffffffu), (unsigned long)(global_ws_shades[11] & 0xffffffu),
        (unsigned long)(global_ws_shades[12] & 0xffffffu), (unsigned long)(global_ws_shades[13] & 0xffffffu),
        (unsigned long)(global_ws_shades[14] & 0xffffffu), (unsigned long)(global_ws_shades[15] & 0xffffffu),
        (unsigned)palette_index[PALETTE_BACK], (unsigned)palette_index[PALETTE_SCREEN0],
        (unsigned)palette_index[PALETTE_SPRITES0], (unsigned)palette_index[PALETTE_SCREEN1],
        (unsigned)palette_index[PALETTE_SPRITES1]);
    if (len <= 0 || (size_t)len >= sizeof(data)) return false;
    if (f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    UINT written = 0;
    const FRESULT fr = f_write(&file, data, (UINT)len, &written);
    const FRESULT close_fr = f_close(&file);
    return fr == FR_OK && written == (UINT)len && close_fr == FR_OK;
}

static bool game_palette_read(void) {
    char path[256];
    if (!game_palette_ini_path(path, sizeof(path))) return false;
    if (f_open(&file, path, FA_READ) != FR_OK) return false;
    char data[768] = {};
    UINT bytes_read = 0;
    const FRESULT fr = f_read(&file, data, sizeof(data) - 1, &bytes_read);
    f_close(&file);
    if (fr != FR_OK || bytes_read == 0) return false;

    unsigned long c[16];
    const int n = sscanf(data,
        "[palette]\r\n"
        "rgb0=%lx\r\nrgb1=%lx\r\nrgb2=%lx\r\nrgb3=%lx\r\n"
        "rgb4=%lx\r\nrgb5=%lx\r\nrgb6=%lx\r\nrgb7=%lx\r\n"
        "rgb8=%lx\r\nrgb9=%lx\r\nrgb10=%lx\r\nrgb11=%lx\r\n"
        "rgb12=%lx\r\nrgb13=%lx\r\nrgb14=%lx\r\nrgb15=%lx",
        &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6], &c[7],
        &c[8], &c[9], &c[10], &c[11], &c[12], &c[13], &c[14], &c[15]);
    if (n != 16) return false;
    for (unsigned i = 0; i < 16; ++i) {
        if (c[i] > 0xfffffful) return false;
        global_ws_shades[i] = (uint32_t)c[i];
    }
    global_palette_valid = true;

    /* New files store only the five preset selectors in addition to the same
       shared 16-colour Custom palette.  Old files have no [layers] section;
       preserve their historical meaning by selecting Custom for every layer. */
    unsigned modes[PALETTE_LAYER_COUNT];
    const char *layers = strstr(data, "[layers]");
    if (layers && sscanf(layers,
            "[layers]\r\nback=%u\r\nscreen1=%u\r\nsprites0=%u\r\nscreen2=%u\r\nsprites1=%u",
            &modes[PALETTE_BACK], &modes[PALETTE_SCREEN0], &modes[PALETTE_SPRITES0],
            &modes[PALETTE_SCREEN1], &modes[PALETTE_SPRITES1]) == PALETTE_LAYER_COUNT) {
        for (unsigned layer = 0; layer < PALETTE_LAYER_COUNT; ++layer)
            palette_index[layer] = modes[layer] <= PALETTE_CUSTOM ? (uint8_t)modes[layer] : PALETTE_DEFAULT;
    } else {
        for (unsigned layer = 0; layer < PALETTE_LAYER_COUNT; ++layer)
            palette_index[layer] = PALETTE_CUSTOM;
    }
    apply_selected_palettes();
    return true;
}

static bool game_palette_action(void) {
    if (!mono_ws_rom_loaded(true)) return false;
    char path[256];
    if (!game_palette_ini_path(path, sizeof(path))) return false;
    if (game_palette_linked) {
        const FRESULT fr = f_unlink(path);
        if (fr == FR_OK || fr == FR_NO_FILE) {
            game_palette_linked = false;
            apply_selected_palettes();
            apply_current_palette_to_video();
        }
    } else if (game_palette_write()) {
        /* The game file owns the current five selectors plus the one shared
           16-colour Custom palette; linking must not force any layer to Custom. */
        game_palette_linked = true;
    }
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
    const int br = (bg >> 16) & 0xff, bgc = (bg >> 8) & 0xff, bb = bg & 0xff;
    uint8_t best = 0;
    unsigned best_distance = 0;

    /* Pick the most distant of the sixteen current colours. This keeps the
       text readable even after colour 0 or 15 has itself been edited. */
    for (unsigned i = 0; i < 16; ++i) {
        const uint32_t fg = ws_shades[i] & 0x00ffffffu;
        const int fr = (fg >> 16) & 0xff, fgc = (fg >> 8) & 0xff, fb = fg & 0xff;
        const unsigned distance = (unsigned)((br-fr)*(br-fr) +
                                             (bgc-fgc)*(bgc-fgc) +
                                             (bb-fb)*(bb-fb));
        if (distance > best_distance) {
            best_distance = distance;
            best = (uint8_t)i;
        }
    }
    return best;
}

static void palette_preview_apply_palette(void) {
    /* Keep the whole preview palette coherent.  In particular, do this on
       every edit step rather than only when leaving the editor: the preview
       framebuffer contains palette indices, so changing ws_shades[] alone
       cannot change the visible swatch. */
    for (unsigned i = 0; i < 16; ++i)
        graphics_set_palette((uint8_t)i, ws_shades[i]);
}

static void palette_preview_draw(uint8_t *buffer, unsigned selected, int edit_channel) {
    /* Mono WS framebuffer colours are the sixteen current shade indices. */
    for (unsigned shade = 0; shade < 16; ++shade) {
        const int x0 = (int)(shade & 3u) * 56;
        const int y0 = (int)(shade >> 2) * 36;
        for (int y = y0; y < y0 + 36; ++y)
            memset(buffer + y * 224 + x0, (int)shade, 56);
    }

    /* Each swatch contains its shade index and the editable RRGGBB value. */
    for (unsigned shade = 0; shade < 16; ++shade) {
        const int x0 = (int)(shade & 3u) * 56;
        const int y0 = (int)(shade >> 2) * 36;
        const uint8_t text = palette_preview_contrast(shade);
        const uint32_t rgb = ws_shades[shade] & 0x00ffffffu;
        palette_preview_hex_digit(buffer, x0 + 13, y0 + 15, shade, text);
        palette_preview_rgb(buffer, x0 + 21, y0 + 15, rgb, text);

        /* A border is the focus indicator in browse mode. */
        if (shade == selected) {
            for (int x = x0 + 1; x < x0 + 55; ++x) {
                buffer[(y0 + 1) * 224 + x] = text;
                buffer[(y0 + 34) * 224 + x] = text;
            }
            for (int y = y0 + 1; y < y0 + 35; ++y) {
                buffer[y * 224 + x0 + 1] = text;
                buffer[y * 224 + x0 + 54] = text;
            }

            /* Editing works at RGB-channel level, not at individual hex
               nibbles.  Underline the selected byte (RR, GG or BB). */
            if (edit_channel >= 0) {
                const int dx = x0 + 21 + edit_channel * 8;
                for (int x = dx; x < dx + 7; ++x)
                    buffer[(y0 + 21) * 224 + x] = text;
            }
        }
    }
}

static bool palette_button_pressed(bool level, bool *armed, uint64_t *released_since) {
    const uint64_t now = time_us_64();
    if (level) {
        *released_since = 0;
        if (*armed) {
            *armed = false;
            return true;
        }
        return false;
    }

    /* Do not re-arm on a short contact bounce.  The button must have been
       continuously released for 50 ms before another press can be accepted. */
    if (*released_since == 0)
        *released_since = now;
    else if (now - *released_since >= 50000)
        *armed = true;
    return false;
}

typedef struct {
    bool active;
    uint64_t next_repeat;
} palette_repeat_t;

static bool palette_repeat(bool level, palette_repeat_t *state) {
    const uint64_t now = time_us_64();
    if (!level) {
        state->active = false;
        state->next_repeat = 0;
        return false;
    }
    if (!state->active) {
        state->active = true;
        state->next_repeat = now + 300000; /* initial key-repeat delay */
        return true;
    }
    if (now >= state->next_repeat) {
        state->next_repeat = now + 80000;  /* controlled repeat rate */
        return true;
    }
    return false;
}

static bool show_current_palettes(bool game_loaded) {
    uint8_t *buffer = (uint8_t *)SCREEN3;
    unsigned selected = 0;
    unsigned source_layer = PALETTE_BACK;
    int edit_channel = -1;
    int hex_digit = -1;

    palette_hex_key = -1;
    palette_tab_requested = false;
    palette_editor_active = true;
    graphics_set_buffer(buffer, 224, 144);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    palette_editor_load_layer(source_layer);
    palette_preview_apply_palette();
    palette_editor_update_title(source_layer);
    palette_preview_draw(buffer, selected, edit_channel);

    /* Editor-local logical controls.  Keyboard works without a gamepad:
       arrows navigate, Enter/X = accept/edit, Esc/Z = back/close, Tab cycles
       the five palette source layers.  Keep physical START as close. */
    bool accept_armed = false, back_armed = false, start_armed = false;
    uint64_t accept_released = 0, back_released = 0, start_released = 0;
    palette_repeat_t rep_left = {}, rep_right = {}, rep_up = {}, rep_down = {};

    for (;;) {
        if (palette_f12_requested) {
            palette_f12_requested = false;
            save_config();
            if (game_loaded)
                apply_selected_palettes();
            palette_editor_active = false;
            palette_hex_key = -1;
            palette_tab_requested = false;
            demo_update_title();
            graphics_set_mode(GRAPHICSMODE_DEFAULT);
            return true;
        }

        if (palette_tab_requested) {
            palette_tab_requested = false;
            source_layer = (source_layer + 1u) % PALETTE_LAYER_COUNT;
            edit_channel = -1;
            hex_digit = -1;
            palette_editor_load_layer(source_layer);
            palette_preview_apply_palette();
            palette_editor_update_title(source_layer);
            palette_preview_draw(buffer, selected, edit_channel);
        }

        const bool left_level  = gamepad1_bits.left;
        const bool right_level = gamepad1_bits.right;
        const bool up_level    = gamepad1_bits.up;
        const bool down_level  = gamepad1_bits.down;
        const bool accept_level = keyboard_bits.a || keyboard_bits.start || gamepad1_bits.a;
        const bool back_level = keyboard_bits.b || keyboard_bits.select || gamepad1_bits.b;
        const bool start_level = gamepad1_bits.start;

        const bool accept = palette_button_pressed(accept_level, &accept_armed, &accept_released);
        const bool back = palette_button_pressed(back_level, &back_armed, &back_released);
        const bool close = palette_button_pressed(start_level, &start_armed, &start_released);
        const bool left = palette_repeat(left_level, &rep_left);
        const bool right = palette_repeat(right_level, &rep_right);
        const bool up = palette_repeat(up_level, &rep_up);
        const bool down = palette_repeat(down_level, &rep_down);

        bool redraw = false;
        bool palette_changed = false;

        const int8_t typed_hex = palette_hex_key;
        if (typed_hex >= 0) {
            palette_hex_key = -1;
            if (hex_digit < 0) hex_digit = 0;
            const unsigned shift = (unsigned)(5 - hex_digit) * 4u;
            ensure_custom_palette_for_edit(source_layer);
            palette_editor_update_title(source_layer);
            uint32_t rgb = ws_shades[selected] & 0x00ffffffu;
            rgb = (rgb & ~(0x0fu << shift)) | ((uint32_t)typed_hex << shift);
            ws_shades[selected] = rgb;
            palette_changed = true;
            redraw = true;

            if (++hex_digit >= 6) {
                hex_digit = -1;
                edit_channel = -1;
            } else {
                edit_channel = hex_digit >> 1;
            }
        } else if (edit_channel < 0) {
            if (left) {
                selected = (selected & ~3u) | ((selected - 1u) & 3u);
                redraw = true;
            } else if (right) {
                selected = (selected & ~3u) | ((selected + 1u) & 3u);
                redraw = true;
            } else if (up) {
                selected = (selected - 4u) & 15u;
                redraw = true;
            } else if (down) {
                selected = (selected + 4u) & 15u;
                redraw = true;
            } else if (accept) {
                edit_channel = 0;
                hex_digit = -1;
                redraw = true;
            } else if (back || close) {
                save_config();
                if (game_loaded)
                    apply_selected_palettes();
                palette_editor_active = false;
                palette_hex_key = -1;
                palette_tab_requested = false;
                demo_update_title();
                graphics_set_mode(GRAPHICSMODE_DEFAULT);
                return true;
            }
        } else {
            /* Inside one swatch, horizontal movement changes the
               field (R/G/B); vertical movement changes its value. */
            if (left) {
                edit_channel = (edit_channel + 2) % 3;
                redraw = true;
            } else if (right) {
                edit_channel = (edit_channel + 1) % 3;
                redraw = true;
            } else if (up || down) {
                const unsigned shift = (unsigned)(2 - edit_channel) * 8u;
                ensure_custom_palette_for_edit(source_layer);
                palette_editor_update_title(source_layer);
                uint32_t rgb = ws_shades[selected] & 0x00ffffffu;
                unsigned value = (rgb >> shift) & 0xffu;
                if (up)
                    value = value == 255u ? 255u : value + 1u;
                else
                    value = value == 0u ? 0u : value - 1u;
                rgb = (rgb & ~(0xffu << shift)) | (value << shift);
                ws_shades[selected] = rgb;
                palette_changed = true;
                redraw = true;
            } else if (back || accept) {
                /* Both Enter/A and Esc/B finish editing, but because they are
                   edge-triggered a held/bouncing contact cannot immediately
                   toggle the mode a second time. */
                edit_channel = -1;
                hex_digit = -1;
                redraw = true;
            } else if (close) {
                save_config();
                if (game_loaded)
                    apply_selected_palettes();
                palette_editor_active = false;
                palette_hex_key = -1;
                palette_tab_requested = false;
                demo_update_title();
                graphics_set_mode(GRAPHICSMODE_DEFAULT);
                return true;
            }
        }

        if (palette_changed) {
            /* Custom is one shared editable palette.  Once the selected layer
               has switched to Custom, keep its canonical RGB array in sync. */
            capture_global_palette();
            palette_preview_apply_palette();
        }
        if (redraw)
            palette_preview_draw(buffer, selected, edit_channel);

        sleep_ms(10);
    }
}

const MenuItem menu_items[] = {
        { "Swap AB <> BA: %s", ARRAY, &swap_ab, nullptr, 1, { "NO ", "YES" }},
        { "Screen rotation: %s", ARRAY, &rotation_mode, nullptr, 3, { "Auto     ", "Landscape", "Portrait ", "Manual   " }},
        { "FPS overlay: %s", ARRAY, &show_fps, nullptr, 1, { "OFF", "ON " }},
        { "Volume: %s", ARRAY, &audio_volume, &apply_audio_volume, 4, { "Mute", "12% ", "25% ", "50% ", "100%" }},
        { "Emulate Sound: %s", ARRAY, &audio_rate_shift, &apply_audio_rate, 3, { "24 kHz", "12 kHz", "6 kHz ", "3 kHz " }},
        { "Frame skip: %s", ARRAY, &frame_skip, nullptr, 3, { "75 Hz", "50 Hz", "25 Hz", "Auto " }},
        { "Back:      %s", ARRAY, &palette_index[PALETTE_BACK],     nullptr, 17, { "Default ", "Red     ", "Orange  ", "Green   ", "Blue    ", "Purple  ", "OGBP    ", "OGBR    ", "GBPR    ", "GBPO    ", "BPRO    ", "BROG    ", "PROG    ", "POGR    ", "ROGB    ", "ROGP    ", "Random  ", "Custom  " }},
        { "Screen 0:  %s", ARRAY, &palette_index[PALETTE_SCREEN0],  nullptr, 17, { "Default ", "Red     ", "Orange  ", "Green   ", "Blue    ", "Purple  ", "OGBP    ", "OGBR    ", "GBPR    ", "GBPO    ", "BPRO    ", "BROG    ", "PROG    ", "POGR    ", "ROGB    ", "ROGP    ", "Random  ", "Custom  " }},
        { "Sprites 0: %s", ARRAY, &palette_index[PALETTE_SPRITES0], nullptr, 17, { "Default ", "Red     ", "Orange  ", "Green   ", "Blue    ", "Purple  ", "OGBP    ", "OGBR    ", "GBPR    ", "GBPO    ", "BPRO    ", "BROG    ", "PROG    ", "POGR    ", "ROGB    ", "ROGP    ", "Random  ", "Custom  " }},
        { "Screen 1:  %s", ARRAY, &palette_index[PALETTE_SCREEN1],  nullptr, 17, { "Default ", "Red     ", "Orange  ", "Green   ", "Blue    ", "Purple  ", "OGBP    ", "OGBR    ", "GBPR    ", "GBPO    ", "BPRO    ", "BROG    ", "PROG    ", "POGR    ", "ROGB    ", "ROGP    ", "Random  ", "Custom  " }},
        { "Sprites 1: %s", ARRAY, &palette_index[PALETTE_SPRITES1], nullptr, 17, { "Default ", "Red     ", "Orange  ", "Green   ", "Blue    ", "Purple  ", "OGBP    ", "OGBR    ", "GBPR    ", "GBPO    ", "BPRO    ", "BROG    ", "PROG    ", "POGR    ", "ROGB    ", "ROGP    ", "Random  ", "Custom  " }},
#ifdef VGA
        { "Backplane: %s", ARRAY, &backplane_mode, nullptr, 1, { "On ", "Off" }},
#else
        { "Backplane: %s", ARRAY, &backplane_mode, nullptr, 1, { "Auto", "Off " }},
#endif
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
        { "TV system: %s", ARRAY, &ws_tv_system, &apply_tv_system, 1, { "PAL ", "NTSC" } },
        { "Colors: %s", ARRAY, &color_mode, &toggle_color, 1, { "NO ", "YES" } },
#endif
        {
                "Overclocking: %s MHz", ARRAY, &frequency_index, &overclock, count_of(frequencies) - 1,
                { "378", "396", "404", "408", "412", "416", "420", "424", "432", "444", "460", "504", "524", "528" }
        },
#if PICO_RP2350
        { "Voltage: %s", ARRAY, &voltage_index, &overclock, 4, { "Auto ", "1.50V", "1.60V", "1.65V", "1.70V" } },
#endif
        { "Demo game time: %s", ARRAY, &demo_duration, nullptr, 7, { "15 sec", "30 sec", "45 sec", "1 min ", "2 min ", "3 min ", "5 min ", "10 min" } },
        { "Press START / Enter to apply", NONE },
        { "Start Demo", START_DEMO },
        { "Current palettes", SHOW_PALETTES },
        { "Save colors for this game", GAME_PALETTE, nullptr, &game_palette_action },
        { "Default", DEFAULTS },
        { "Reset to ROM select", ROM_SELECT },
        { "Return to game", RETURN }
};
#define MENU_ITEMS_NUMBER (sizeof(menu_items) / sizeof (MenuItem))

static bool menu_item_selectable(uint index, bool game_loaded) {
    const menu_type_e type = menu_items[index].type;
    return type != NONE &&
           (type != RETURN || game_loaded) &&
           (type != SHOW_PALETTES || mono_ws_rom_loaded(game_loaded)) &&
           (type != GAME_PALETTE || mono_ws_rom_loaded(game_loaded));
}

static bool reset_config_and_offer_reboot(void) {
    if (f_mount(&fs, "", 1) != FR_OK)
        return false;

    char cfgp[128];
    config_path(cfgp, sizeof(cfgp));
    const FRESULT fr = f_unlink(cfgp);
    if (fr != FR_OK && fr != FR_NO_FILE)
        return false;

    /* Wait for the key that activated Default to be released before asking. */
    while (gamepad1_bits.start)
        sleep_ms(10);

    static const char message[] = "Config deleted. Reboot now?";
    static const char controls[] = "START/Enter = Yes   B/Esc = No";
    const uint32_t dialog_width = sizeof(controls) + 1; /* 30 chars + 2 borders. */
    const uint32_t dialog_x = (TEXTMODE_COLS - dialog_width) / 2;
    const uint32_t dialog_y = TEXTMODE_ROWS / 2 - 2;

    draw_window("Default", dialog_x, dialog_y, dialog_width, 5);
    draw_text(message, dialog_x + 1 + (dialog_width - 2 - (sizeof(message) - 1)) / 2, dialog_y + 1, 15, 1);
    draw_text(controls, dialog_x + 1, dialog_y + 3, 15, 1);

    for (;;) {
        if (gamepad1_bits.start) {
            *(uint32_t *)0x400d000c = 0x60007204;
            set_sys_clock_khz(150000, false);
            sleep_ms(100);
            vreg_set_voltage(VREG_VOLTAGE_1_10);
            watchdog_reboot(0, 0, 0);
            while (true)
                tight_loop_contents();
        }
        if (gamepad1_bits.b || (gamepad1_bits.select && !gamepad1_bits.start))
            return true;
        sleep_ms(10);
    }
}

static void shuffle_palettes(bool persist, bool game_loaded = true) {
    for (unsigned layer = 0; layer < PALETTE_LAYER_COUNT; ++layer)
        palette_index[layer] = (uint8_t)(random_palette_next() % (PALETTE_CUSTOM + 1));
    if (game_loaded)
        apply_selected_palettes();
    if (persist)
        save_config();
}

static bool service_hotkeys(bool game_loaded) {
    const int8_t one = palette_layer_cycle_requested;
    if (one) {
        palette_layer_cycle_requested = 0;
        const unsigned layer = (unsigned)((one < 0 ? -one : one) - 1);
        if (layer < PALETTE_LAYER_COUNT) {
            palette_index[layer] = one < 0 ? (palette_index[layer] + PALETTE_CUSTOM) % (PALETTE_CUSTOM + 1) : (palette_index[layer] + 1) % (PALETTE_CUSTOM + 1);
            if (game_loaded) apply_selected_palettes();
            save_config();
            if (game_loaded) show_palette_overlay(layer);
        }
    }
    if (palette_shuffle_requested) {
        palette_shuffle_requested = false;
        shuffle_palettes(true, game_loaded);
    }
    if (palette_shuffle_requested_no_save) {
        palette_shuffle_requested_no_save = false;
        shuffle_palettes(false, game_loaded);
    }
    if (game_palette_save_requested) {
        game_palette_save_requested = false;
        if (mono_ws_rom_loaded(game_loaded) && game_palette_write())
            game_palette_linked = true;
    }
    const int8_t all = palette_all_set_requested;
    if (all >= 0) {
        palette_all_set_requested = -1;
        const uint8_t mode = all == 0 ? PALETTE_DEFAULT : all == 1 ? PALETTE_RANDOM : PALETTE_CUSTOM;
        if (mode == PALETTE_RANDOM) generate_random_palette();
        for (unsigned layer = 0; layer < PALETTE_LAYER_COUNT; ++layer) palette_index[layer] = mode;
        if (game_loaded) apply_selected_palettes();
        save_config();
    }
    if (backplane_toggle_requested) { backplane_toggle_requested = false; backplane_mode ^= 1u; save_config(); }
    bool palette_editor_opened = false;
    if (palette_f12_requested) {
        palette_f12_requested = false;
        show_current_palettes(game_loaded);
        palette_editor_opened = true;
    }
    if (fxPressedV) {
        const uint8_t slot = fxPressedV;
        fxPressedV = 0; /* Never defer a game-state hotkey until a ROM appears. */
        if (game_loaded) {
            demo_stop();
            save_slot = slot;
            if (altPressed) load(); else if (ctrlPressed) save();
        }
    }
    return palette_editor_opened;
}

static void menu(bool game_loaded) {
#ifdef HWAY
    ws_audio_hway_silence();
#endif
    bool exit = false;
    bool suppress_config_save = false;
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
        if (service_hotkeys(game_loaded)) {
            /* F12 may enter the palette editor directly while this menu is
               active.  The editor leaves graphics in framebuffer mode; put
               the menu back into text mode instead of returning to whatever
               was displayed before the menu (notably the ROM browser). */
            graphics_set_mode(TEXTMODE_DEFAULT);
        }
        /* F10 leaves the modal menu so main() can open the browser while the
           current emulation core remains alive. */
        if (reboot || (game_loaded && filebrowser_direct_requested))
            return;
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
                            else if (changed && (item->value == &palette_index[0] || item->value == &palette_index[1] || item->value == &palette_index[2] || item->value == &palette_index[3] || item->value == &palette_index[4])) {
                                if (game_loaded) apply_selected_palettes();
                            }
                        }
                        break;
                    case RETURN:
                        if (game_loaded && gamepad1_bits.start)
                            exit = true;
                        break;

                    case SHOW_PALETTES:
                        if (gamepad1_bits.start && mono_ws_rom_loaded(game_loaded)) {
                            if (show_current_palettes(game_loaded)) {
                                save_config();
                                return;
                            }
                        }
                        break;

                    case GAME_PALETTE:
                        break;

                    case START_DEMO:
                        if (gamepad1_bits.start) {
                            demo_requested = true;
                            if (game_loaded)
                                reboot = true;
                            save_config();
                            return;
                        }
                        break;

                    case DEFAULTS:
                        if (gamepad1_bits.start) {
                            /* Do not let the normal menu epilogue recreate the
                               config file after Default has deleted it. */
                            suppress_config_save = reset_config_and_offer_reboot();
                            if (suppress_config_save)
                                exit = true;
                        }
                        break;

                    case ROM_SELECT:
                        if (gamepad1_bits.start) {
                            demo_stop();
                            if (game_loaded)
                                reboot = true;
                            save_config();
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
                case GAME_PALETTE:
                    snprintf(result, TEXTMODE_COLS, "%s", game_palette_linked ? "Unlink game color file" : "Save colors for this game");
                    break;
                case NONE:
                    color = 6;
                default:
                    snprintf(result, TEXTMODE_COLS, "%s", item->text);
            }
            if ((!game_loaded && item->type == RETURN) ||
                (item->type == SHOW_PALETTES && !mono_ws_rom_loaded(game_loaded)) ||
                (item->type == GAME_PALETTE && !mono_ws_rom_loaded(game_loaded))) {
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

    if (!suppress_config_save)
        save_config();

    /* Palette selectors are valid configuration even before a ROM exists.
       Only push them into the emulated WS GPU after ws_init() has run. */
    if (game_loaded) {
        if (!game_palette_linked)
            apply_selected_palette();
        else
            apply_current_palette_to_video();
        graphics_set_mode(GRAPHICSMODE_DEFAULT);
        ws_gpu_refresh_palette();
    } else {
        graphics_set_mode(TEXTMODE_DEFAULT);
    }

}

/* Renderer loop on Pico's second core */
void __time_critical_func(render_core)() {
    flash_safe_execute_core_init();

#ifndef HWAY
    i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = 256;
    i2s_volume(&i2s_config, 0);
    i2s_init(&i2s_config);
#endif
    apply_audio_volume();
    apply_audio_rate();

    ps2kbd.init_gpio();
    usbhid_init(process_kbd_report);
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);

#if SOFTTV
    tv_apply_system();   /* PAL/NTSC timing from config, before the TV PIO starts */
#endif
    graphics_init();

#ifdef HWAY
    hway_init();
    ws_audio_hway_sync();
#endif

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
#ifdef HWAY
        /* Physical HWAY writes belong to core1.  Core0 only publishes the
         * newest PCM byte, so audio output cannot stall emulation. */
        hway_poll();
#endif

        if (tick >= last_frame_tick + frame_tick) {
#ifdef TFT
            refresh_lcd();
#endif
            ps2kbd.tick();
            nespad_tick();

            last_frame_tick = tick;
        }

        tick = time_us_64();

        usbhid_task();
        tight_loop_contents();
    }

    __unreachable();
}

int frame;
bool PSRAM_AVAILABLE = true;

int main() {
    overclock();

    /* Persistent config contains only emulator/UI settings. Clock and voltage
       are deliberately runtime-only, so loading a config can never alter the
       bootstrap clock before core1 starts. */
    if (f_mount(&fs, "", 1) == FR_OK)
        load_config();

//    stdio_init_all();

    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);

    /* Once input is alive, held aggregate SELECT removes the global config
       and reboots. Per-game INI files are left untouched. */
    while (!runtime_drivers_ready)
        tight_loop_contents();
    sleep_ms(200);
    if (gamepad1_bits.select) {
        draw_text("Reboot!", 0, 0, 12, 0);
        if (f_mount(&fs, "", 1) == FR_OK) {
            char cfgp[128];
            config_path(cfgp, sizeof(cfgp));
            f_unlink(cfgp);
        }
        *(uint32_t *)0x400d000c = 0x60007204;
        set_sys_clock_khz(150000, false);
        sleep_ms(100);
        vreg_set_voltage(VREG_VOLTAGE_1_10);
        watchdog_reboot(0, 0, 0);
        while (true)
            tight_loop_contents();
    }

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

    bool need_browser = true;
    bool rom_selected_from_live_browser = false;
    while (true) {
        if (need_browser) {
            graphics_set_mode(TEXTMODE_DEFAULT);
            /* The browser owns a manual/non-Demo state. A Demo request made
               inside it is the only path allowed to re-enter Demo mode. */
            demo_stop();
            rom_size = 0;
            filebrowser(HOME_DIR, "ws,wsc");

            if (demo_requested) {
                demo_requested = false;
                demo_active = true;
                demo_current_name[0] = '\0';
                if (!demo_load_next_rom(nullptr)) {
                    demo_stop();
                    continue;
                }
            } else if (rom_size == 0) {
                continue;
            }
            need_browser = false;
        }

        if (!ws_init((uint8_t *)rom, rom_size)) {
            graphics_set_mode(TEXTMODE_DEFAULT);
            draw_text("ERROR: not enough RAM for cartridge save memory!", 0, 0, 13, 0);
            sleep_ms(demo_active ? 1500 : 5000);
            if (demo_active && demo_load_next_rom(demo_current_name))
                continue;
            demo_stop();
            need_browser = true;
            continue;
        }
        if (filename[strlen(filename)-1]=='c'|| filename[strlen(filename)-1]=='C') {
            ws_set_system(WS_SYSTEM_COLOR);
        } else {
            ws_set_system(WS_SYSTEM_MONO);
        }

        /* Random is generated afresh for every ROM start and is never persisted. */
        generate_random_palette();

        /* Start from the selected persistent palette.  A per-game INI is
           loaded into Custom and selects Custom automatically. */
        apply_selected_palette();
        game_palette_linked = mono_ws_rom_loaded(true) && game_palette_exists();
        if (game_palette_linked && !game_palette_read()) {
            game_palette_linked = false;
            apply_selected_palette();
        }
        if (demo_active && mono_ws_rom_loaded(true))
            shuffle_palettes(false);
        ws_reset();
        apply_current_palette_to_video();
#ifdef HDMI
        if (mono_ws_rom_loaded(true)) {
            const uint32_t *backplane_palette = portrait_enabled()
                ? ws_backplane_hdmi_portrait_palette
                : ws_backplane_palette;
            for (unsigned i = 0; i < 144; ++i)
                graphics_set_palette(ws_backplane_palette_slots[i], backplane_palette[i]);
        }
#endif

        graphics_set_mode(GRAPHICSMODE_DEFAULT);
        demo_update_title();

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
        uint8_t  fs_phase = 0;      // frame-skip phase counter (mod 3)
        bool     fs_behind = false; // Auto: did the previous frame overrun its budget
        while (!reboot) {
            if (service_hotkeys(true)) {
                /* Do not count time spent in the palette editor as slowdown. */
                fps_started = time_us_64();
                fps_frames = 0;
                graphics_set_fps_overlay(show_fps, 0);
            }
            if (filebrowser_direct_requested) {
                filebrowser_direct_requested = false;
                demo_stop();
                save_config();
#ifdef HWAY
                ws_audio_hway_silence();
#endif
                filebrowser_return_requested = false;
                filebrowser_resumed_game = false;
                filebrowser_can_return_to_game = true;
                graphics_set_mode(TEXTMODE_DEFAULT);
                filebrowser(HOME_DIR, "ws,wsc");
                filebrowser_can_return_to_game = false;

                if (filebrowser_resumed_game) {
                    filebrowser_resumed_game = false;
                    /* F10 -> browser -> F10: resume the same live core. */
                    graphics_set_mode(GRAPHICSMODE_DEFAULT);
                    apply_current_palette_to_video();
                    fps_started = time_us_64();
                    fps_frames = 0;
                    graphics_set_fps_overlay(show_fps, 0);
#if defined(VGA) || defined(HDMI)
                    next_ws_frame = time_us_64() + 13250;
#endif
                    continue;
                }

                /* A ROM was explicitly selected in the browser. Its image is
                   already loaded; end the old core and start that cartridge. */
                rom_selected_from_live_browser = true;
                reboot = true;
                continue;
            }

            update_palette_overlay();

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

            if (demo_active) {
                if (graphics_demo_overlay_enabled &&
                    time_us_64() - demo_game_started_at >= 10000000ull)
                    graphics_set_demo_overlay(false, nullptr);
                const uint8_t di = demo_duration < count_of(demo_seconds) ? demo_duration : 0;
                if (time_us_64() - demo_game_started_at >= (uint64_t)demo_seconds[di] * 1000000ull) {
                    demo_advance_pending = true;
                    reboot = true;
                    continue;
                }
            }

            // WonderSwan has no Select button. NES Select / keyboard
            // Backspace is an emulator hotkey that flips the presentation.
            if (!gamepad1_bits.start && gamepad1_bits.select && !select_pressed_last_frame)
                rotation_hotkey_override = !rotation_hotkey_override;
            select_pressed_last_frame = gamepad1_bits.select;

            portrait = portrait_enabled();

            const bool move_up    = gamepad1_bits.up;
            const bool move_right = gamepad1_bits.right;
            const bool move_down  = gamepad1_bits.down;
            const bool move_left  = gamepad1_bits.left;

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
#ifdef VGA
            // VGA backplane uses independent preconverted RGB222 data, so
            // On applies to both monochrome .ws and color .wsc cartridges.
            ws_backplane_enabled = (backplane_mode == 0);
            ws_backplane_portrait = portrait;
#else
            ws_backplane_enabled = (backplane_mode == 0) && mono_ws_rom_loaded(true);
            ws_backplane_portrait = portrait;
#endif
            graphics_overlay_palette_index = ws_backplane_enabled ? 15 : 0;
            graphics_set_offset(portrait ? 88 : 48, portrait ? 8 : 48);

            // Portrait mode renders the native 224x144 frame into SCREEN1, then
            // rotates it to a 144x224 presentation buffer.
            if (portrait)
                buffer = (uint8_t*)SCREEN1;

            /* Emulate every frame (CPU + audio stay at 75 Hz so the game runs at
               the right speed and the audio ring never starves); only skip the
               visual render/present. 75/50/25 Hz = render 3/2/1 of every 3.
               Auto drops the picture only while behind, with a 25 Hz floor so it
               never freezes. */
            bool do_render;
            switch (frame_skip) {
                case 1:  do_render = (fs_phase != 2);                break; // 50 Hz
                case 2:  do_render = (fs_phase == 0);                break; // 25 Hz
                case 3:  do_render = (!fs_behind) || (fs_phase == 0);break; // Auto
                default: do_render = true;                          break; // 75 Hz
            }
            fs_phase = (fs_phase >= 2) ? 0 : (uint8_t)(fs_phase + 1);

            while(!ws_executeLine(buffer, do_render ? 1 : 0)) ;
            if (do_render) {
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
            if (portrait) {
                buffer = (uint8_t*)SCREEN1;
            } else if (do_render) {
                /* Pick a free buffer for the next rendered frame. On a skipped
                   frame we keep the current one (never presented, so still free). */
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
            }

            fs_behind = ((int64_t)(time_us_64() - next_ws_frame) >= 0);
            while ((int64_t)(time_us_64() - next_ws_frame) < 0) {
#ifndef HWAY
                i2s_dma_pump(&i2s_config);   // feed audio DMA while pacing the frame
#else
                tight_loop_contents();
#endif
            }
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

        ws_done();
        reboot = false;

        if (demo_requested) {
            demo_requested = false;
            demo_active = true;
            demo_advance_pending = false;
            demo_current_name[0] = '\0';
            if (demo_load_next_rom(nullptr))
                continue;
            demo_stop();
        } else if (demo_active && demo_advance_pending) {
            demo_advance_pending = false;
            if (demo_load_next_rom(demo_current_name))
                continue;
            demo_stop();
        } else if (demo_active) {
            /* Any other path out of emulation is a manual exit. */
            demo_stop();
        }

        /* Never let stale Demo state leak into the browser. */
        demo_stop();
        if (rom_selected_from_live_browser) {
            rom_selected_from_live_browser = false;
            need_browser = false;
        } else {
            need_browser = true;
        }
    }
    __unreachable();
}
