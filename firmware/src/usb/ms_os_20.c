/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * Microsoft OS 2.0 descriptor set + BOS descriptor.
 *
 * Layout inventory:
 *
 *   BOS (33 B)
 *     ├── BOS header                                      5 B
 *     └── Platform capability descriptor                 28 B
 *           ├── Windows OS 2.0 UUID                        16 B
 *           └── descriptor-set length + vendor code       12 B
 *
 *   MS OS 2.0 descriptor set (330 B)
 *     ├── Set header                                      10 B
 *     ├── Function subset for IF N   (RPBP control)     160 B
 *     │     ├── subset header                              8 B
 *     │     ├── COMPATIBLE_ID "WINUSB"                    20 B
 *     │     └── REG_PROPERTY DeviceInterfaceGUIDs        132 B
 *     └── Function subset for IF N+1 (gs_usb CAN)       160 B   (same shape)
 *
 * The vendor interface numbers depend on RPBRIDGE_DEBUG_CDC: with the
 * default 4-CDC build they are {8,9}; with the optional 5th debug-log
 * CDC included they shift to {10,11}. The two macros below pick up
 * the correct values from the same flag that gates usb_descriptors.c.
 *
 * DeviceInterfaceGUIDs:
 *   - RPBP control:  {24D7D4F5-FB9F-4B5D-8A1F-69F3C9FE8B23}
 *   - gs_usb CAN:    {8F3B2E41-5A6C-4D7E-9F1A-2B3C4D5E6F70}
 *
 * Both GUIDs are project-local. Host-side Windows libraries (C#, Rust
 * `windows` crate, libusb-1.0) enumerate RPBridge by these GUIDs so a
 * user's tooling does not need to know the VID/PID.
 */

#include "ms_os_20.h"
#include "tusb.h"

/* ───── Byte-packing helpers ─────────────────────────────────────────── */

#define U16LE(v)        (uint8_t)((v) & 0xFFu), \
                        (uint8_t)(((v) >> 8) & 0xFFu)

#define U32LE(v)        (uint8_t)((v) & 0xFFu),          \
                        (uint8_t)(((v) >> 8) & 0xFFu),   \
                        (uint8_t)(((v) >> 16) & 0xFFu),  \
                        (uint8_t)(((v) >> 24) & 0xFFu)

/* Pack an ASCII character as one UTF-16LE code unit. */
#define U16CH(c)        (uint8_t)(c), 0u
#define U16NUL          0u, 0u

/* ───── MS OS 2.0 descriptor-set section types ───────────────────────── */

#define MS_OS_20_SET_HEADER          0x0000u
#define MS_OS_20_SUBSET_HEADER_CFG   0x0001u
#define MS_OS_20_SUBSET_HEADER_FN    0x0002u
#define MS_OS_20_FEATURE_COMPAT_ID   0x0003u
#define MS_OS_20_FEATURE_REG_PROP    0x0004u

#define MS_OS_20_REG_MULTI_SZ        0x0007u

/* Windows 8.1. Earlier releases don't implement BOS+MS OS 2.0. */
#define MS_OS_20_WINDOWS_VERSION     0x06030000u

/* Block sizes (computed once so the totals stay consistent). */
#define SET_HEADER_LEN               10u
#define FUNCTION_SUBSET_HEADER_LEN    8u
#define COMPAT_ID_LEN                20u
#define REG_PROP_DEVIF_LEN          132u
#define FUNCTION_SUBSET_LEN          (FUNCTION_SUBSET_HEADER_LEN + \
                                      COMPAT_ID_LEN + REG_PROP_DEVIF_LEN)
#define MS_OS_20_DESC_LEN            (SET_HEADER_LEN + 2u * FUNCTION_SUBSET_LEN)

/* ───── Vendor-interface numbers — must match usb_descriptors.c ─────── */

#ifdef RPBRIDGE_DEBUG_CDC
#define IF_NUM_RPBP_CTRL             10u
#define IF_NUM_GS_USB_CAN            11u
#else
#define IF_NUM_RPBP_CTRL             8u
#define IF_NUM_GS_USB_CAN            9u
#endif

/* ───── MS OS 2.0 descriptor set blob ────────────────────────────────── */

