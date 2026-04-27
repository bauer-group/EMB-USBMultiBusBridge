/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * SYS subsystem handlers — implementation of the opcode table in
 * docs/protocol/subsystems/sys.md.
 */

#include "cmd_sys.h"

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_framing.h"
#include "rpbp/rpbp_dispatch.h"
#include "rpbp/rpbp_cbor.h"
#include "capabilities/capabilities.h"
#include "version.h"

#include "rpbp/rpbp_crc32c.h"
#include "peripherals/pin_broker.h"
#include "peripherals/i2c_hw.h"
#include "peripherals/spi.h"
#include "peripherals/pwm.h"
#include "peripherals/adc.h"
#include "peripherals/can.h"
#include "io_control/io_control.h"

#include "pico/time.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"

#include <string.h>

/* ───── LED override state (consumed by main loop) ───────────────────── */

static struct {
    bool     active;
    uint8_t  r, g, b;
    uint8_t  mode;           /* 0=off, 1=solid, 2=blink, 3=pulse, 4=fast */
    uint8_t  brightness_pct; /* 0..100 */
} g_led_override;

bool cmd_sys_get_led_override(struct cmd_sys_led_override *out)
{
    if (!g_led_override.active) return false;
    if (out != NULL) {
        out->r              = g_led_override.r;
        out->g              = g_led_override.g;
        out->b              = g_led_override.b;
        out->mode           = g_led_override.mode;
        out->brightness_pct = g_led_override.brightness_pct;
    }
    return true;
}

/* ───── UART claim state (consumed by cdc_glue pump path) ────────────── */

/*
 * 2-bit mask: bit 0 = UART0 HW, bit 1 = UART2 PIO.
 * Set via SYS_UART_CLAIM, cleared via SYS_UART_RELEASE. Operations are
 * idempotent (claiming an already-claimed UART returns OK, releasing
 * a free UART returns OK). The instance count is fixed by the board's
 * UART layout — no dynamic allocation.
 */
#define CMD_SYS_UART_INSTANCE_COUNT  2u
static volatile uint8_t g_uart_claim_mask;

bool cmd_sys_uart_claimed(uint8_t uart_idx)
{
    if (uart_idx >= CMD_SYS_UART_INSTANCE_COUNT) return false;
    return (g_uart_claim_mask >> uart_idx) & 1u;
}

int cmd_sys_set_led(uint8_t r, uint8_t g, uint8_t b,
                    uint8_t mode, uint8_t brightness_pct)
{
    if (mode > 4) {
        return -1;
    }
    if (brightness_pct > 100) brightness_pct = 100;

    if (mode == 0) {
        /* mode 0 releases the override so the firmware state machine
         * resumes control (heartbeat green). */
        g_led_override.active = false;
        return 0;
    }
    g_led_override.active         = true;
    g_led_override.r              = r;
    g_led_override.g              = g;
    g_led_override.b              = b;
    g_led_override.mode           = mode;
    g_led_override.brightness_pct = brightness_pct;
    return 0;
}

/* ───── HELLO ────────────────────────────────────────────────────────── */

int cmd_sys_reply_hello(const struct rpbp_header *req)
{
    /*
     * HELLO reply is a CBOR map carrying identity and protocol version.
     * It reuses the rpbridge_identity_emit() emitter for compactness;
     * nonce mirroring is not implemented in the scaffold (host-supplied
     * nonce from the request is not parsed — CBOR reader would add
     * weight and the nonce is best verified host-side via attestation
     * once that lands).
     */
    uint8_t buf[256];
    size_t  used = rpbridge_identity_emit(buf, sizeof(buf));
    if (used == 0)
        return -1;

    struct rpbp_header hdr = {
        .msg_type     = RPBP_MT_HELLO,
        .flags        = RPBP_FL_CBOR,
        .channel      = req->channel,
        .seq          = req->seq,
        .timestamp_us = (uint32_t)time_us_64(),
    };

    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  n = rpbp_frame_build(frame, &hdr, buf, used);
    if (n == 0) return -1;

    /* Piggy-back on the dispatcher's registered TX function. */
    struct rpbp_session *s = rpbp_session();
    return (s && s->tx) ? s->tx(frame, n) : -1;
}

