/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * uart_pio — PIO-backed 8N1 UART with software ring buffers.
 *
 * Design
 * ──────
 *   One instance consumes two PIO state machines on the same block:
 *   SM n+0 runs the TX program, SM n+1 runs the RX program. The PIO
 *   instruction memory is shared; each program is added once per
 *   firmware run. Instance 0 runs on PIO1 (PIO0 is reserved for
 *   can2040 / CAN).
 *
 *   Data flow (RX):
 *     PIO RX FIFO  --(tick drain)-->  rx_ring  --(uart_pio_read)-->  caller
 *   Data flow (TX):
 *     caller  --(uart_pio_write)-->  tx_ring  --(tick feed)-->  PIO TX FIFO
 *
 *   The PIO FIFOs are only 4 deep (8 joined), so the tick function
 *   MUST run fast enough that the RX FIFO never fills. At 3 Mbaud a
 *   byte lands every ~3.3 µs; the main loop polls tens of kHz so
 *   this is easily met.
 */

#include "uart_pio.h"
#include "pin_broker.h"

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

#include "uart_pio_tx.pio.h"
#include "uart_pio_rx.pio.h"

#include <string.h>

#define NUM_UART_PIO_INSTANCES     1u

#define UART_PIO_RX_RING_SIZE      2048u
#define UART_PIO_TX_RING_SIZE      1024u
#define UART_PIO_RX_RING_MASK      (UART_PIO_RX_RING_SIZE - 1u)
#define UART_PIO_TX_RING_MASK      (UART_PIO_TX_RING_SIZE - 1u)

_Static_assert((UART_PIO_RX_RING_SIZE & (UART_PIO_RX_RING_SIZE - 1u)) == 0,
               "RX ring size must be a power of two");
_Static_assert((UART_PIO_TX_RING_SIZE & (UART_PIO_TX_RING_SIZE - 1u)) == 0,
               "TX ring size must be a power of two");

struct uart_pio_state {
    PIO      pio;
    uint     sm_tx;
    uint     sm_rx;
    uint     offset_tx;
    uint     offset_rx;

    uint8_t  tx_pin;
    uint8_t  rx_pin;

    uint8_t  rx_buf[UART_PIO_RX_RING_SIZE];
    uint8_t  tx_buf[UART_PIO_TX_RING_SIZE];

    uint16_t rx_head;
    uint16_t rx_tail;
    uint16_t tx_head;
    uint16_t tx_tail;

    struct uart_pio_stats stats;
    bool    initialised;
};

static struct uart_pio_state g_uarts[NUM_UART_PIO_INSTANCES];

/* ───── Small helpers ────────────────────────────────────────────────── */

static inline uint16_t ring_used(uint16_t head, uint16_t tail, uint16_t mask)
{
    return (uint16_t)((head - tail) & mask);
}

static float compute_clkdiv(uint32_t baud)
{
    /* Our PIO programs run at 8 cycles per bit. */
    return (float)clock_get_hz(clk_sys) / (8.0f * (float)baud);
}

/* ───── SM bring-up ──────────────────────────────────────────────────── */

static void init_tx_sm(struct uart_pio_state *s, uint32_t baud)
{
    pio_sm_config c = rpbridge_uart_pio_tx_program_get_default_config(s->offset_tx);

    /* Same pin for OUT and side-set — the `.side_set 1 opt` directive
     * lets the start / stop framing drive the line via side-set while
     * data bits come out of OUT. */
    sm_config_set_out_pins(&c, s->tx_pin, 1);
    sm_config_set_sideset_pins(&c, s->tx_pin);

    /* Shift LSB-first; no autopull — we push one byte per 32-bit word. */
    sm_config_set_out_shift(&c, true /*shift_right*/, false /*autopull*/, 32);

    /* Join the TX FIFO for 8 words of buffering. */
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    sm_config_set_clkdiv(&c, compute_clkdiv(baud));

    /* Start with line idle (HIGH). */
    pio_sm_set_pins_with_mask   (s->pio, s->sm_tx, 1u << s->tx_pin, 1u << s->tx_pin);
    pio_sm_set_pindirs_with_mask(s->pio, s->sm_tx, 1u << s->tx_pin, 1u << s->tx_pin);
    pio_gpio_init(s->pio, s->tx_pin);

    pio_sm_init(s->pio, s->sm_tx, s->offset_tx, &c);
    pio_sm_set_enabled(s->pio, s->sm_tx, true);
}

