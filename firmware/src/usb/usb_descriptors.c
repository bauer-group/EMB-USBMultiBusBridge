/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * USB composite device descriptors for RPBridge.
 *
 * Interface layout (bInterfaceNumber → purpose):
 *   0,1   CDC-ACM #0           "RPBridge UART1 (RS232)"  — via MAX3232
 *   2,3   CDC-ACM #1           "RPBridge UART1 (RS485)"  — via SN65HVD72
 *   4,5   CDC-ACM #2           "RPBridge UART1 (TTL)"    — direct drive
 *   6,7   CDC-ACM #3           "RPBridge UART2 (TTL)"    — PIO-UART #2
 * # if RPBRIDGE_DEBUG_CDC
 *   8,9   CDC-ACM #4           "RPBridge Debug Log"      — printf() sink
 *   10    Vendor (WinUSB)      "RPBridge RPBP transport" — bulk control
 *   11    Vendor (gs_usb)      "RPBridge CAN (gs_usb)"   — SocketCAN-compat
 * # else
 *   8     Vendor (WinUSB)      "RPBridge RPBP transport" — bulk control
 *   9     Vendor (gs_usb)      "RPBridge CAN (gs_usb)"   — SocketCAN-compat
 * # endif
 *
 * The four user-facing CDCs are all data channels for UART traffic.
 * Three of them are alternate views of the same UART1 hardware peripheral
 * routed through different transceivers (MAX3232 / SN65HVD72 / direct
 * TTL); the firmware switches the active transceiver bank based on
 * which CDC currently asserts DTR=1. The fourth CDC drives UART2 via a
 * PIO state-machine on dedicated pins.
 *
 * The optional 5th CDC ("Debug Log") is gated by the CMake option
 * RPBRIDGE_DEBUG_CDC (default OFF). When enabled, it carries pico-sdk
 * stdio output as a USB-side log channel and shifts the vendor
 * interfaces from {8,9} to {10,11}; ms_os_20.c mirrors the same gate.
 * UART1-hardware stdio on the 5-pin debug header (GP4/GP5) stays
 * enabled either way — see firmware/src/usb/debug_cdc.c and the
 * pico_enable_stdio_* lines in firmware/CMakeLists.txt.
 *
 * VID/PID — see the §VID / PID block below. The pair 0x0545:0x0101 is
 * the production allocation made to BAUER GROUP on 2026-04-27. Keep
 * the kernel driver, udev rules, and userspace client in lock-step
 * with this pair (see the §VID / PID block for the lock-step list).
 */

#include "tusb.h"
#include "pico/unique_id.h"

#include "version.h"
#include "ms_os_20.h"
#include "gs_usb_glue.h"

#include <string.h>

/* ───── VID / PID ──────────────────────────────────────────────────── *
 *
 * Production VID/PID assigned to BAUER GROUP for the RPBridge.
 * Allocated 2026-04-27. The same pair is wired into the kernel
 * driver's match table (driver/src/rpbp.h: RPBRIDGE_USB_*) and
 * the userspace client's defaults (userspace/rpbridge/src/lib.rs:
 * DEFAULT_VID / DEFAULT_PID). Keep all three sites in lock-step
 * — a mismatch silently breaks the auto-bind path.
 */

#define RPBRIDGE_USB_VID    0x0545   /* BAUER GROUP — production VID */
#define RPBRIDGE_USB_PID    0x0101   /* RPBridge — production PID    */
#define RPBRIDGE_BCD_DEVICE 0x0100u  /* RPBP wire-protocol major v1  */

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
    /*
     * bcdDevice is intentionally coupled to the RPBP wire-protocol
     * MAJOR version, NOT to the firmware semver. RPBP v1 → 0x0100;
     * RPBP v2 (future, breaking wire-format change) would step to
     * 0x0200. Coupling to firmware patches would force Windows
     * (which keys on `REV_xxxx`) to re-install drivers + re-pair
     * applications on every bug-fix release — exactly the opposite
     * of what bcdDevice is for.
     */
    .bcdDevice          = RPBRIDGE_BCD_DEVICE,

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

