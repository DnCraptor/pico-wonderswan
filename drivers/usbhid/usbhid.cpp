#include "usbhid.h"
#include "tusb.h"
#include "nespad.h"
#include <string.h>

#define MAX_REPORT 4
static struct {
    uint8_t count;
    tuh_hid_report_info_t info[MAX_REPORT];
} hid_info[CFG_TUH_HID];
static hid_keyboard_report_t prev_keyboard;
static usbhid_keyboard_cb_t keyboard_callback;
static volatile uint32_t gamepad_state;

static uint32_t decode_common_gamepad(uint8_t const *p, uint16_t n) {
    if (!p || n < 2) return 0;
    uint32_t s = 0;
    /* Common DirectInput/SNES-clone layout: X,Y first, buttons around byte 5. */
    uint8_t x = p[0], y = p[1];
    if (x < 0x40) s |= DPAD_LEFT;
    if (x > 0xc0) s |= DPAD_RIGHT;
    if (y < 0x40) s |= DPAD_UP;
    if (y > 0xc0) s |= DPAD_DOWN;
    if (n > 5) {
        if (p[5] & 0x20) s |= DPAD_A;
        if (p[5] & 0x40) s |= DPAD_B;
        if (p[5] & 0x10) s |= DPAD_SELECT;
        if (p[5] & 0x80) s |= DPAD_START;
    } else {
        uint8_t b = p[n - 1];
        if (b & 0x01) s |= DPAD_A;
        if (b & 0x02) s |= DPAD_B;
        if (b & 0x04) s |= DPAD_SELECT;
        if (b & 0x08) s |= DPAD_START;
    }
    return s;
}

static void process_generic(uint8_t instance, uint8_t const *report, uint16_t len) {
    if (instance >= CFG_TUH_HID || !report || !len) return;
    tuh_hid_report_info_t *ri = nullptr;
    uint8_t count = hid_info[instance].count;
    if (count == 1 && hid_info[instance].info[0].report_id == 0) {
        ri = &hid_info[instance].info[0];
    } else {
        uint8_t id = report[0];
        for (uint8_t i = 0; i < count; ++i)
            if (hid_info[instance].info[i].report_id == id) { ri = &hid_info[instance].info[i]; break; }
        if (!ri) return;
        ++report; --len;
    }
    if (ri->usage_page != HID_USAGE_PAGE_DESKTOP) return;
    if (ri->usage == HID_USAGE_DESKTOP_KEYBOARD && len >= sizeof(hid_keyboard_report_t)) {
        auto const *kbd = reinterpret_cast<hid_keyboard_report_t const *>(report);
        if (keyboard_callback) keyboard_callback(kbd, &prev_keyboard);
        prev_keyboard = *kbd;
    } else if (ri->usage == HID_USAGE_DESKTOP_GAMEPAD || ri->usage == HID_USAGE_DESKTOP_JOYSTICK) {
        gamepad_state = decode_common_gamepad(report, len);
    }
}

extern "C" void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                                  uint8_t const *desc_report, uint16_t desc_len) {
    if (instance < CFG_TUH_HID) {
        memset(&hid_info[instance], 0, sizeof(hid_info[instance]));
        if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_NONE)
            hid_info[instance].count = tuh_hid_parse_report_descriptor(
                hid_info[instance].info, MAX_REPORT, desc_report, desc_len);
    }
    tuh_hid_receive_report(dev_addr, instance);
}

extern "C" void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr;
    if (instance < CFG_TUH_HID) memset(&hid_info[instance], 0, sizeof(hid_info[instance]));
    gamepad_state = 0;
}

extern "C" void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                            uint8_t const *report, uint16_t len) {
    uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
    if (proto == HID_ITF_PROTOCOL_KEYBOARD && len >= sizeof(hid_keyboard_report_t)) {
        auto const *kbd = reinterpret_cast<hid_keyboard_report_t const *>(report);
        if (keyboard_callback) keyboard_callback(kbd, &prev_keyboard);
        prev_keyboard = *kbd;
    } else if (proto == HID_ITF_PROTOCOL_NONE) {
        process_generic(instance, report, len);
    }
    tuh_hid_receive_report(dev_addr, instance);
}

void usbhid_init(usbhid_keyboard_cb_t cb) {
    keyboard_callback = cb;
    memset(&prev_keyboard, 0, sizeof(prev_keyboard));
    memset(hid_info, 0, sizeof(hid_info));
    gamepad_state = 0;
    tuh_init(BOARD_TUH_RHPORT);
}

void usbhid_task(void) { tuh_task(); }
uint32_t usbhid_gamepad_state(void) { return gamepad_state; }