/* ───── GET_CAPABILITIES ─────────────────────────────────────────────── */

static enum rpbp_status op_get_capabilities(const struct rpbp_header *req,
                                            bool cbor)
{
    (void)cbor;  /* capability response is CBOR-only regardless */

    uint8_t caps[2048];
    size_t  caps_len = rpbridge_capabilities_emit(caps, sizeof(caps));
    if (caps_len == 0) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_CAPABILITIES,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    /*
     * Wrap the capability map in the canonical CMD_RESPONSE CBOR shape:
     *   { "s": 0, "o": 0, "st": 0, "r": <capability-map> }
     *
     * We build the outer wrapper in a second buffer and inline the
     * caps bytes as pre-encoded CBOR.
     */
    uint8_t out[2048 + 32];
    struct cbor_writer w;
    cbor_writer_init(&w, out, sizeof(out));

    cbor_writer_map(&w, 4);
    cbor_writer_kv_uint(&w, "s",  RPBP_SUBSYS_SYS);
    cbor_writer_kv_uint(&w, "o",  RPBP_SYS_GET_CAPABILITIES);
    cbor_writer_kv_uint(&w, "st", RPBP_OK);
    cbor_writer_text_cstr(&w, "r");

    /* Inline: copy the pre-built CBOR capability map as raw bytes. */
    if (w.pos + caps_len > w.cap) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_CAPABILITIES,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    memcpy(&w.buf[w.pos], caps, caps_len);
    w.pos += caps_len;

    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_CAPABILITIES,
                   RPBP_OK, w.buf, w.pos, true);
    return RPBP_OK;
}

/* ───── ECHO ─────────────────────────────────────────────────────────── */

static enum rpbp_status op_echo(const struct rpbp_header *req,
                                const uint8_t *args, size_t args_len)
{
    /* Echo back the exact byte sequence; status=OK. */
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_ECHO, RPBP_OK,
                   args, args_len, false);
    return RPBP_OK;
}

/* ───── REBOOT_BOOTSEL ───────────────────────────────────────────────── */

static enum rpbp_status op_reboot_bootsel(const struct rpbp_header *req)
{
    /* Reply FIRST so the host sees the ACK before the USB link drops. */
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_REBOOT_BOOTSEL, RPBP_OK,
                   NULL, 0, false);

    /* Give TinyUSB a few ms to push the frame onto the wire. */
    sleep_ms(50);

    /* Arg 0: activity-LED GPIO mask (0 = firmware did it); arg 1: 0
     * = default behaviour (both MSC and PICOBOOT on). */
    reset_usb_boot(0, 0);
    /* unreachable */
    return RPBP_OK;
}

/* ───── UPTIME ───────────────────────────────────────────────────────── */

static enum rpbp_status op_uptime(const struct rpbp_header *req)
{
    uint64_t us = time_us_64();
    uint8_t  buf[8];
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)((us >> (8 * i)) & 0xFFu);
    }
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_UPTIME, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── GET_VBUS_MV ──────────────────────────────────────────────────── */

static enum rpbp_status op_get_vbus_mv(const struct rpbp_header *req)
{
    /*
     * Scaffold: return a nominal 5000 mV. The real implementation
     * reads GP46 through a 100k/49.9k divider and an ADC, which lands
     * with the ADC subsystem handler.
     */
    uint16_t mv = 5000;
    uint8_t  buf[2] = {
        (uint8_t)(mv & 0xFFu),
        (uint8_t)((mv >> 8) & 0xFFu),
    };
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_VBUS_MV, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── SET_LED ──────────────────────────────────────────────────────── */

static enum rpbp_status op_set_led(const struct rpbp_header *req,
                                   const uint8_t *args, size_t args_len)
{
    if (args_len != 5) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_SET_LED,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = cmd_sys_set_led(args[0], args[1], args[2], args[3], args[4]);
    uint8_t status = (rc == 0) ? RPBP_OK : RPBP_EINVAL;
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_SET_LED, status,
                   NULL, 0, false);
    return RPBP_OK;
}

