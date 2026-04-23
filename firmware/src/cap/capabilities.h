/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * Capability-map CBOR emitter. Schema: docs/protocol/capabilities.md.
 *
 * The emitter writes into a caller-provided buffer. Callers check
 * cbor_writer_ok() on the embedded writer to learn whether the buffer
 * was large enough; 2 kB is ample for the v1 scaffold.
 */

#ifndef RPBRIDGE_CAPABILITIES_H
#define RPBRIDGE_CAPABILITIES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Emit the full capability map into `buf`. Returns number of bytes
 * written on success, or 0 on buffer overflow.
 */
size_t rpbridge_capabilities_emit(uint8_t *buf, size_t cap);

/*
 * Emit the compact identity-only map (used by SYS GET_IDENTITY).
 * Same behaviour as the full emitter but omits every non-identity
 * field.
 */
size_t rpbridge_identity_emit(uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CAPABILITIES_H */
