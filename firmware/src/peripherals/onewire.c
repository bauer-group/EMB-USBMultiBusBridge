/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * onewire — Maxim 1-Wire master on PIO1 SM 2.
 */

#include "onewire.h"

#include "pin_broker.h"
#include "rpbp/rpbp.h"

#include "pico.h"

#if defined(RPBRIDGE_HAVE_ONEWIRE) && RPBRIDGE_HAVE_ONEWIRE

#include "hardware/pio.h"
#include "hardware/clocks.h"

#include "onewire.pio.h"

#include <string.h>

#ifndef RPBRIDGE_ONEWIRE_PIN
#  error "RPBRIDGE_ONEWIRE_PIN must be defined by the active board header"
#endif

#define ONEWIRE_PIO     pio1

/* PIO command-word bits (mirrors onewire.pio header). */
#define OW_CMD_RESET    (1u << 1)
#define OW_CMD_BIT_1    (1u << 0)
#define OW_CMD_BIT_0    (0u)

struct onewire_state {
    uint     sm;
    uint     program_offset;
    bool     initialised;
};

static struct onewire_state g_ow;

/* Issue one PIO command + read the result bit. Blocking — slot
 * timing is 70 µs (bit) or ~1 ms (reset), comfortably inside our
 * RPBP per-request budget. */
static uint32_t ow_xchg(uint32_t cmd)
{
    pio_sm_put_blocking(ONEWIRE_PIO, g_ow.sm, cmd);
    return pio_sm_get_blocking(ONEWIRE_PIO, g_ow.sm) & 1u;
}

int onewire_init(void)
{
    if (g_ow.initialised) return 0;

    if (pin_broker_claim(RPBRIDGE_ONEWIRE_PIN,
                         PIN_OWNER_ONE_WIRE, PIN_FUNC_PIO) < 0) {
        return -RPBP_EBUSY;
    }

    PIO pio = ONEWIRE_PIO;
    if (!pio_can_add_program(pio, &rpbridge_onewire_program)) {
        pin_broker_release(RPBRIDGE_ONEWIRE_PIN, PIN_OWNER_ONE_WIRE);
        return -RPBP_EBUSY;
    }
    g_ow.program_offset = pio_add_program(pio, &rpbridge_onewire_program);

    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        pin_broker_release(RPBRIDGE_ONEWIRE_PIN, PIN_OWNER_ONE_WIRE);
        return -RPBP_EBUSY;
    }
    g_ow.sm = (uint)sm;

    rpbridge_onewire_program_init(pio, g_ow.sm,
                                  g_ow.program_offset,
                                  RPBRIDGE_ONEWIRE_PIN);

    g_ow.initialised = true;
    return 0;
}

int onewire_reset_present(void)
{
    if (!g_ow.initialised) return -RPBP_EAGAIN;
    /* PIO returns 0 = presence pulse detected, 1 = none.
     * Higher layer expects "1 = present" so invert. */
    uint32_t r = ow_xchg(OW_CMD_RESET);
    return (r == 0u) ? 1 : 0;
}

int onewire_write_byte(uint8_t b)
{
    if (!g_ow.initialised) return -RPBP_EAGAIN;
    /* LSB first per Maxim AN126. */
    for (int i = 0; i < 8; ++i) {
        uint32_t cmd = (b & 1u) ? OW_CMD_BIT_1 : OW_CMD_BIT_0;
        (void)ow_xchg(cmd);
        b >>= 1;
    }
    return 0;
}

int onewire_read_byte(void)
{
    if (!g_ow.initialised) return -RPBP_EAGAIN;
    uint8_t b = 0;
    for (int i = 0; i < 8; ++i) {
        uint32_t r = ow_xchg(OW_CMD_BIT_1);  /* READ slot = WRITE_1 + sample */
        if (r & 1u) b |= (uint8_t)(1u << i);
    }
    return (int)b;
}

int onewire_write(const uint8_t *bytes, size_t len)
{
    if (bytes == NULL && len > 0u) return -RPBP_EINVAL;
    for (size_t i = 0; i < len; ++i) {
        int rc = onewire_write_byte(bytes[i]);
        if (rc < 0) return rc;
    }
    return 0;
}

int onewire_read(uint8_t *dst, size_t len)
{
    if (dst == NULL && len > 0u) return -RPBP_EINVAL;
    for (size_t i = 0; i < len; ++i) {
        int v = onewire_read_byte();
        if (v < 0) return v;
        dst[i] = (uint8_t)v;
    }
    return 0;
}

int onewire_triplet(uint8_t direction)
{
    if (!g_ow.initialised) return -RPBP_EAGAIN;
    /* Maxim AN187: read two bits (id, ~id), then write the
     * search_direction. The triplet is the building block of the
     * SEARCH-ROM state machine. */
    uint8_t id_bit  = (uint8_t)ow_xchg(OW_CMD_BIT_1);
    uint8_t cmp_bit = (uint8_t)ow_xchg(OW_CMD_BIT_1);
    uint8_t dir     = (uint8_t)(direction & 1u);
    (void)ow_xchg(dir ? OW_CMD_BIT_1 : OW_CMD_BIT_0);
    return (int)((id_bit & 1u)
               | ((cmp_bit & 1u) << 1)
               | ((dir & 1u) << 2));
}

void onewire_get_info(struct onewire_info *out)
{
    if (out == NULL) return;
    out->count    = g_ow.initialised ? 1u : 0u;
    out->pin      = RPBRIDGE_ONEWIRE_PIN;
    out->speed    = 0u;            /* standard speed */
    out->reserved = 0u;
}

#else  /* !RPBRIDGE_HAVE_ONEWIRE */

int onewire_init(void)                                 { return -RPBP_ENOTSUP; }
int onewire_reset_present(void)                        { return -RPBP_ENOTSUP; }
int onewire_write_byte(uint8_t b)                      { (void)b; return -RPBP_ENOTSUP; }
int onewire_read_byte(void)                            { return -RPBP_ENOTSUP; }
int onewire_write(const uint8_t *b, size_t l)          { (void)b; (void)l; return -RPBP_ENOTSUP; }
int onewire_read(uint8_t *b, size_t l)                 { (void)b; (void)l; return -RPBP_ENOTSUP; }
int onewire_triplet(uint8_t d)                         { (void)d; return -RPBP_ENOTSUP; }
void onewire_get_info(struct onewire_info *out)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
}

#endif /* RPBRIDGE_HAVE_ONEWIRE */