/* ───── SELFTEST ─────────────────────────────────────────────────────── */

/*
 * Bit positions in `test_mask` / `pass_mask`. Must stay stable across
 * MINOR version bumps — hosts identify tests by bit number. Add new
 * bits at the highest free position; never renumber.
 */
enum cmd_sys_test_bit {
    TEST_PIN_BROKER_INTEGRITY = 0,  /* broker ledger internally consistent */
    TEST_CRC32C_KNOWN_VECTOR  = 1,  /* RFC 3720 "123456789" == 0xE3069283  */
    TEST_CLK_SYS_IN_RANGE     = 2,  /* clk_sys within ±5 % of 150 MHz     */
    TEST_ADC_TEMP_SENSIBLE    = 3,  /* on-chip temp sensor between 5..85 °C */
    TEST_I2C0_BAUDRATE_SET    = 4,  /* I²C #0 has a non-zero baudrate     */
    TEST_SPI0_FREQ_APPLIED    = 5,  /* SPI #0 SET_FREQ round-trip matches  */
    TEST_PWM_LED_RUNNING      = 6,  /* LED R channel state == running      */
    TEST_CAN_INITIALISED      = 7,  /* can2040 reports a non-zero bitrate  */
    TEST_CORE1_ALIVE          = 8,  /* io_control counter advances in 2 ms */
    TEST_WATCHDOG_ENABLED     = 9,  /* watchdog ENABLE bit set             */

    TEST_BIT_COUNT            = 10,
};

#define TEST_ALL_MASK   ((1u << TEST_BIT_COUNT) - 1u)

/*
 * Individual test runners. Each returns true on pass. They MUST be
 * side-effect-free with respect to the live state of the firmware —
 * e.g. the SPI freq test saves + restores the previous baud.
 */

static bool selftest_pin_broker_integrity(void)
{
    struct pin_broker_entry entries[PIN_BROKER_NUM_PINS];
    size_t n = pin_broker_snapshot(entries, PIN_BROKER_NUM_PINS);
    if (n != PIN_BROKER_NUM_PINS) return false;

    for (size_t i = 0; i < n; ++i) {
        /* pin number must equal its table index */
        if (entries[i].pin != i) return false;
        /* an owner of NONE must have function UNKNOWN */
        if (entries[i].owner == PIN_OWNER_NONE &&
            entries[i].function != PIN_FUNC_UNKNOWN) return false;
        /* a live owner must have a concrete function tag */
        if (entries[i].owner != PIN_OWNER_NONE &&
            entries[i].function == PIN_FUNC_UNKNOWN) return false;
    }
    return true;
}

static bool selftest_crc32c(void)
{
    /* RFC 3720 Castagnoli reference vector. */
    const uint8_t v[] = "123456789";
    return rpbp_crc32c(v, sizeof(v) - 1u) == 0xE3069283u;
}

static bool selftest_clk_sys_range(void)
{
    uint32_t hz = clock_get_hz(clk_sys);
    /* Nominal 150 MHz on this board; accept ±5 %. */
    return hz > 142500000u && hz < 157500000u;
}

static bool selftest_adc_temp(void)
{
    int16_t cdeg;
    if (adc_hw_read_temp_cdegC(&cdeg) < 0) return false;
    /* Reasonable bench range: 5 °C .. 85 °C (= 500 .. 8500 cdegC). */
    return cdeg > 500 && cdeg < 8500;
}

static bool selftest_i2c0_baud(void)
{
    return i2c_hw_baudrate(0u) > 0u;
}

static bool selftest_spi0_freq(void)
{
    uint32_t saved = spi_hw_freq(0u);
    if (saved == 0u) return false;

    uint32_t applied = spi_hw_set_freq(0u, 1000000u);   /* 1 MHz */
    bool ok = applied >= 500000u && applied <= 2000000u;

    /* Restore the previous baud so we don't disturb in-flight work. */
    (void)spi_hw_set_freq(0u, saved);
    return ok;
}

