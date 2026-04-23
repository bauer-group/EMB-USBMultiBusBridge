/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * USB composite device descriptors for RPBridge.
 *
 * Interface layout (bInterfaceNumber → purpose):
 *   0,1   CDC-ACM #0           "rpbridge UART 1"
 *   2,3   CDC-ACM #1           "rpbridge UART 2"
 *   4,5   CDC-ACM #2           "rpbridge Debug Log"
 *   6     Vendor (WinUSB)      "rpbridge Control" — RPBP transport
 *   7     Vendor (gs_usb)      "rpbridge CAN"     — SocketCAN-compatible
 *
 * VID/PID placeholder (update before first release):
 *   0x1209 / 0xXXXX  — pid.codes free tier for open-hardware projects.
 */

#include "tusb.h"
#include "pico/unique_id.h"

#include "version.h"
#include "ms_os_20.h"

#include <string.h>

/* ───── VID / PID placeholder (to be assigned via pid.codes) ─────────── */

#define RPBRIDGE_USB_VID    0x1209   /* pid.codes (replace with real PID before shipping) */
#define RPBRIDGE_USB_PID    0xFFFE   /* temporary "testing" PID per pid.codes policy */

#define RPBRIDGE_USB_BCD    0x0210   /* USB 2.1 to enable BOS / MS OS 2.0 */

/* ───── Device descriptor ────────────────────────────────────────────── */

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = RPBRIDGE_USB_BCD,

    /* Composite device with IADs — class must be "Miscellaneous / 0x02 / 0x01". */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = RPBRIDGE_USB_VID,
    .idProduct          = RPBRIDGE_USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *) &desc_device;
}

/* ───── Configuration descriptor ─────────────────────────────────────── */

enum {
    ITF_CDC_0     = 0,     /* user UART 1 (phy-bank: RS232/RS485/TTL) */
    ITF_CDC_0_DATA,
    ITF_CDC_1,             /* user UART 2 (PIO-UART #2)                */
    ITF_CDC_1_DATA,
    ITF_VENDOR_CTRL,       /* RPBP control pipe                        */
    ITF_VENDOR_CAN,        /* gs_usb CAN pipe                          */
    ITF_TOTAL
};

#define EPNUM_CDC_0_NOTIF   0x81
#define EPNUM_CDC_0_OUT     0x02
#define EPNUM_CDC_0_IN      0x82

#define EPNUM_CDC_1_NOTIF   0x83
#define EPNUM_CDC_1_OUT     0x04
#define EPNUM_CDC_1_IN      0x84

#define EPNUM_VCTRL_OUT     0x05
#define EPNUM_VCTRL_IN      0x85

#define EPNUM_VCAN_OUT      0x06
#define EPNUM_VCAN_IN       0x86

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN +                \
                             TUD_CDC_DESC_LEN * 2 +               \
                             TUD_VENDOR_DESC_LEN * 2)

uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80 /* bus-powered */, 250 /* 500 mA */),

    TUD_CDC_DESCRIPTOR(ITF_CDC_0, 4 /* iInterface */, EPNUM_CDC_0_NOTIF, 8,
                       EPNUM_CDC_0_OUT, EPNUM_CDC_0_IN, 64),

    TUD_CDC_DESCRIPTOR(ITF_CDC_1, 5 /* iInterface */, EPNUM_CDC_1_NOTIF, 8,
                       EPNUM_CDC_1_OUT, EPNUM_CDC_1_IN, 64),

    TUD_VENDOR_DESCRIPTOR(ITF_VENDOR_CTRL, 6 /* iInterface */,
                          EPNUM_VCTRL_OUT, EPNUM_VCTRL_IN, 64),

    TUD_VENDOR_DESCRIPTOR(ITF_VENDOR_CAN, 7 /* iInterface */,
                          EPNUM_VCAN_OUT, EPNUM_VCAN_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void) index;
    return desc_fs_configuration;
}

/* ───── String descriptors ───────────────────────────────────────────── */

static char rpbridge_serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1] = { 0 };

static void serial_from_unique_id(void)
{
    if (rpbridge_serial[0] != 0) {
        return;
    }
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    static const char hex[] = "0123456789ABCDEF";
    size_t w = 0;
    for (size_t i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; ++i) {
        rpbridge_serial[w++] = hex[(id.id[i] >> 4) & 0xFu];
        rpbridge_serial[w++] = hex[(id.id[i]) & 0xFu];
    }
    rpbridge_serial[w] = '\0';
}

static char const *const string_desc_arr[] = {
    (const char[]){0x09, 0x04},           /* 0: supported language = en-US (0x0409) */
    "BAUER GROUP",                        /* 1: Manufacturer */
    "RPBridge Multi-Bus Developer Bridge",/* 2: Product */
    rpbridge_serial,                      /* 3: Serial number (filled lazily) */
    "rpbridge UART 1",                    /* 4: CDC-ACM #0 — user phy-bank UART */
    "rpbridge UART 2",                    /* 5: CDC-ACM #1 — PIO-UART #2        */
    "rpbridge Control",                   /* 6: vendor RPBP                     */
    "rpbridge CAN",                       /* 7: vendor gs_usb                   */
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void) langid;

    uint8_t chr_count;

    if (index == 3) {
        serial_from_unique_id();
    }

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
        return NULL;
    } else {
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t) strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; ++i) {
            _desc_str[i + 1] = str[i];
        }
    }

    _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

/*
 * BOS descriptor — carries the MS OS 2.0 platform capability. Windows
 * 10 (1709+) reads this during enumeration and then fetches the
 * descriptor set via the vendor control transfer handled below.
 * Linux ignores BOS silently, so the same binary works everywhere.
 */
uint8_t const *tud_descriptor_bos_cb(void)
{
    return desc_bos;
}

/*
 * Vendor control-transfer handler. We only use it for the MS OS 2.0
 * "GetMSDescriptor" request; any other vendor-class control request
 * is rejected (the host's RPBP control channel uses bulk endpoints,
 * not control transfers).
 *
 * Request shape:
 *   bmRequestType = 0xC0  (Device-to-Host, Vendor, Device)
 *   bRequest      = MS_OS_20_VENDOR_CODE  (our chosen code)
 *   wValue        = 0
 *   wIndex        = MS_OS_20_DESCRIPTOR_INDEX  (0x0007)
 *   wLength       = requested byte count
 */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    /* Only SETUP carries the request we inspect; DATA and ACK stages
     * are handled by TinyUSB's own machinery. */
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    const uint8_t req_type  = request->bmRequestType_bit.type;
    const uint8_t req_recip = request->bmRequestType_bit.recipient;
    const uint8_t dir       = request->bmRequestType_bit.direction;

    if (req_type  != TUSB_REQ_TYPE_VENDOR) return false;
    if (dir       != TUSB_DIR_IN)          return false;
    if (req_recip != TUSB_REQ_RCPT_DEVICE) return false;

    if (request->bRequest == MS_OS_20_VENDOR_CODE &&
        request->wIndex   == MS_OS_20_DESCRIPTOR_INDEX) {
        const uint16_t cap = (uint16_t)desc_ms_os_20_len;
        const uint16_t len = request->wLength < cap ? request->wLength : cap;
        return tud_control_xfer(rhport, request,
                                (void *)(uintptr_t)desc_ms_os_20, len);
    }

    /* Unknown vendor control request — stall the pipe. */
    return false;
}
