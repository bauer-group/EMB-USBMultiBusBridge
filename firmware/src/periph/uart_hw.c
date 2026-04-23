/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * uart_hw — IRQ-driven UART wrapper with per-instance RX/TX ring buffers.
 *
 * Design
 * ──────
 *   RX flow:
 *     UART RX FIFO  --(ISR drain)-->  rx_ring  --(uart_hw_read)-->  caller
 *
 *   TX flow:
 *     caller  --(uart_hw_write)-->  tx_ring  --(ISR drain)-->  UART TX FIFO
 *
 * Concurrency:
 *   Each ring is single-producer / single-consumer. Producer writes
 *   `head`, consumer writes `tail`; both read the other side
 *   non-authoritatively. On a Cortex-M33 without SMP and with same-
 *   core ISRs, a simple volatile head/tail pair suffices — the IRQ
 *   cannot preempt itself and aligned 16-bit accesses are atomic.
 *
 * Ring sizes (per instance):
 *   RX = 2048 bytes — ~22 ms of headroom at 921 600 baud.
 *   TX = 1024 bytes — enough to smooth out host-side bursts.
 */

#include "uart_hw.h"

#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "hardware/regs/uart.h"

#include <string.h>

#define NUM_UART_HW_INSTANCES    2u

#define UART_HW_RX_RING_SIZE     2048u
#define UART_HW_TX_RING_SIZE     1024u
#define UART_HW_RX_RING_MASK     (UART_HW_RX_RING_SIZE - 1u)
#define UART_HW_TX_RING_MASK     (UART_HW_TX_RING_SIZE - 1u)

/* Compile-time sanity checks — rings must be a power of two. */
_Static_assert((UART_HW_RX_RING_SIZE & (UART_HW_RX_RING_SIZE - 1u)) == 0,
               "RX ring size must be a power of two");
_Static_assert((UART_HW_TX_RING_SIZE & (UART_HW_TX_RING_SIZE - 1u)) == 0,
               "TX ring size must be a power of two");

struct uart_hw_state {
    uart_inst_t *inst;
    uint         irq_num;
    bool         initialised;

    uint8_t  rx_buf[UART_HW_RX_RING_SIZE];
    uint8_t  tx_buf[UART_HW_TX_RING_SIZE];

    volatile uint16_t rx_head;     /* ISR writes, consumer reads */
    volatile uint16_t rx_tail;     /* consumer writes, ISR reads */
    volatile uint16_t tx_head;     /* producer writes, ISR reads */
    volatile uint16_t tx_tail;     /* ISR writes, producer reads */

    /* RS485 DE pin. -1 = disabled, otherwise GPIO number. Driven HIGH
     * just before any TX byte enters the ring / FIFO, released back to
     * LOW by uart_hw_tick() once the UART BUSY flag clears. */
    int8_t            de_pin;
    volatile bool     de_asserted;

    struct uart_hw_stats stats;
};

static struct uart_hw_state g_uarts[NUM_UART_HW_INSTANCES];

/* ───── Small accessors ──────────────────────────────────────────────── */

static inline uart_inst_t *inst_ptr(uart_hw_instance_t i)
{
    return (i == 0u) ? uart0 : uart1;
}

static inline uint inst_irq(uart_hw_instance_t i)
{
    return (i == 0u) ? UART0_IRQ : UART1_IRQ;
}

static inline uint16_t ring_used(uint16_t head, uint16_t tail, uint16_t mask)
{
    return (uint16_t)((head - tail) & mask);
}

/* ───── IRQ handler core ─────────────────────────────────────────────── */

