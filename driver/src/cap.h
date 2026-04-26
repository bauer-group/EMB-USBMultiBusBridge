/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Capability-map decoder. The device returns CBOR in response to
 * CMD_REQUEST{SYS, GET_CAPABILITIES}; we parse it once at probe()
 * time, store the relevant bits, and free the structure on the
 * driver's kref release.
 */

#ifndef RPBRIDGE_CAP_H
#define RPBRIDGE_CAP_H

#include <linux/types.h>

/*
 * Maximum supported counts per subsystem. Tight limits are a
 * defense-in-depth measure: a malicious or corrupted device cannot
 * make the driver allocate unbounded memory at probe time.
 */
#define RPBP_CAPS_MAX_BUSES         16u
#define RPBP_CAPS_MAX_FEATURES      32u
#define RPBP_CAPS_MAX_CLASS_LEN     16u  /* "onewire" is the longest today */
#define RPBP_CAPS_MAX_FW_LEN        32u
#define RPBP_CAPS_MAX_BOARD_LEN     32u
#define RPBP_CAPS_MAX_SERIAL_LEN    32u

struct rpbp_caps_bus {
	char  class[RPBP_CAPS_MAX_CLASS_LEN];  /* NUL-terminated. */
	u16   idx;
	u32   max_freq_hz;                     /* 0 = unspecified */
};

struct rpbp_caps {
	u8                    proto[3];        /* major.minor.patch */
	char                  fw[RPBP_CAPS_MAX_FW_LEN];
	char                  board[RPBP_CAPS_MAX_BOARD_LEN];
	u8                    serial[RPBP_CAPS_MAX_SERIAL_LEN];
	size_t                serial_len;

	u32                   mtu_in;
	u32                   mtu_out;

	struct rpbp_caps_bus  buses[RPBP_CAPS_MAX_BUSES];
	size_t                bus_count;
};

/**
 * rpbp_caps_parse() - decode a CBOR capability map.
 * @cbor:   payload bytes from CMD_RESPONSE.
 * @len:    number of bytes at @cbor.
 * @out:    pre-allocated, zero-filled output.
 *
 * Returns 0 on success or a negative errno on malformed input.
 * Unknown keys are skipped (forward-compat); structurally invalid
 * CBOR returns -EPROTO.
 */
int rpbp_caps_parse(const u8 *cbor, size_t len, struct rpbp_caps *out);

#endif /* RPBRIDGE_CAP_H */
