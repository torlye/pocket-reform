/*
 * Forked from stdio_usb. Provides USB setup for Pocket Reform sysctl.
 *
 * This file is based on a file originally part of the
 * MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2019 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "tusb.h"
#include "reform_stdio_usb.h"
#include "pico/unique_id.h"
#include "mntre_reset_priv.h"

#define USB_VID_PIDCODES     0x1209
#define USB_VID_RASPBERRYPI  0x2E8A
#define USB_PID_MNT_POCKET_REFORM_SYSCTL_10   0x6D07
#define USB_PID_RASPBERRYPI_PICO_SDK_CDC      0x000A

#define USBD_MANUFACTURER "MNT"

#if 1

#define USBD_VID USB_VID_PIDCODES
#define USBD_PID USB_PID_MNT_POCKET_REFORM_SYSCTL_10
#define USBD_PRODUCT "Pocket Reform System Controller 1.0"

#else

/* useful for debug builds */

#define USBD_VID USB_VID_RASPBERRYPI
#define USBD_PID USB_PID_RASPBERRYPI_PICO_SDK_CDC
#define USBD_PRODUCT "pocket-reform-sysctl-debug"

#endif


#define USBD_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + MNTRE_RESET_TUD_DESC_LEN)
#if !PICO_STDIO_USB_DEVICE_SELF_POWERED
#define USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE (0)
#define USBD_MAX_POWER_MA (250)
#else
#define USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE TUSB_DESC_CONFIG_ATT_SELF_POWERED
#define USBD_MAX_POWER_MA (1)
#endif

#define USBD_ITF_CDC         (0) // needs 2 interfaces
#define USBD_ITF_MNTRE_RESET (2)
#define USBD_ITF_MAX         (3)

#define USBD_CDC_EP_CMD (0x81)
#define USBD_CDC_EP_OUT (0x02)
#define USBD_CDC_EP_IN (0x82)
#define USBD_CDC_CMD_MAX_SIZE (8)
#define USBD_CDC_IN_OUT_MAX_SIZE (64)

#define USBD_STR_0 (0x00)
#define USBD_STR_MANUF (0x01)
#define USBD_STR_PRODUCT (0x02)
#define USBD_STR_SERIAL (0x03)
#define USBD_STR_CDC (0x04)
#define USBD_STR_MNTRE_RESET (0x05)

// Note: descriptors returned from callbacks must exist long enough for transfer to complete

static const tusb_desc_device_t usbd_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,  // needed to export a BOS descriptor
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0001,
    .iManufacturer = USBD_STR_MANUF,
    .iProduct = USBD_STR_PRODUCT,
    .iSerialNumber = USBD_STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t usbd_desc_cfg[USBD_DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, USBD_ITF_MAX, USBD_STR_0, USBD_DESC_LEN,
        USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE, USBD_MAX_POWER_MA),

    TUD_CDC_DESCRIPTOR(USBD_ITF_CDC, USBD_STR_CDC, USBD_CDC_EP_CMD,
        USBD_CDC_CMD_MAX_SIZE, USBD_CDC_EP_OUT, USBD_CDC_EP_IN, USBD_CDC_IN_OUT_MAX_SIZE),

    MNTRE_RESET_TUD_DESCRIPTOR(USBD_ITF_MNTRE_RESET, USBD_STR_MNTRE_RESET)
};

static char usbd_serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const usbd_desc_str[] = {
    [USBD_STR_MANUF] = USBD_MANUFACTURER,
    [USBD_STR_PRODUCT] = USBD_PRODUCT,
    [USBD_STR_SERIAL] = usbd_serial_str,
    [USBD_STR_CDC] = "Board CDC",
    [USBD_STR_MNTRE_RESET] = MNTRE_RESET_INTERFACE_NAME_STR,
};

//------------- DS-20 (fwupd) -------------//
static const uint8_t desc_ds20[] = {
#include "ds20-descriptor.h"
};

#define TUD_BOS_DS_20_DESC_LEN   28

#define DS_20_DESC_LEN  (sizeof(desc_ds20))
#define BOS_TOTAL_LEN      (TUD_BOS_DESC_LEN + TUD_BOS_DS_20_DESC_LEN)

#define TUD_BOS_DS20_UUID   \
    0x63, 0xec, 0x0a, 0x01, 0x74, 0xf5, 0xcd, 0x52, \
    0x9d, 0xda, 0x28, 0x52, 0x55, 0x0d, 0x94, 0xf0

#define TUD_BOS_DS20_DESCRIPTOR(_desc_set_len, _vendor_code) \
    TUD_BOS_PLATFORM_DESCRIPTOR(TUD_BOS_DS20_UUID, U32_TO_U8S_LE(0x0001090e), U16_TO_U8S_LE(_desc_set_len), _vendor_code, 0)

uint8_t const desc_bos[] = {
    // total length, number of device caps
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),

    // DS-20, as used in fwupd
    TUD_BOS_DS20_DESCRIPTOR(DS_20_DESC_LEN, 0x42)
};

const uint8_t *tud_descriptor_bos_cb(void) {
    return desc_bos;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request) {
    // nothing to with DATA & ACK stage
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bRequest == 0x42 && request->wIndex == 7) {
        // Get DS-20 descriptor
        return tud_control_xfer(rhport, request, (void*)(uintptr_t) desc_ds20, sizeof(desc_ds20));
    }

    // stall unknown request
    return false;
}

//------------- DS-20 (fwupd) -------------//

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&usbd_desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(__unused uint8_t index) {
    return usbd_desc_cfg;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, __unused uint16_t langid) {
#ifndef USBD_DESC_STR_MAX
#define USBD_DESC_STR_MAX (40)
#elif USBD_DESC_STR_MAX > 127
#error USBD_DESC_STR_MAX too high (max is 127).
#elif USBD_DESC_STR_MAX < 17
#error USBD_DESC_STR_MAX too low (min is 17).
#endif
    static uint16_t desc_str[USBD_DESC_STR_MAX];

    // Assign the SN using the unique flash id
    if (!usbd_serial_str[0]) {
        pico_get_unique_board_id_string(usbd_serial_str, sizeof(usbd_serial_str));
    }

    uint8_t len;
    if (index == 0) {
        desc_str[1] = 0x0409; // supported language is English
        len = 1;
    } else {
        if (index >= sizeof(usbd_desc_str) / sizeof(usbd_desc_str[0])) {
            return NULL;
        }
        const char *str = usbd_desc_str[index];
        for (len = 0; len < USBD_DESC_STR_MAX - 1 && str[len]; ++len) {
            desc_str[1 + len] = str[len];
        }
    }

    // first byte is length (including header), second byte is string type
    desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * len + 2));

    return desc_str;
}
