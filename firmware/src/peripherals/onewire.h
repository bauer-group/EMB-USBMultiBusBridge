/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * onewire — 1-Wire (Maxim/Dallas) bus master via a PIO state machine.
 *
 * The PIO program issues one bit-time slot per FIFO command and
 * pushes the sampled bus level back. All byte-level work (8-bit
 * read / write, ROM SEARCH triplet) is composed here in C.
 *
 * Standard speed only (~70 µs per bit slot, ~1 ms reset). Overdrive
 * speed is a future extension — needs a different PIO clkdiv and
 * the host opt-in.
 *
 * The driver is non-reentrant: only one byte-level operation at a
 * time. The RPBP opcodes serialise naturally through the dispatcher.
 */

#ifndef RPBRIDGE_ONEWIRE_H
#define RPBRIDGE_ONEWIRE_H

#include "pico.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct onewire_info {
    uint8_t  count;        /* bus count (always 1 in v1)              */
    uint8_t  pin;          /* GPIO carrying the data line             */
    uint8_t  speed;        /* 0 = standard (only option in v1)        */
    uint8_t  reserved;
};

/*
 * Bring the 1-Wire driver online: claims PIO1 SM 2, loads the
 * onewire program, claims RPBRIDGE_ONEWIRE_PIN. Idempotent.
 *
 * Returns 0 on success, -RPBP_EBUSY on pin/PIO conflict,
 * -RPBP_ENOTSUP if the build does not include 1-Wire support.
 */
int onewire_init(void);

/*
 * Issue a 1-Wire reset and sample for a presence pulse. Returns:
 *   1  on presence detected
 *   0  on no presence (bus empty or all slaves silent)
 *  -RPBP_EAGAIN if the driver is not initialised
 */
int onewire_reset_present(void);

/* Write one byte LSB-first. Returns 0 on success, -RPBP_EAGAIN
 * if the driver is not initialised. */
int onewire_write_byte(uint8_t b);

/* Read one byte LSB-first. Returns the byte (0..255) on success,
 * < 0 on error. */
int onewire_read_byte(void);

/* Write `len` bytes; returns 0 on success or the first error. */
int onewire_write(const uint8_t *bytes, size_t len);

/* Read `len` bytes into `dst`; returns 0 on success. */
int onewire_read(uint8_t *dst, size_t len);

/*
 * SEARCH-ROM triplet — read two bits (true and complement),
 * decide a direction, write that direction. Returns the encoded
 * tuple in the low 3 bits:
 *   bit 0 : id_bit (the read true bit)
 *   bit 1 : cmp_id_bit (the read complement bit)
 *   bit 2 : search_direction (the bit that was actually written)
 *
 * `direction` is either 0 or 1 — the host's choice for this slot
 * (typically driven by Maxim AN187 SEARCH ROM state machine).
 *
 * Returns -RPBP_EAGAIN if the driver is not initialised.
 */
int onewire_triplet(uint8_t direction);

void onewire_get_info(struct onewire_info *out);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_ONEWIRE_H */
