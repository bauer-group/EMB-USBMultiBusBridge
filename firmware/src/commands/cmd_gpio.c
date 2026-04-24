/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * GPIO subsystem dispatcher. Wire format in
 * docs/protocol/subsystems/gpio.md.
 */

#include "cmd_gpio.h"

#include "peripherals/gpio.h"
#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#include <string.h>

static inline uint64_t rd_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; ++i) {
        v |= (uint64_t)p[i] << (i * 8u);
    }
    return v;
}

static inline void wr_le64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8u; ++i) {
        p[i] = (uint8_t)((v >> (i * 8u)) & 0xFFu);
    }
}

static uint8_t rc_to_status(int rc)
{
    if (rc >= 0) return RPBP_OK;
    int v = -rc;
    if (v <= 0 || v > 0xFF) return RPBP_EIO;
    return (uint8_t)v;
}

/* ───── 0x00 — LIST ──────────────────────────────────────────────────── */

static enum rpbp_status op_list(const struct rpbp_header *req,
                                const uint8_t *args, size_t args_len)
{
    (void)args;
    if (args_len != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_LIST,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    uint8_t buf[1u + GPIO_HW_NUM_USER_PINS];
    size_t n = gpio_hw_list(&buf[1], GPIO_HW_NUM_USER_PINS);
    buf[0] = (uint8_t)n;

    rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_LIST, RPBP_OK,
                   buf, 1u + n, false);
    return RPBP_OK;
}

/* ───── 0x01 — CONFIGURE ─────────────────────────────────────────────── */

static enum rpbp_status op_configure(const struct rpbp_header *req,
                                     const uint8_t *args, size_t args_len)
{
    if (args_len != 3u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_CONFIGURE,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t pin  = args[0];
    uint8_t dir  = args[1];
    uint8_t pull = args[2];

    if (dir > 1u || pull > 2u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_CONFIGURE,
                       RPBP_EINVAL, NULL, 0, false);
        return RPBP_OK;
    }

    int rc = gpio_hw_configure(pin,
                               (enum gpio_hw_dir)dir,
                               (enum gpio_hw_pull)pull);
    rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_CONFIGURE,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x02 — SET ───────────────────────────────────────────────────── */

static enum rpbp_status op_set(const struct rpbp_header *req,
                               const uint8_t *args, size_t args_len)
{
    if (args_len != 2u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_SET,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = gpio_hw_set(args[0], args[1] != 0u);
    rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_SET,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x03 — GET ───────────────────────────────────────────────────── */

static enum rpbp_status op_get(const struct rpbp_header *req,
                               const uint8_t *args, size_t args_len)
{
    if (args_len != 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_GET,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = gpio_hw_get(args[0]);
    if (rc < 0) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_GET,
                       rc_to_status(rc), NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t level = (uint8_t)rc;
    rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_GET, RPBP_OK,
                   &level, 1u, false);
    return RPBP_OK;
}

/* ───── 0x04 — SET_MASK ──────────────────────────────────────────────── */

static enum rpbp_status op_set_mask(const struct rpbp_header *req,
                                    const uint8_t *args, size_t args_len)
{
    if (args_len != 16u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_SET_MASK,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint64_t mask   = rd_le64(&args[0]);
    uint64_t values = rd_le64(&args[8]);

    int rc = gpio_hw_set_mask(mask, values);
    rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_SET_MASK,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x05 — GET_MASK ──────────────────────────────────────────────── */

static enum rpbp_status op_get_mask(const struct rpbp_header *req,
                                    const uint8_t *args, size_t args_len)
{
    if (args_len != 8u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_GET_MASK,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint64_t mask   = rd_le64(&args[0]);
    uint64_t levels = gpio_hw_get_mask(mask);

    uint8_t buf[8];
    wr_le64(buf, levels);
    rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, RPBP_GPIO_GET_MASK, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── Dispatch ─────────────────────────────────────────────────────── */

enum rpbp_status cmd_gpio_dispatch(const struct rpbp_header *req,
                                   uint8_t opcode,
                                   const uint8_t *args, size_t args_len,
                                   bool cbor)
{
    (void)cbor;

    switch (opcode) {
    case RPBP_GPIO_LIST:      return op_list     (req, args, args_len);
    case RPBP_GPIO_CONFIGURE: return op_configure(req, args, args_len);
    case RPBP_GPIO_SET:       return op_set      (req, args, args_len);
    case RPBP_GPIO_GET:       return op_get      (req, args, args_len);
    case RPBP_GPIO_SET_MASK:  return op_set_mask (req, args, args_len);
    case RPBP_GPIO_GET_MASK:  return op_get_mask (req, args, args_len);
    case RPBP_GPIO_WATCH:
    case RPBP_GPIO_UNWATCH:
        /* IRQ subscription lands with M6 (core1 + events channel). */
        rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, opcode, RPBP_ENOTSUP,
                       NULL, 0, false);
        return RPBP_OK;
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_GPIO, opcode, RPBP_ENOENT,
                       NULL, 0, false);
        return RPBP_OK;
    }
}