static bool selftest_pwm_led(void)
{
    struct pwm_hw_state st;
    /* LED R lives on GP22 (see main.c led_init). */
    if (pwm_hw_state(22u, &st) < 0) return false;
    return st.running != 0u;
}

static bool selftest_can_init(void)
{
    return can_hw_bitrate() > 0u;
}

static bool selftest_core1_alive(void)
{
    struct io_control_stats a, b;
    io_control_stats(&a);
    /* Core 1's idle loop ticks at ~1 kHz. 2 ms is enough with margin. */
    sleep_ms(2);
    io_control_stats(&b);
    return b.core1_loops > a.core1_loops;
}

static bool selftest_watchdog(void)
{
    /* On RP2350 the watchdog CTRL register's ENABLE bit lives at bit 30.
     * Check it via watchdog_hw — already included by hardware/watchdog.h. */
    return (watchdog_hw->ctrl & (1u << 30)) != 0u;
}

/* Dispatch table — indexed by enum cmd_sys_test_bit. */
typedef bool (*cmd_sys_test_fn)(void);
static const cmd_sys_test_fn g_tests[TEST_BIT_COUNT] = {
    [TEST_PIN_BROKER_INTEGRITY] = selftest_pin_broker_integrity,
    [TEST_CRC32C_KNOWN_VECTOR]  = selftest_crc32c,
    [TEST_CLK_SYS_IN_RANGE]     = selftest_clk_sys_range,
    [TEST_ADC_TEMP_SENSIBLE]    = selftest_adc_temp,
    [TEST_I2C0_BAUDRATE_SET]    = selftest_i2c0_baud,
    [TEST_SPI0_FREQ_APPLIED]    = selftest_spi0_freq,
    [TEST_PWM_LED_RUNNING]      = selftest_pwm_led,
    [TEST_CAN_INITIALISED]      = selftest_can_init,
    [TEST_CORE1_ALIVE]          = selftest_core1_alive,
    [TEST_WATCHDOG_ENABLED]     = selftest_watchdog,
};

static enum rpbp_status op_selftest(const struct rpbp_header *req,
                                    const uint8_t *args, size_t args_len)
{
    if (args_len != 4) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_SELFTEST,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    uint32_t test_mask = (uint32_t)args[0]
                       | ((uint32_t)args[1] << 8)
                       | ((uint32_t)args[2] << 16)
                       | ((uint32_t)args[3] << 24);
    /* Any bit above our known range is silently ignored — hosts that
     * pass 0xFFFFFFFF get "all available". */
    test_mask &= TEST_ALL_MASK;

    uint32_t pass_mask = 0u;
    for (unsigned bit = 0; bit < TEST_BIT_COUNT; ++bit) {
        if ((test_mask & (1u << bit)) == 0u) continue;
        /* Feed the watchdog between tests — core1-alive spins 2 ms and
         * SPI round-trip can be tens of ms on a cold cache. */
        watchdog_update();
        if (g_tests[bit] != NULL && g_tests[bit]()) {
            pass_mask |= (1u << bit);
        }
    }

    /* Response: [pass_mask LE u32][fails u8 = 0] — no per-failure
     * blobs in v1. Hosts compare (test_mask & ~pass_mask) to find
     * which tests failed and can then issue subsystem-specific
     * diagnostics (I²C SCAN, SPI loopback, etc.). */
    uint8_t buf[5] = {
        (uint8_t)(pass_mask         & 0xFFu),
        (uint8_t)((pass_mask >> 8)  & 0xFFu),
        (uint8_t)((pass_mask >> 16) & 0xFFu),
        (uint8_t)((pass_mask >> 24) & 0xFFu),
        0u,
    };
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_SELFTEST, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── GET_IDENTITY ─────────────────────────────────────────────────── */

static enum rpbp_status op_get_identity(const struct rpbp_header *req)
{
    uint8_t id[256];
    size_t  n = rpbridge_identity_emit(id, sizeof(id));
    if (n == 0) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_IDENTITY,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    /* Wrap in { "s":0, "o":7, "st":0, "r": <identity-map> } */
    uint8_t out[384];
    struct cbor_writer w;
    cbor_writer_init(&w, out, sizeof(out));
    cbor_writer_map(&w, 4);
    cbor_writer_kv_uint(&w, "s",  RPBP_SUBSYS_SYS);
    cbor_writer_kv_uint(&w, "o",  RPBP_SYS_GET_IDENTITY);
    cbor_writer_kv_uint(&w, "st", RPBP_OK);
    cbor_writer_text_cstr(&w, "r");
    if (w.pos + n > w.cap) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_IDENTITY,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    memcpy(&w.buf[w.pos], id, n);
    w.pos += n;

    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_IDENTITY, RPBP_OK,
                   w.buf, w.pos, true);
    return RPBP_OK;
}