static void uart_hw_irq_core(uart_hw_instance_t instance)
{
    struct uart_hw_state *s = &g_uarts[instance];
    uart_hw_t *hw = uart_get_hw(s->inst);

    /* Drain the RX FIFO. The DR register latches one byte plus four
     * flag bits — check each for diagnostic counters. */
    while (uart_is_readable(s->inst)) {
        uint32_t dr = hw->dr;

        if (dr & UART_UARTDR_OE_BITS) s->stats.rx_overrun++;       /* HW FIFO overflow */
        if (dr & UART_UARTDR_BE_BITS) s->stats.rx_break++;
        if (dr & UART_UARTDR_PE_BITS) s->stats.rx_parity_err++;
        if (dr & UART_UARTDR_FE_BITS) s->stats.rx_framing_err++;

        uint8_t  byte      = (uint8_t)(dr & UART_UARTDR_DATA_BITS);
        uint16_t next_head = (uint16_t)((s->rx_head + 1u) & UART_HW_RX_RING_MASK);
        if (next_head == s->rx_tail) {
            /* Software ring full — drop byte and count it. */
            s->stats.rx_overrun++;
        } else {
            s->rx_buf[s->rx_head] = byte;
            s->rx_head = next_head;
            s->stats.rx_bytes++;
        }
    }

    /* Push from the TX ring into the TX FIFO while there's room. */
    while (uart_is_writable(s->inst) && s->tx_tail != s->tx_head) {
        hw->dr = s->tx_buf[s->tx_tail];
        s->tx_tail = (uint16_t)((s->tx_tail + 1u) & UART_HW_TX_RING_MASK);
        s->stats.tx_bytes++;
    }

    /* No more TX data → disable TX IRQ so it doesn't storm us. The
     * producer re-enables it next time uart_hw_write() queues bytes. */
    if (s->tx_tail == s->tx_head) {
        uart_set_irq_enables(s->inst, true /*rx*/, false /*tx*/);
    }

    /* Clear the latched error / timeout flags — these stay sticky
     * until software acknowledges. Data-register flags are cleared
     * by the FIFO reads above. */
    hw->icr = UART_UARTICR_OEIC_BITS | UART_UARTICR_BEIC_BITS |
              UART_UARTICR_PEIC_BITS | UART_UARTICR_FEIC_BITS |
              UART_UARTICR_RTIC_BITS;
}

static void uart0_hw_irq_handler(void) { uart_hw_irq_core(0u); }
static void uart1_hw_irq_handler(void) { uart_hw_irq_core(1u); }

/* ───── Public API ───────────────────────────────────────────────────── */

