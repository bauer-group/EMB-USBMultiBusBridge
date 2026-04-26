/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Session init helpers — used once during probe() to bring the
 * device into a known state and harvest its capability map.
 */

#ifndef RPBRIDGE_SESSION_H
#define RPBRIDGE_SESSION_H

struct rpbridge_dev;

/**
 * rpbridge_session_hello() - exchange MSG_HELLO frames.
 * @dev: per-device state, freshly minted by probe().
 *
 * RPBP v1 §4.1 mandates a HELLO exchange before any other traffic.
 * Behaves as a sanity check that the bulk pair is the right one
 * and that the firmware speaks the same MAJOR version we do.
 *
 * Returns 0 on success; the same -E* surface as
 * rpbridge_round_trip() on failure.
 */
int rpbridge_session_hello(struct rpbridge_dev *dev);

/**
 * rpbridge_session_fetch_caps() - run SYS,GET_CAPABILITIES, parse and
 * stash the resulting capability map in @dev->caps.
 * @dev: per-device state.
 *
 * Allocates dev->caps via kzalloc; the kref release callback frees
 * it. On error, dev->caps stays NULL.
 *
 * Returns 0 on success or a negative errno.
 */
int rpbridge_session_fetch_caps(struct rpbridge_dev *dev);

#endif /* RPBRIDGE_SESSION_H */