/* ───── RESET ────────────────────────────────────────────────────────── */

static enum rpbp_status op_reset(const struct rpbp_header *req,
                                 const uint8_t *args, size_t args_len)
{
    if (args_len != 1) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_RESET,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t delay_ms = args[0];
    if (delay_ms > 200) delay_ms = 200;

    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_RESET, RPBP_OK,
                   NULL, 0, false);

    sleep_ms(delay_ms);
    watchdog_reboot(0, 0, 0);
    /* unreachable */
    return RPBP_OK;
}

/* ───── UART_CLAIM / UART_RELEASE ────────────────────────────────────── */

static enum rpbp_status op_uart_claim(const struct rpbp_header *req,
                                      const uint8_t *args, size_t args_len)
{
    if (args_len != 1) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_UART_CLAIM,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t uart_idx = args[0];
    if (uart_idx >= CMD_SYS_UART_INSTANCE_COUNT) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_UART_CLAIM,
                       RPBP_EINVAL, NULL, 0, false);
        return RPBP_OK;
    }
    /* Idempotent: re-claim by the same logical owner is a no-op. RPBP
     * has no session/identity tracking in v1 so any host can claim —
     * intentional; coordination is a higher-layer concern. */
    g_uart_claim_mask |= (uint8_t)(1u << uart_idx);
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_UART_CLAIM,
                   RPBP_OK, NULL, 0, false);
    return RPBP_OK;
}

static enum rpbp_status op_uart_release(const struct rpbp_header *req,
                                        const uint8_t *args, size_t args_len)
{
    if (args_len != 1) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_UART_RELEASE,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t uart_idx = args[0];
    if (uart_idx >= CMD_SYS_UART_INSTANCE_COUNT) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_UART_RELEASE,
                       RPBP_EINVAL, NULL, 0, false);
        return RPBP_OK;
    }
    g_uart_claim_mask &= (uint8_t)~(1u << uart_idx);
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_UART_RELEASE,
                   RPBP_OK, NULL, 0, false);
    return RPBP_OK;
}

/* ───── Dispatch ─────────────────────────────────────────────────────── */

enum rpbp_status cmd_sys_dispatch(const struct rpbp_header *req,
                                  uint8_t opcode,
                                  const uint8_t *args, size_t args_len,
                                  bool cbor)
{
    switch (opcode) {
    case RPBP_SYS_GET_CAPABILITIES: return op_get_capabilities(req, cbor);
    case RPBP_SYS_ECHO:             return op_echo(req, args, args_len);
    case RPBP_SYS_REBOOT_BOOTSEL:   return op_reboot_bootsel(req);
    case RPBP_SYS_UPTIME:           return op_uptime(req);
    case RPBP_SYS_GET_VBUS_MV:      return op_get_vbus_mv(req);
    case RPBP_SYS_SET_LED:          return op_set_led(req, args, args_len);
    case RPBP_SYS_SELFTEST:         return op_selftest(req, args, args_len);
    case RPBP_SYS_GET_IDENTITY:     return op_get_identity(req);
    case RPBP_SYS_RESET:            return op_reset(req, args, args_len);
    case RPBP_SYS_UART_CLAIM:       return op_uart_claim(req, args, args_len);
    case RPBP_SYS_UART_RELEASE:     return op_uart_release(req, args, args_len);
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, opcode, RPBP_ENOENT,
                       NULL, 0, false);
        return RPBP_OK;
    }
}
