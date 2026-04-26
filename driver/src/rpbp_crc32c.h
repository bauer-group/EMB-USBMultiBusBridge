/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CRC-32C (Castagnoli) — kept locally so the wire-level frame check
 * does not depend on whether <linux/crc32c.h> is enabled in the
 * target kernel config. Polynomial 0x1EDC6F41 (reflected
 * 0x82F63B78), init 0xFFFFFFFF, final XOR 0xFFFFFFFF. Matches the
 * firmware's firmware/src/rpbp/rpbp_crc32c.c byte-for-byte; the
 * shared golden-vector corpus under tests/golden/ pins both sides.
 */

#ifndef RPBRIDGE_RPBP_CRC32C_H
#define RPBRIDGE_RPBP_CRC32C_H

#include <linux/types.h>

#define RPBP_CRC32C_INIT  0xFFFFFFFFu

/**
 * rpbp_crc32c_update() - feed @len bytes into a running accumulator.
 * @crc: previous return value, or %RPBP_CRC32C_INIT for a fresh stream.
 * @buf: input bytes; may be %NULL iff @len == 0.
 * @len: number of bytes at @buf.
 *
 * Returns the updated accumulator. Caller XORs with 0xFFFFFFFF to
 * obtain the final CRC; rpbp_crc32c() does that in one shot.
 */
u32 rpbp_crc32c_update(u32 crc, const u8 *buf, size_t len);

/**
 * rpbp_crc32c() - one-shot CRC-32C of a contiguous buffer.
 * @buf: input bytes; may be %NULL iff @len == 0.
 * @len: number of bytes at @buf.
 */
static inline u32 rpbp_crc32c(const u8 *buf, size_t len)
{
	return rpbp_crc32c_update(RPBP_CRC32C_INIT, buf, len) ^ 0xFFFFFFFFu;
}

#endif /* RPBRIDGE_RPBP_CRC32C_H */
