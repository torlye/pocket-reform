/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
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
 *
 */

#include "tusb.h"
#include "device/usbd_pvt.h"
#include "mntre_usbids.h"
#include "usb_bos.h"
#include "usb_descriptors.h"
#include "pico/unique_id.h"
#include "mntre_reset_priv.h"

/* A combination of interfaces must have a unique product id, since PC will save device driver after the first plug.
 * Same VID/PID with different interface e.g MSC (first), then CDC (later) will possibly cause system error on PC.
 *
 * Auto ProductID layout's Bitmap:
 *   [MSB]         HID | MSC | CDC          [LSB]
 */
#define _PID_MAP(itf, n)  ( (CFG_TUD_##itf) << (n) )

#define USBD_VID USB_VID_PIDCODES
#define USBD_PID USB_PID_MNT_POCKET_REFORM_INPUT_10
#define USBD_MANUFACTURER USB_STR_MANUFACTURER_MNT
#define USBD_PRODUCT USB_STR_PRODUCT_MNT_POCKET_REFORM_INPUT_10

#define USBD_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + MNTRE_RESET_TUD_DESC_LEN)
#define USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP
#define USBD_MAX_POWER_MA (100)

#define EPNUM_HID   0x81

enum {
    USBD_ITF_HID = 0,
    USBD_ITF_MNTRE_RESET,
    USBD_ITF_MAX,
};

enum {
    USBD_STR_0 = 0,
    USBD_STR_MANUFACTURER,
    USBD_STR_PRODUCT,
    USBD_STR_SERIAL,
    USBD_STR_MNTRE_RESET,
};

// Note: descriptors returned from callbacks must exist long enough for transfer to complete

//--------------------------------------------------------------------+
// HID Report Descriptor
//--------------------------------------------------------------------+

/* FIXME REPORT_ID 5 hardcoded */
#define TUD_HID_REPORT_DESC_MNTMOUSE() \
  HID_USAGE_PAGE ( HID_USAGE_PAGE_DESKTOP      )                   ,\
  HID_USAGE      ( HID_USAGE_DESKTOP_MOUSE     )                   ,\
  HID_COLLECTION ( HID_COLLECTION_APPLICATION  )                   ,\
    HID_REPORT_ID ( REPORT_ID_MOUSE ) \
    HID_USAGE      ( HID_USAGE_DESKTOP_POINTER )                   ,\
    HID_COLLECTION ( HID_COLLECTION_PHYSICAL   )                   ,\
      HID_USAGE_PAGE  ( HID_USAGE_PAGE_BUTTON  )                   ,\
        HID_USAGE_MIN   ( 1                                      ) ,\
        HID_USAGE_MAX   ( 5                                      ) ,\
        HID_LOGICAL_MIN ( 0                                      ) ,\
        HID_LOGICAL_MAX ( 1                                      ) ,\
        /* Left, Right, Middle, Backward, Forward buttons */ \
        HID_REPORT_COUNT( 5                                      ) ,\
        HID_REPORT_SIZE ( 1                                      ) ,\
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ) ,\
        /* 3 bit padding */ \
        HID_REPORT_COUNT( 1                                      ) ,\
        HID_REPORT_SIZE ( 3                                      ) ,\
        HID_INPUT       ( HID_CONSTANT                           ) ,\
      HID_USAGE_PAGE  ( HID_USAGE_PAGE_DESKTOP )                   ,\
        /* X, Y position [-127, 127] */ \
        HID_USAGE       ( HID_USAGE_DESKTOP_X                    ) ,\
        HID_USAGE       ( HID_USAGE_DESKTOP_Y                    ) ,\
        HID_LOGICAL_MIN ( 0x81                                   ) ,\
        HID_LOGICAL_MAX ( 0x7f                                   ) ,\
        HID_REPORT_COUNT( 2                                      ) ,\
        HID_REPORT_SIZE ( 8                                      ) ,\
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_RELATIVE ) ,\
      HID_COLLECTION ( HID_COLLECTION_LOGICAL   )                   ,\
        HID_REPORT_ID ( 5 ) \
        HID_USAGE_PAGE  ( HID_USAGE_PAGE_DESKTOP )                   ,\
        HID_USAGE       ( HID_USAGE_DESKTOP_RESOLUTION_MULTIPLIER                )  ,\
        HID_LOGICAL_MIN ( 0                                   )  ,\
        HID_LOGICAL_MAX ( 1                                   )  ,\
        HID_PHYSICAL_MIN ( 1                                   ), \
        HID_PHYSICAL_MAX ( 12                                   ), \
        HID_REPORT_COUNT( 1                                      )  ,\
        HID_REPORT_SIZE ( 8                                      )  ,\
        HID_FEATURE       ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ) ,\
        /* Vertical wheel scroll [-127, 127] */ \
        HID_REPORT_ID ( REPORT_ID_MOUSE ) \
        HID_USAGE_PAGE  ( HID_USAGE_PAGE_DESKTOP )                   ,\
        HID_USAGE       ( HID_USAGE_DESKTOP_WHEEL                )  ,\
        HID_LOGICAL_MIN ( 0x81                                   )  ,\
        HID_LOGICAL_MAX ( 0x7f                                   )  ,\
        HID_PHYSICAL_MIN ( 0                                   ), \
        HID_PHYSICAL_MAX ( 0                                   ), \
        HID_REPORT_COUNT( 1                                      )  ,\
        HID_REPORT_SIZE ( 8                                      )  ,\
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_RELATIVE )  ,\
        HID_REPORT_ID ( REPORT_ID_MOUSE ) \
        HID_USAGE_PAGE  ( HID_USAGE_PAGE_CONSUMER ), \
       /* Horizontal wheel scroll [-127, 127] */ \
        HID_USAGE_N     ( HID_USAGE_CONSUMER_AC_PAN, 2           ), \
        HID_LOGICAL_MIN ( 0x81                                   ), \
        HID_LOGICAL_MAX ( 0x7f                                   ), \
        HID_PHYSICAL_MIN ( 0x81                                   ), \
        HID_PHYSICAL_MAX ( 0x7f                                   ), \
        HID_REPORT_COUNT( 1                                      ), \
        HID_REPORT_SIZE ( 8                                      ), \
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_RELATIVE ), \
      HID_COLLECTION_END                                            , \
    HID_COLLECTION_END                                            , \
  HID_COLLECTION_END \


