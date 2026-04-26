// SPDX-License-Identifier: GPL-2.0
/*
 * Capability-map decoder — initial scaffold.
 *
 * The full CBOR walker (RFC 8949 §3) lands in a follow-up commit so
 * each commit stays one logical unit. For now the function clears
 * @out and returns 0, leaving the driver to attach with empty caps;
 * subsystem children are not yet spawned in this baseline anyway.
 *
 * The function signature, output struct and bounds in cap.h are
 * already stable so the parser drop-in won't perturb callers.
 */

#include <linux/errno.h>
#include <linux/string.h>

#include "cap.h"

int rpbp_caps_parse(const u8 *cbor, size_t len, struct rpbp_caps *out)
{
	if (!out)
		return -EINVAL;
	if (len > 0 && !cbor)
		return -EINVAL;

	/*
	 * Spec ceiling. Anything larger is malformed by definition;
	 * cap here so a hostile device can't make us walk a 4 GB blob
	 * even before the per-key bounds checks fire.
	 */
	if (len > 4096)
		return -EMSGSIZE;

	memset(out, 0, sizeof(*out));

	/* TODO(rpbp): full CBOR walker (RFC 8949) lands in next commit.
	 * Until then, "no caps" is a safe default — the bus_count
	 * stays 0, no auxiliary children get spawned, and userspace
	 * sees an attached-but-empty device. */
	(void)cbor;
	(void)len;
	return 0;
}