/* Interface-number enum. Each CDC slot is conditional on the board
 * header's HAVE_* flag — bare-board variants without an on-board
 * transceiver simply don't enumerate the corresponding CDC, and
 * TUSB renumbers the survivors starting from 0 in descriptor order.
 * The TTL UART0 CDC is unconditional (it's the bare-wire path on
 * UART0 hardware that always exists). */
enum {
#if RPBRIDGE_HAVE_RS232_PHY
    ITF_CDC_0     = 0,     /* UART 1 RS232 (MAX3232 via UART0 HW)        */
    ITF_CDC_0_DATA,
#endif
#if RPBRIDGE_HAVE_RS485_PHY
    ITF_CDC_1,             /* UART 1 RS485 (SN65HVD72 via UART0 HW)      */
    ITF_CDC_1_DATA,
#endif
    ITF_CDC_2,             /* UART 1 TTL   (direct 3.3 V via UART0 HW)   */
    ITF_CDC_2_DATA,
#if RPBRIDGE_HAVE_PIO_UART
    ITF_CDC_3,             /* UART 2 TTL   (PIO-UART #2)                 */
    ITF_CDC_3_DATA,
#endif
#ifdef RPBRIDGE_DEBUG_CDC
    ITF_CDC_4,             /* Debug Log    (printf() sink, optional)     */
    ITF_CDC_4_DATA,
#endif
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

/* Vendor EPs are kept at the same hardware EP numbers regardless of
 * the debug-CDC flag — only the interface NUMBERS shift. The optional
 * CDC #4 is allocated EP11/EP12 so it sits past the vendor EPs and
 * adding/removing the flag never touches the RPBP/gs_usb endpoints. */
#define EPNUM_VCTRL_OUT     0x09
#define EPNUM_VCTRL_IN      0x89

#define EPNUM_VCAN_OUT      0x0A
#define EPNUM_VCAN_IN       0x8A

#ifdef RPBRIDGE_DEBUG_CDC
#define EPNUM_CDC_4_NOTIF   0x8B
#define EPNUM_CDC_4_OUT     0x0C
#define EPNUM_CDC_4_IN      0x8C

#define ISTR_CDC_4          8u
#define ISTR_VENDOR_CTRL    9u
#define ISTR_VENDOR_CAN     10u
#else
#define ISTR_VENDOR_CTRL    8u
#define ISTR_VENDOR_CAN     9u
#endif

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN +                \
                             TUD_CDC_DESC_LEN * CFG_TUD_CDC +     \
                             TUD_VENDOR_DESC_LEN * 2)

uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80 /* bus-powered */, 250 /* 500 mA */),

#if RPBRIDGE_HAVE_RS232_PHY
    /* CDC #0 — UART 1 RS232 (gated on RPBRIDGE_HAVE_RS232_PHY) */
    TUD_CDC_DESCRIPTOR(ITF_CDC_0, 4 /* iInterface */, EPNUM_CDC_0_NOTIF, 8,
                       EPNUM_CDC_0_OUT, EPNUM_CDC_0_IN, 64),
#endif

#if RPBRIDGE_HAVE_RS485_PHY
    /* CDC #1 — UART 1 RS485 (gated on RPBRIDGE_HAVE_RS485_PHY) */
    TUD_CDC_DESCRIPTOR(ITF_CDC_1, 5 /* iInterface */, EPNUM_CDC_1_NOTIF, 8,
                       EPNUM_CDC_1_OUT, EPNUM_CDC_1_IN, 64),
#endif

    /* CDC #2 — UART 1 TTL (always present where UART0 GPIOs are wired) */
    TUD_CDC_DESCRIPTOR(ITF_CDC_2, 6 /* iInterface */, EPNUM_CDC_2_NOTIF, 8,
                       EPNUM_CDC_2_OUT, EPNUM_CDC_2_IN, 64),

#if RPBRIDGE_HAVE_PIO_UART
    /* CDC #3 — UART 2 TTL (PIO-UART, gated on RPBRIDGE_HAVE_PIO_UART) */
    TUD_CDC_DESCRIPTOR(ITF_CDC_3, 7 /* iInterface */, EPNUM_CDC_3_NOTIF, 8,
                       EPNUM_CDC_3_OUT, EPNUM_CDC_3_IN, 64),