uint8_t static const desc_hid_report[] =
{
  TUD_HID_REPORT_DESC_KEYBOARD( HID_REPORT_ID(REPORT_ID_KEYBOARD         )),
  TUD_HID_REPORT_DESC_MNTMOUSE( ),
  TUD_HID_REPORT_DESC_CONSUMER( HID_REPORT_ID(REPORT_ID_CONSUMER_CONTROL )),
  TUD_HID_REPORT_DESC_GAMEPAD ( HID_REPORT_ID(REPORT_ID_GAMEPAD          ))
};

// Invoked when received GET HID REPORT DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance)
{
  (void) instance;
  return desc_hid_report;
}

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
static const tusb_desc_device_t usbd_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = BCD_USB_MIN_FOR_BOS,
    .bDeviceClass = 0,  // Use data from interface descriptors
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0001,
    .iManufacturer = USBD_STR_MANUFACTURER,
    .iProduct = USBD_STR_PRODUCT,
    .iSerialNumber = USBD_STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t usbd_desc_cfg[USBD_DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, USBD_ITF_MAX, USBD_STR_0, USBD_DESC_LEN,
        USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE, USBD_MAX_POWER_MA),

    // Interface number, string index, protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(USBD_ITF_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 5),

    MNTRE_RESET_TUD_DESCRIPTOR(USBD_ITF_MNTRE_RESET, USBD_STR_MNTRE_RESET)
};

static char usbd_serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const usbd_desc_str[] = {
    [USBD_STR_MANUFACTURER] = USBD_MANUFACTURER,
    [USBD_STR_PRODUCT] = USBD_PRODUCT,
    [USBD_STR_SERIAL] = usbd_serial_str,
    [USBD_STR_MNTRE_RESET] = MNTRE_RESET_INTERFACE_NAME_STR,
};

// Custom USB class drivers. Due to how tinyusb works, the drivers have to be in a contigous array.
static usbd_class_driver_t usb_class_drivers[1];
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = sizeof(usb_class_drivers)/sizeof(usb_class_drivers[0]);
    usb_class_drivers[0] = mntre_reset_class_driver;
    return &usb_class_drivers[0];
}

//------------- DS-20 (fwupd) -------------//
static const uint8_t desc_ds20[] = {
#include "ds20-descriptor.h"
};

#define DS_20_DESC_LEN           (sizeof(desc_ds20))
#define BOS_TOTAL_LEN            (TUD_BOS_DESC_LEN + TUD_BOS_DS_20_DESC_LEN)
#define DS_20_VENDOR_CODE        0x42

uint8_t const desc_bos[] = {
    // total length, number of device caps
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),

    // DS-20, as used in fwupd
    TUD_BOS_DS20_DESCRIPTOR(DS_20_DESC_LEN, DS_20_VENDOR_CODE)
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
