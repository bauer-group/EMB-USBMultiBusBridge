/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * ipc_queue — lock-free SPSC ring for inter-core and IRQ → main-loop
 * message passing. Fixed-size slots, power-of-two capacity.
 *
 * Concurrency model
 * ─────────────────
 *   Exactly ONE producer + exactly ONE consumer. Producer writes the
 *   slot before advancing `head`; consumer reads the slot after
 *   sampling `head` and before advancing `tail`. A `__DMB()` between
 *   slot-write and head-store, and between head-sample and slot-read,
 *   gives the necessary acquire/release semantics on Cortex-M33.
 *
 * Safe call sites
 * ───────────────
 *   Producer: PIO IRQ, timer IRQ, Core 1 main, USB OUT callback …
 *   Consumer: Core 0 main loop, Core 1 scheduler …
 *
 *   It is NOT safe to use the same queue endpoint from two producers
 *   or two consumers — wrap a mutex if you need that.
 */

#ifndef RPBRIDGE_IPC_QUEUE_H
#define RPBRIDGE_IPC_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "hardware/sync.h"             /* __dmb() barrier */

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_queue {
    uint8_t  *buf;
    uint16_t  slot_size;
    uint16_t  num_slots;        /* must be a power of two */
    uint16_t  mask;             /* num_slots - 1 */
    volatile uint16_t head;     /* write index (producer) */
    volatile uint16_t tail;     /* read  index (consumer) */
};

/*
 * Initialise `q` to back onto `storage` which must be at least
 * `num_slots * slot_size` bytes and `num_slots` must be a power of
 * two. Not thread-safe — call before any producer/consumer activity.
 */
static inline void ipc_queue_init(struct ipc_queue *q,
                                  void *storage,
                                  uint16_t slot_size,
                                  uint16_t num_slots)
{
    q->buf       = (uint8_t *)storage;
    q->slot_size = slot_size;
    q->num_slots = num_slots;
    q->mask      = (uint16_t)(num_slots - 1u);
    q->head      = 0u;
    q->tail      = 0u;
}

static inline uint16_t ipc_queue_count(const struct ipc_queue *q)
{
    return (uint16_t)((q->head - q->tail) & q->mask);
}

static inline bool ipc_queue_empty(const struct ipc_queue *q)
{
    return q->head == q->tail;
}

static inline bool ipc_queue_full(const struct ipc_queue *q)
{
    return ((uint16_t)(q->head + 1u) & q->mask) == q->tail;
}

/*
 * Producer-side push. Returns false if the queue is full (no blocking,
 * no waiting — the producer decides how to handle backpressure).
 */
static inline bool ipc_queue_push(struct ipc_queue *q, const void *item)
{
    uint16_t next = (uint16_t)((q->head + 1u) & q->mask);
    if (next == q->tail) return false;

    memcpy(&q->buf[q->head * q->slot_size], item, q->slot_size);
    __dmb();                              /* slot written before head publish */
    q->head = next;
    return true;
}

/*
 * Consumer-side pop. Returns false if the queue is empty.
 */
static inline bool ipc_queue_pop(struct ipc_queue *q, void *item)
{
    if (q->tail == q->head) return false;

    __dmb();                              /* head sampled before slot read */
    memcpy(item, &q->buf[q->tail * q->slot_size], q->slot_size);
    q->tail = (uint16_t)((q->tail + 1u) & q->mask);
    return true;
}

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_IPC_QUEUE_H */
