/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * CRC-32C (Castagnoli polynomial 0x1EDC6F41).
 *
 * Matches the crc32c() routine in <linux/crc32c.h> and the SSE4.2
 * `crc32` instruction on x86_64 — so host and device agree bit-for-bit.
 *
 * Two backends are provided:
 *   - A portable software table (single 1 kB table, ~8 cycles/byte on M33).
 *   - (future) An RP2350 DMA SNIFFER path once the firmware has a DMA
 *     channel free for it; not implemented in this scaffold.
 */

#ifndef RPBP_CRC32C_H
#define RPBP_CRC32C_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Init value for a new CRC: 0xFFFFFFFF (per RFC 3720 §B.4). The final
 * return from rpbp_crc32c_finish() XORs 0xFFFFFFFF as required. */
#define RPBP_CRC32C_INIT  0xFFFFFFFFu

uint32_t rpbp_crc32c_update(uint32_t crc, const uint8_t *buf, size_t len);

static inline uint32_t rpbp_crc32c(const uint8_t *buf, size_t len)
{
    return rpbp_crc32c_update(RPBP_CRC32C_INIT, buf, len) ^ 0xFFFFFFFFu;
}

#ifdef __cplusplus
}
#endif
#endif /* RPBP_CRC32C_H */