static void init_rx_sm(struct uart_pio_state *s, uint32_t baud)
{
    pio_sm_config c = rpbridge_uart_pio_rx_program_get_default_config(s->offset_rx);

    sm_config_set_in_pins (&c, s->rx_pin);   /* IN / WAIT base */
    sm_config_set_jmp_pin (&c, s->rx_pin);   /* JMP pin check  */

    sm_config_set_in_shift(&c, true /*shift_right*/, false /*autopush*/, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

    sm_config_set_clkdiv(&c, compute_clkdiv(baud));

    pio_sm_set_consecutive_pindirs(s->pio, s->sm_rx, s->rx_pin, 1, false);
    pio_gpio_init(s->pio, s->rx_pin);
    gpio_pull_up(s->rx_pin);                 /* idle = HIGH */

    pio_sm_init(s->pio, s->sm_rx, s->offset_rx, &c);
    pio_sm_set_enabled(s->pio, s->sm_rx, true);
}

/* ───── Public API ───────────────────────────────────────────────────── */

int uart_pio_init(uart_pio_instance_t instance,
                  uint8_t tx_pin, uint8_t rx_pin,
                  uint32_t baudrate)
{
    if (instance >= NUM_UART_PIO_INSTANCES) return -1;
    if (baudrate == 0u) return -1;

    struct uart_pio_state *s = &g_uarts[instance];
    memset(s, 0, sizeof(*s));

    /* PIO1 is the block we use for user-facing PIO applets. PIO0 is
     * reserved for can2040 (see firmware/TODO.md §M5). */
    s->pio = pio1;

    if (!pio_can_add_program(s->pio, &rpbridge_uart_pio_tx_program)) return -1;
    s->offset_tx = pio_add_program(s->pio, &rpbridge_uart_pio_tx_program);

    if (!pio_can_add_program(s->pio, &rpbridge_uart_pio_rx_program)) return -1;
    s->offset_rx = pio_add_program(s->pio, &rpbridge_uart_pio_rx_program);

    int sm_tx = pio_claim_unused_sm(s->pio, false);
    int sm_rx = pio_claim_unused_sm(s->pio, false);
    if (sm_tx < 0 || sm_rx < 0) return -1;
    s->sm_tx = (uint)sm_tx;
    s->sm_rx = (uint)sm_rx;

    s->tx_pin = tx_pin;
    s->rx_pin = rx_pin;

    /* Claim the pins on the broker — a conflict is a firmware bug
     * and propagates up as init failure. */
    if (pin_broker_claim(tx_pin, PIN_OWNER_UART_PIO, PIN_FUNC_UART_TX) < 0) return -1;
    if (pin_broker_claim(rx_pin, PIN_OWNER_UART_PIO, PIN_FUNC_UART_RX) < 0) return -1;

    init_tx_sm(s, baudrate);
    init_rx_sm(s, baudrate);

    s->initialised = true;
    return 0;
}

int uart_pio_set_line_coding(uart_pio_instance_t instance,
                             uint32_t baudrate,
                             uint8_t  data_bits,
                             uint8_t  stop_bits,
                             uint8_t  parity)
{
    if (instance >= NUM_UART_PIO_INSTANCES) return -1;
    if (baudrate == 0u)                     return -1;

    struct uart_pio_state *s = &g_uarts[instance];
    if (!s->initialised) return -1;

    /* Apply the baud first — this is the only thing we can actually
     * honour. Framing divergence is handled separately below. */
    float div = compute_clkdiv(baudrate);
    pio_sm_set_clkdiv(s->pio, s->sm_tx, div);
    pio_sm_set_clkdiv(s->pio, s->sm_rx, div);
    pio_clkdiv_restart_sm_mask(s->pio, (1u << s->sm_tx) | (1u << s->sm_rx));

    /* Detect non-8N1 line-coding requests. The PIO programs are
     * framing-locked (see uart_pio.h header-block), so rather than
     * silently pretending to honour 7E2 / 8O1 / etc. we count the
     * mismatch and signal it upward via the return value. */
    bool framing_ok = (data_bits == 8u)
                   && (stop_bits == 1u)
                   && (parity    == UART_PIO_PARITY_NONE);
    if (!framing_ok) {
        s->stats.framing_clamped++;
        return 1;
    }
    return 0;
}

size_t uart_pio_read(uart_pio_instance_t instance, uint8_t *buf, size_t maxlen)
{
    if (instance >= NUM_UART_PIO_INSTANCES) return 0;
    struct uart_pio_state *s = &g_uarts[instance];

    size_t copied = 0;
    while (copied < maxlen && s->rx_tail != s->rx_head) {
        buf[copied++] = s->rx_buf[s->rx_tail];
        s->rx_tail = (uint16_t)((s->rx_tail + 1u) & UART_PIO_RX_RING_MASK);
    }
    return copied;
}

size_t uart_pio_write(uart_pio_instance_t instance, const uint8_t *buf, size_t len)
{
    if (instance >= NUM_UART_PIO_INSTANCES) return 0;
    struct uart_pio_state *s = &g_uarts[instance];

    size_t pushed = 0;
    while (pushed < len) {
        uint16_t next_head = (uint16_t)((s->tx_head + 1u) & UART_PIO_TX_RING_MASK);
        if (next_head == s->tx_tail) break;
        s->tx_buf[s->tx_head] = buf[pushed++];
        s->tx_head = next_head;
    }
    return pushed;
}

size_t uart_pio_rx_available(uart_pio_instance_t instance)
{
    if (instance >= NUM_UART_PIO_INSTANCES) return 0;
    const struct uart_pio_state *s = &g_uarts[instance];
    return ring_used(s->rx_head, s->rx_tail, UART_PIO_RX_RING_MASK);
}

size_t uart_pio_tx_space(uart_pio_instance_t instance)
{
    if (instance >= NUM_UART_PIO_INSTANCES) return 0;
    const struct uart_pio_state *s = &g_uarts[instance];
    uint16_t used = ring_used(s->tx_head, s->tx_tail, UART_PIO_TX_RING_MASK);
    return (size_t)(UART_PIO_TX_RING_SIZE - 1u - used);
}

void uart_pio_tick(uart_pio_instance_t instance)
{
    if (instance >= NUM_UART_PIO_INSTANCES) return;
    struct uart_pio_state *s = &g_uarts[instance];
    if (!s->initialised) return;

    /* 1. Drain PIO RX FIFO into software ring. Bytes are right-shifted,
     *    landing in the top 8 bits of the 32-bit word. */
    while (!pio_sm_is_rx_fifo_empty(s->pio, s->sm_rx)) {
        uint32_t raw  = pio_sm_get(s->pio, s->sm_rx);
        uint8_t  byte = (uint8_t)(raw >> 24);

        uint16_t next_head = (uint16_t)((s->rx_head + 1u) & UART_PIO_RX_RING_MASK);
        if (next_head == s->rx_tail) {
            s->stats.rx_overrun++;          /* ring full, byte dropped */
        } else {
            s->rx_buf[s->rx_head] = byte;
            s->rx_head = next_head;
            s->stats.rx_bytes++;
        }
    }

    /* 2. Push software TX ring into PIO TX FIFO. */
    while (s->tx_tail != s->tx_head &&
           !pio_sm_is_tx_fifo_full(s->pio, s->sm_tx)) {
        pio_sm_put(s->pio, s->sm_tx, (uint32_t)s->tx_buf[s->tx_tail]);
        s->tx_tail = (uint16_t)((s->tx_tail + 1u) & UART_PIO_TX_RING_MASK);
        s->stats.tx_bytes++;
    }

    /* 3. PIO framing-error IRQ → surface as stat. The RX program
     *    fires `irq 4 rel` which maps to irq bit (4 + sm_rx & 3) on
     *    the block. We clear it here so the next framing event is
     *    visible as a new counter bump. */
    uint irq_bit = (uint)((s->sm_rx & 3u) + 4u);
    if (pio_interrupt_get(s->pio, irq_bit)) {
        s->stats.rx_framing_err++;
        pio_interrupt_clear(s->pio, irq_bit);
    }
}

const struct uart_pio_stats *uart_pio_stats(uart_pio_instance_t instance)
{
    if (instance >= NUM_UART_PIO_INSTANCES) return NULL;
    return &g_uarts[instance].stats;
}
