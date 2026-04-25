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
 * VID/PID — see the §VID / PID block below for the production
 * allocation status. The current values are pre-production and MUST
 * NOT ship in a public hardware release without the pid.codes
 * registration entry being filed first. See docs/usb-vid-pid.md.
 */

#include "tusb.h"
#include "pico/unique_id.h"

#include "version.h"
#include "ms_os_20.h"
#include "gs_usb_glue.h"

#include <string.h>

/* ───── VID / PID ──────────────────────────────────────────────────── *
 *
 * STATUS: PRE-PRODUCTION. The PID below is from the pid.codes
 * "testing" range (0xFFF0..0xFFFF), shared by every developer who
 * has not yet completed the pid.codes registration. Two RPBridge
 * boards on the same Linux host while using this PID would collide
 * with anyone else's testing-range device — fine for development,
 * UNACCEPTABLE for production hardware.
 *
 * Action required before first hardware shipment:
 *   1. File a pid.codes registration entry pointing at this repo.
 *      (Wiki: https://pid.codes/howto/ — kostenlos für Open-Hardware.)
 *   2. Replace RPBRIDGE_USB_PID with the allocated value.
 *   3. Update driver/src/core_driver.rs to match.
 *   4. Update docs/usb-vid-pid.md with the allocation timestamp.
 *
 * Tracked in firmware/TODO.md → "Pre-production gates".
 */

#define RPBRIDGE_USB_VID    0x1209   /* pid.codes — Open Hardware VID */
#define RPBRIDGE_USB_PID    0xFFFE   /* TESTING-RANGE — see PRE-PRODUCTION block above */

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
    ITF_CDC_0     = 0,     /* UART 1 RS232 (MAX3232 via UART0 HW)        */
    ITF_CDC_0_DATA,
    ITF_CDC_1,             /* UART 1 RS485 (SN65HVD72 via UART0 HW)      */
    ITF_CDC_1_DATA,
    ITF_CDC_2,             /* UART 1 TTL   (direct 3.3 V via UART0 HW)   */
    ITF_CDC_2_DATA,
    ITF_CDC_3,             /* UART 2 TTL   (PIO-UART #2)                 */
    ITF_CDC_3_DATA,
    ITF_VENDOR_CTRL,       /* RPBP control pipe                          */
    ITF_VENDOR_CAN,        /* gs_usb CAN pipe                            */
    ITF_TOTAL
};

#define EPNUM_CDC_0_NOTIF   0x81
#define EPNUM_CDC_0_OUT     0x02
#define EPNUM_CDC_0_IN      0x82

#define EPNUM_CDC_1_NOTIF   0x83
#define EPNUM_CDC_1_OUT     0x04
#define EPNUM_CDC_1_IN      0x84

#define EPNUM_CDC_2_NOTIF   0x85
#define EPNUM_CDC_2_OUT     0x06
#define EPNUM_CDC_2_IN      0x86

#define EPNUM_CDC_3_NOTIF   0x87
#define EPNUM_CDC_3_OUT     0x08
#define EPNUM_CDC_3_IN      0x88

#define EPNUM_VCTRL_OUT     0x09
#define EPNUM_VCTRL_IN      0x89

#define EPNUM_VCAN_OUT      0x0A
#define EPNUM_VCAN_IN       0x8A

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN +                \
                             TUD_CDC_DESC_LEN * 4 +               \
                             TUD_VENDOR_DESC_LEN * 2)

uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80 /* bus-powered */, 250 /* 500 mA */),

    /* CDC #0 — UART 1 RS232 */
    TUD_CDC_DESCRIPTOR(ITF_CDC_0, 4 /* iInterface */, EPNUM_CDC_0_NOTIF, 8,
                       EPNUM_CDC_0_OUT, EPNUM_CDC_0_IN, 64),

    /* CDC #1 — UART 1 RS485 */
    TUD_CDC_DESCRIPTOR(ITF_CDC_1, 5 /* iInterface */, EPNUM_CDC_1_NOTIF, 8,
                       EPNUM_CDC_1_OUT, EPNUM_CDC_1_IN, 64),

    /* CDC #2 — UART 1 TTL */
    TUD_CDC_DESCRIPTOR(ITF_CDC_2, 6 /* iInterface */, EPNUM_CDC_2_NOTIF, 8,
                       EPNUM_CDC_2_OUT, EPNUM_CDC_2_IN, 64),

    /* CDC #3 — UART 2 TTL (PIO-UART) */
    TUD_CDC_DESCRIPTOR(ITF_CDC_3, 7 /* iInterface */, EPNUM_CDC_3_NOTIF, 8,
                       EPNUM_CDC_3_OUT, EPNUM_CDC_3_IN, 64),

    /* Vendor #0 — RPBP control */
    TUD_VENDOR_DESCRIPTOR(ITF_VENDOR_CTRL, 8 /* iInterface */,
                          EPNUM_VCTRL_OUT, EPNUM_VCTRL_IN, 64),

    /* Vendor #1 — gs_usb CAN */
    TUD_VENDOR_DESCRIPTOR(ITF_VENDOR_CAN, 9 /* iInterface */,
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
    "rpbridge UART 1 (RS232)",            /* 4: CDC #0 — UART0 HW via MAX3232   */
    "rpbridge UART 1 (RS485)",            /* 5: CDC #1 — UART0 HW via SN65HVD72 */
    "rpbridge UART 1 (TTL)",              /* 6: CDC #2 — UART0 HW direct drive  */
    "rpbridge UART 2 (TTL)",              /* 7: CDC #3 — PIO-UART #2            */
    "rpbridge Control",                   /* 8: vendor RPBP                     */
    "rpbridge CAN",                       /* 9: vendor gs_usb                   */
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

    if (req_type != TUSB_REQ_TYPE_VENDOR) return false;

    /* Device-recipient IN: MS OS 2.0 descriptor — Windows auto-binds
     * WinUSB to our vendor interfaces via this path. */
    if (req_recip == TUSB_REQ_RCPT_DEVICE && dir == TUSB_DIR_IN &&
        request->bRequest == MS_OS_20_VENDOR_CODE &&
        request->wIndex   == MS_OS_20_DESCRIPTOR_INDEX) {
        const uint16_t cap = (uint16_t)desc_ms_os_20_len;
        const uint16_t len = request->wLength < cap ? request->wLength : cap;
        return tud_control_xfer(rhport, request,
                                (void *)(uintptr_t)desc_ms_os_20, len);
    }

    /* Interface-recipient: gs_usb class requests on the CAN vendor
     * interface land here. Delegate to gs_usb_glue which knows the
     * wire protocol. */
    if (req_recip == TUSB_REQ_RCPT_INTERFACE) {
        return gs_usb_glue_control_xfer(rhport, stage, request);
    }

    /* Unknown vendor control request — stall the pipe. */
    return false;
}