int uart_hw_init(uart_hw_instance_t instance, uint8_t tx_pin, uint8_t rx_pin)
{
    if (instance >= NUM_UART_HW_INSTANCES) return -1;

    struct uart_hw_state *s = &g_uarts[instance];
    memset(s, 0, sizeof(*s));

    s->inst    = inst_ptr(instance);
    s->irq_num = inst_irq(instance);
    s->de_pin  = -1;               /* no DE pin unless explicitly set */

    /* Route the pins first so the UART never drives a floating pad. */
    gpio_set_function(tx_pin, GPIO_FUNC_UART);
    gpio_set_function(rx_pin, GPIO_FUNC_UART);

    /* Default 115 200 8N1, no HW flow control. Line coding is refined
     * by the CDC callback once the host connects. */
    uart_init(s->inst, 115200);
    uart_set_format(s->inst, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(s->inst, true);
    uart_set_hw_flow(s->inst, false, false);

    /* Attach the ISR. Use the shared-IRQ helpers so co-existing
     * drivers (e.g. stdio_uart on UART1) don't get kicked off. */
    irq_set_exclusive_handler(s->irq_num,
        (instance == 0u) ? uart0_hw_irq_handler : uart1_hw_irq_handler);
    irq_set_enabled(s->irq_num, true);

    /* RX IRQ on — drain whenever data lands. TX IRQ starts off and is
     * flipped on in uart_hw_write(). */
    uart_set_irq_enables(s->inst, true /*rx*/, false /*tx*/);

    s->initialised = true;
    return 0;
}

int uart_hw_set_line_coding(uart_hw_instance_t instance,
                            uint32_t baudrate,
                            uint8_t  data_bits,
                            uint8_t  stop_bits,
                            uint8_t  parity)
{
    if (instance >= NUM_UART_HW_INSTANCES) return -1;
    if (!g_uarts[instance].initialised)   return -1;
    if (data_bits < 5u || data_bits > 8u) return -1;
    if (stop_bits < 1u || stop_bits > 2u) return -1;

    uart_parity_t p;
    switch (parity) {
    case UART_HW_PARITY_NONE: p = UART_PARITY_NONE; break;
    case UART_HW_PARITY_ODD:  p = UART_PARITY_ODD;  break;
    case UART_HW_PARITY_EVEN: p = UART_PARITY_EVEN; break;
    default: return -1;
    }

    uart_set_baudrate(g_uarts[instance].inst, baudrate);
    uart_set_format(g_uarts[instance].inst, data_bits, stop_bits, p);
    return 0;
}

size_t uart_hw_read(uart_hw_instance_t instance, uint8_t *buf, size_t maxlen)
{
    if (instance >= NUM_UART_HW_INSTANCES) return 0;
    struct uart_hw_state *s = &g_uarts[instance];

    size_t copied = 0;
    while (copied < maxlen && s->rx_tail != s->rx_head) {
        buf[copied++] = s->rx_buf[s->rx_tail];
        s->rx_tail = (uint16_t)((s->rx_tail + 1u) & UART_HW_RX_RING_MASK);
    }
    return copied;
}

size_t uart_hw_write(uart_hw_instance_t instance, const uint8_t *buf, size_t len)
{
    if (instance >= NUM_UART_HW_INSTANCES) return 0;
    struct uart_hw_state *s = &g_uarts[instance];

    size_t pushed = 0;
    while (pushed < len) {
        uint16_t next_head = (uint16_t)((s->tx_head + 1u) & UART_HW_TX_RING_MASK);
        if (next_head == s->tx_tail) break;   /* ring full — caller retries */
        s->tx_buf[s->tx_head] = buf[pushed++];
        s->tx_head = next_head;
    }

    if (pushed > 0) {
        /* Before kicking the TX IRQ chain, assert DE if a DE pin is
         * registered and not already high. We do this on the producer
         * side (not inside the ISR) so the DE transition happens
         * strictly before the first byte can hit the TX shift
         * register, and the driver's lifetime fully brackets the
         * transmission. */
        if (s->de_pin >= 0 && !s->de_asserted) {
            gpio_put((uint)s->de_pin, 1);
            s->de_asserted = true;
        }
        uart_set_irq_enables(s->inst, true /*rx*/, true /*tx*/);
    }
    return pushed;
}

size_t uart_hw_rx_available(uart_hw_instance_t instance)
{
    if (instance >= NUM_UART_HW_INSTANCES) return 0;
    const struct uart_hw_state *s = &g_uarts[instance];
    return ring_used(s->rx_head, s->rx_tail, UART_HW_RX_RING_MASK);
}

size_t uart_hw_tx_space(uart_hw_instance_t instance)
{
    if (instance >= NUM_UART_HW_INSTANCES) return 0;
    const struct uart_hw_state *s = &g_uarts[instance];
    uint16_t used = ring_used(s->tx_head, s->tx_tail, UART_HW_TX_RING_MASK);
    /* -1 because a fully-equal head==tail pair means "empty", so the
     * ring can only hold SIZE-1 bytes before it would collide. */
    return (size_t)(UART_HW_TX_RING_SIZE - 1u - used);
}

const struct uart_hw_stats *uart_hw_stats(uart_hw_instance_t instance)
{
    if (instance >= NUM_UART_HW_INSTANCES) return NULL;
    return &g_uarts[instance].stats;
}

/* ───── RS485 DE-pin support ─────────────────────────────────────────── */

void uart_hw_set_de_pin(uart_hw_instance_t instance, int8_t gpio)
{
    if (instance >= NUM_UART_HW_INSTANCES) return;
    struct uart_hw_state *s = &g_uarts[instance];

    /* Release the previous pin (if any). The GPIO's direction / pull
     * configuration stays untouched — the phy module owns the pin
     * lifecycle and will repurpose it on the next phy change. */
    if (s->de_pin >= 0 && s->de_asserted) {
        gpio_put((uint)s->de_pin, 0);
    }
    s->de_asserted = false;
    s->de_pin      = gpio;
}

void uart_hw_tick(uart_hw_instance_t instance)
{
    if (instance >= NUM_UART_HW_INSTANCES) return;
    struct uart_hw_state *s = &g_uarts[instance];
    if (!s->initialised)   return;
    if (s->de_pin < 0)     return;
    if (!s->de_asserted)   return;

    /* DE stays up until the UART is fully idle:
     *   - no bytes queued in our ring     (tx_head == tx_tail)
     *   - the hardware BUSY flag cleared  (the last bit has left the shift register)
     * Without the BUSY check we would clip the last stop bit on
     * short bursts. */
    const bool ring_empty = (s->tx_tail == s->tx_head);
    const bool hw_idle    = (uart_get_hw(s->inst)->fr & UART_UARTFR_BUSY_BITS) == 0u;

    if (ring_empty && hw_idle) {
        gpio_put((uint)s->de_pin, 0);
        s->de_asserted = false;
    }
}
