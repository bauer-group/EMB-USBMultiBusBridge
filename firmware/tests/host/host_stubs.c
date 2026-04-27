/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * Host-test stubs: the portable RPBP dispatch layer references a
 * handful of pico-sdk + firmware symbols that do not exist off-
 * target. This TU provides minimal stand-ins so `rpbp_dispatch.c`
 * and `rpbp_stream.c` link cleanly into host-only test binaries.
 *
 * The stubs are deliberately simple. Tests that need richer
 * behaviour override them via a `__weak`-style linker trick or by
 * not linking this file at all.
 */

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#include <stdint.h>

/* ───── pico-sdk shims ───────────────────────────────────────────────── */

/*
 * pico/time.h provides a microsecond counter via time_us_64(). The
 * dispatcher reads it for the timestamp field on every outgoing
 * frame; tests don't care about wall time, so return a monotonic
 * counter that increments per call. Predictable + easy to assert.
 */
uint64_t time_us_64(void);
uint64_t time_us_64(void)
{
    static uint64_t counter = 0u;
    return counter++;
}

/* ───── Subsystem dispatcher stubs ───────────────────────────────────── */

/*
 * Each stub sends a trivial CMD_RESPONSE so the dispatcher's call
 * path is exercised end-to-end. Tests that want to assert the exact
 * subsystem/opcode values just inspect the captured TX frame.
 */

#define CMD_STUB_IMPL(name, subsys_id)                                      \
    enum rpbp_status name(const struct rpbp_header *req,                    \
                          uint8_t opcode,                                   \
                          const uint8_t *args, size_t args_len,             \
                          bool cbor)                                        \
    {                                                                       \
        (void)args; (void)args_len; (void)cbor;                             \
        rpbp_reply_cmd(req, subsys_id, opcode, RPBP_OK, NULL, 0, false);    \
        return RPBP_OK;                                                     \
    }

CMD_STUB_IMPL(cmd_sys_dispatch,  RPBP_SUBSYS_SYS)
CMD_STUB_IMPL(cmd_i2c_dispatch,  RPBP_SUBSYS_I2C)
CMD_STUB_IMPL(cmd_spi_dispatch,  RPBP_SUBSYS_SPI)
CMD_STUB_IMPL(cmd_gpio_dispatch, RPBP_SUBSYS_GPIO)
CMD_STUB_IMPL(cmd_pwm_dispatch,  RPBP_SUBSYS_PWM)
CMD_STUB_IMPL(cmd_adc_dispatch,  RPBP_SUBSYS_ADC)
CMD_STUB_IMPL(cmd_uart_dispatch, RPBP_SUBSYS_UART)

/*
 * cmd_sys_reply_hello sends a HELLO reply in the real firmware. The
 * stub emits an empty-payload HELLO frame — enough to drive the
 * dispatcher's "session open" state machine in tests.
 */
int cmd_sys_reply_hello(const struct rpbp_header *req);
int cmd_sys_reply_hello(const struct rpbp_header *req)
{
    struct rpbp_header hdr = {
        .msg_type     = RPBP_MT_HELLO,
        .flags        = 0,
        .channel      = req->channel,
        .seq          = req->seq,
        .timestamp_us = 0u,
    };
    uint8_t frame[RPBP_HEADER_LEN + RPBP_CRC_LEN];
    size_t  n = rpbp_frame_build(frame, &hdr, NULL, 0u);
    if (n == 0u) return -1;

    struct rpbp_session *s = rpbp_session();
    if (s == NULL || s->tx == NULL) return -1;
    return s->tx(frame, n);
}
