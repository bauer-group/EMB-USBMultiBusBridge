/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * Microsoft OS 2.0 descriptor set + BOS descriptor.
 *
 * When Windows 10 (version 1709+) enumerates a USB device that carries
 * a BOS containing an MS OS 2.0 platform capability, it fetches our
 * MS OS 2.0 descriptor set via a vendor control-transfer. Entries in
 * that set tell Windows:
 *
 *   - interface 6 (RPBP control) binds to WinUSB with a project-
 *     specific DeviceInterfaceGUID;
 *   - interface 7 (gs_usb CAN) binds to WinUSB with a separate
 *     DeviceInterfaceGUID.
 *
 * No INF file, no signed driver, no user intervention. Linux ignores
 * BOS + MS OS 2.0 silently, so the firmware stays single-binary.
 *
 * References:
 *   - Microsoft OS 2.0 Descriptors Specification, rev 1.0 (June 2016)
 *   - USB 3.2 spec §9.6.2 (BOS descriptor)
 */

#ifndef MS_OS_20_H
#define MS_OS_20_H

#include <stdint.h>
#include <stddef.h>

/* Vendor request code the host uses when asking for the descriptor
 * set. Advertised to Windows through the platform-capability payload
 * in the BOS descriptor. Value is arbitrary but MUST NOT collide with
 * other vendor control requests the firmware handles. */
#define MS_OS_20_VENDOR_CODE        0x02u

/* wIndex of the MS OS 2.0 vendor request — fixed by the spec. */
#define MS_OS_20_DESCRIPTOR_INDEX   0x0007u

/* Descriptor blobs — byte layouts match the MS OS 2.0 spec sections
 * 4 (BOS) and 4–10 (descriptor set). */
extern const uint8_t desc_ms_os_20[];
extern const size_t  desc_ms_os_20_len;

extern const uint8_t desc_bos[];
extern const size_t  desc_bos_len;

#endif /* MS_OS_20_H */
