#pragma once
#include <stdint.h>
#include "class/hid/hid.h"

typedef void (*usbhid_keyboard_cb_t)(hid_keyboard_report_t const *report,
                                     hid_keyboard_report_t const *prev_report);

void usbhid_init(usbhid_keyboard_cb_t keyboard_cb);
void usbhid_task(void);
uint32_t usbhid_gamepad_state(void);