#endif

#ifdef RPBRIDGE_DEBUG_CDC
    /* CDC #4 — Debug Log (printf sink, build-gated) */
    TUD_CDC_DESCRIPTOR(ITF_CDC_4, ISTR_CDC_4, EPNUM_CDC_4_NOTIF, 8,
                       EPNUM_CDC_4_OUT, EPNUM_CDC_4_IN, 64),
#endif

    /* Vendor #0 — RPBP control */
    TUD_VENDOR_DESCRIPTOR(ITF_VENDOR_CTRL, ISTR_VENDOR_CTRL,
                          EPNUM_VCTRL_OUT, EPNUM_VCTRL_IN, 64),

    /* Vendor #1 — gs_usb CAN */
    TUD_VENDOR_DESCRIPTOR(ITF_VENDOR_CAN, ISTR_VENDOR_CAN,
                          EPNUM_VCAN_OUT, EPNUM_VCAN_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void) index;
    return desc_fs_configuration;
}

/* ───── String descriptors ───────────────────────────────────────────── */

/*
 * Serial number format: "RPB-" prefix + 16 hex chars derived from
 * the W25Q flash chip's unique 64-bit ID via pico_get_unique_board_id().
 * Total length 20 chars + NUL. The prefix makes the serial visually
 * identifiable as our device in lsusb output and is short enough to
 * fit comfortably in udev SYMLINK paths.
 *
 * Deterministic: same hardware → same serial across reboots and
 * reflashes. Globally unique: W25Q-class flash chip ID is guaranteed
 * unique by the manufacturer. No production-time provisioning step
 * is required.
 */
#define RPBRIDGE_SERIAL_PREFIX     "RPB-"
#define RPBRIDGE_SERIAL_PREFIX_LEN (sizeof(RPBRIDGE_SERIAL_PREFIX) - 1)
#define RPBRIDGE_SERIAL_LEN \
    (RPBRIDGE_SERIAL_PREFIX_LEN + PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2)

static char rpbridge_serial[RPBRIDGE_SERIAL_LEN + 1] = { 0 };

static void serial_from_unique_id(void)
{
    if (rpbridge_serial[0] != 0) {
        return;
    }
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    static const char hex[] = "0123456789ABCDEF";
    size_t w = 0;
    memcpy(&rpbridge_serial[w], RPBRIDGE_SERIAL_PREFIX,
           RPBRIDGE_SERIAL_PREFIX_LEN);
    w += RPBRIDGE_SERIAL_PREFIX_LEN;
    for (size_t i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; ++i) {
        rpbridge_serial[w++] = hex[(id.id[i] >> 4) & 0xFu];
        rpbridge_serial[w++] = hex[(id.id[i]) & 0xFu];
    }
    rpbridge_serial[w] = '\0';
}

static char const *const string_desc_arr[] = {
    (const char[]){0x09, 0x04},     /* 0: language = en-US (0x0409)       */
    "BAUER GROUP",                  /* 1: iManufacturer                   */
    "RPBridge",                     /* 2: iProduct                        */
    rpbridge_serial,                /* 3: iSerialNumber (filled lazily)   */
    "RPBridge UART1 (RS232)",       /* 4: CDC #0 — UART0 HW via MAX3232   */
    "RPBridge UART1 (RS485)",       /* 5: CDC #1 — UART0 HW via SN65HVD72 */
    "RPBridge UART1 (TTL)",         /* 6: CDC #2 — UART0 HW direct drive  */
    "RPBridge UART2 (TTL)",         /* 7: CDC #3 — PIO-UART #2            */
#ifdef RPBRIDGE_DEBUG_CDC
    "RPBridge Debug Log",           /* 8: CDC #4 — printf sink (optional) */
    "RPBridge RPBP transport",      /* 9: vendor RPBP                     */
    "RPBridge CAN (gs_usb)",        /* 10: vendor gs_usb                  */
#else
    "RPBridge RPBP transport",      /* 8: vendor RPBP                     */
    "RPBridge CAN (gs_usb)",        /* 9: vendor gs_usb                   */
#endif
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