const uint8_t desc_ms_os_20[MS_OS_20_DESC_LEN] = {
    /* ──────────────── Set header (10 B) ──────────────── */
    U16LE(SET_HEADER_LEN),
    U16LE(MS_OS_20_SET_HEADER),
    U32LE(MS_OS_20_WINDOWS_VERSION),
    U16LE(MS_OS_20_DESC_LEN),

    /* ──── Function subset — RPBP control (IF_NUM_RPBP_CTRL) ──── */
    U16LE(FUNCTION_SUBSET_HEADER_LEN),
    U16LE(MS_OS_20_SUBSET_HEADER_FN),
    IF_NUM_RPBP_CTRL,
    0u,                         /* bReserved */
    U16LE(FUNCTION_SUBSET_LEN),

    /* Compatible ID "WINUSB" */
    U16LE(COMPAT_ID_LEN),
    U16LE(MS_OS_20_FEATURE_COMPAT_ID),
    'W','I','N','U','S','B', 0u, 0u,         /* compatibleID[8] */
    0u,0u,0u,0u,0u,0u,0u,0u,                 /* subCompatibleID[8] */

    /* Registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ) */
    U16LE(REG_PROP_DEVIF_LEN),
    U16LE(MS_OS_20_FEATURE_REG_PROP),
    U16LE(MS_OS_20_REG_MULTI_SZ),
    U16LE(42u),                  /* name length, bytes */
    /* "DeviceInterfaceGUIDs\0" — 21 UTF-16LE code units = 42 B */
    U16CH('D'),U16CH('e'),U16CH('v'),U16CH('i'),U16CH('c'),
    U16CH('e'),U16CH('I'),U16CH('n'),U16CH('t'),U16CH('e'),
    U16CH('r'),U16CH('f'),U16CH('a'),U16CH('c'),U16CH('e'),
    U16CH('G'),U16CH('U'),U16CH('I'),U16CH('D'),U16CH('s'),
    U16NUL,
    U16LE(80u),                  /* data length, bytes */
    /* "{24D7D4F5-FB9F-4B5D-8A1F-69F3C9FE8B23}" + NUL + NUL (MULTI_SZ) */
    U16CH('{'),
    U16CH('2'),U16CH('4'),U16CH('D'),U16CH('7'),U16CH('D'),
    U16CH('4'),U16CH('F'),U16CH('5'),U16CH('-'),U16CH('F'),
    U16CH('B'),U16CH('9'),U16CH('F'),U16CH('-'),U16CH('4'),
    U16CH('B'),U16CH('5'),U16CH('D'),U16CH('-'),U16CH('8'),
    U16CH('A'),U16CH('1'),U16CH('F'),U16CH('-'),U16CH('6'),
    U16CH('9'),U16CH('F'),U16CH('3'),U16CH('C'),U16CH('9'),
    U16CH('F'),U16CH('E'),U16CH('8'),U16CH('B'),U16CH('2'),
    U16CH('3'),U16CH('}'),
    U16NUL, U16NUL,

    /* ──── Function subset — gs_usb CAN (IF_NUM_GS_USB_CAN) ───── */
    U16LE(FUNCTION_SUBSET_HEADER_LEN),
    U16LE(MS_OS_20_SUBSET_HEADER_FN),
    IF_NUM_GS_USB_CAN,
    0u,
    U16LE(FUNCTION_SUBSET_LEN),

    U16LE(COMPAT_ID_LEN),
    U16LE(MS_OS_20_FEATURE_COMPAT_ID),
    'W','I','N','U','S','B', 0u, 0u,
    0u,0u,0u,0u,0u,0u,0u,0u,

    U16LE(REG_PROP_DEVIF_LEN),
    U16LE(MS_OS_20_FEATURE_REG_PROP),
    U16LE(MS_OS_20_REG_MULTI_SZ),
    U16LE(42u),
    U16CH('D'),U16CH('e'),U16CH('v'),U16CH('i'),U16CH('c'),
    U16CH('e'),U16CH('I'),U16CH('n'),U16CH('t'),U16CH('e'),
    U16CH('r'),U16CH('f'),U16CH('a'),U16CH('c'),U16CH('e'),
    U16CH('G'),U16CH('U'),U16CH('I'),U16CH('D'),U16CH('s'),
    U16NUL,
    U16LE(80u),
    /* "{8F3B2E41-5A6C-4D7E-9F1A-2B3C4D5E6F70}" + NUL + NUL */
    U16CH('{'),
    U16CH('8'),U16CH('F'),U16CH('3'),U16CH('B'),U16CH('2'),
    U16CH('E'),U16CH('4'),U16CH('1'),U16CH('-'),U16CH('5'),
    U16CH('A'),U16CH('6'),U16CH('C'),U16CH('-'),U16CH('4'),
    U16CH('D'),U16CH('7'),U16CH('E'),U16CH('-'),U16CH('9'),
    U16CH('F'),U16CH('1'),U16CH('A'),U16CH('-'),U16CH('2'),
    U16CH('B'),U16CH('3'),U16CH('C'),U16CH('4'),U16CH('D'),
    U16CH('5'),U16CH('E'),U16CH('6'),U16CH('F'),U16CH('7'),
    U16CH('0'),U16CH('}'),
    U16NUL, U16NUL,
};

const size_t desc_ms_os_20_len = sizeof(desc_ms_os_20);

/* ───── BOS descriptor ───────────────────────────────────────────────── */

#define BOS_HEADER_LEN        5u
#define BOS_PLATFORM_CAP_LEN 28u
#define BOS_TOTAL_LEN        (BOS_HEADER_LEN + BOS_PLATFORM_CAP_LEN)

const uint8_t desc_bos[BOS_TOTAL_LEN] = {
    /* BOS header (5 B) */
    BOS_HEADER_LEN,
    TUSB_DESC_BOS,
    U16LE(BOS_TOTAL_LEN),
    1u,                                /* bNumDeviceCaps */

    /* Platform capability descriptor (28 B) */
    BOS_PLATFORM_CAP_LEN,
    TUSB_DESC_DEVICE_CAPABILITY,
    DEVICE_CAPABILITY_PLATFORM,
    0u,                                /* bReserved */
    /* UUID {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F} packed per Microsoft's
     * GUID binary layout (data1..3 are little-endian, data4 is verbatim). */
    0xDFu, 0x60u, 0xDDu, 0xD8u,
    0x89u, 0x45u,
    0xC7u, 0x4Cu,
    0x9Cu, 0xD2u, 0x65u, 0x9Du, 0x9Eu, 0x64u, 0x8Au, 0x9Fu,
    /* Platform capability data (8 B) */
    U32LE(MS_OS_20_WINDOWS_VERSION),
    U16LE(MS_OS_20_DESC_LEN),
    MS_OS_20_VENDOR_CODE,
    0u,                                /* bAltEnumCode */
};

const size_t desc_bos_len = sizeof(desc_bos);
