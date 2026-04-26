/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RPBP v1 — wire-protocol constants shared between firmware, driver
 * and userspace. Byte layout mirrors firmware/src/rpbp/rpbp.h
 * one-to-one. All multi-byte integers are little-endian.
 *
 * Keep this file in lockstep with firmware/src/rpbp/rpbp.h. Any
 * additions here MUST be additive (new opcodes, new flags) and
 * MUST also land in the firmware before a wire-format-using
 * commit can ship.
 */

#ifndef RPBRIDGE_RPBP_H
#define RPBRIDGE_RPBP_H

#include <linux/types.h>

/* ───── Wire constants ───────────────────────────────────────────── */

#define RPBP_MAGIC               0x52u   /* 'R' */
#define RPBP_VERSION             0x01u
#define RPBP_HEADER_LEN          16u
#define RPBP_CRC_LEN             4u
#define RPBP_MAX_PAYLOAD_LEN     4096u
#define RPBP_MAX_FRAME_LEN       (RPBP_HEADER_LEN + RPBP_MAX_PAYLOAD_LEN \
                                  + RPBP_CRC_LEN)

/* ───── Channels ─────────────────────────────────────────────────── */

#define RPBP_CHAN_CONTROL        0u
#define RPBP_CHAN_EVENTS         1u
#define RPBP_CHAN_DYN_MIN        16u
#define RPBP_CHAN_DYN_MAX        239u

/* ───── Message types ────────────────────────────────────────────── */

enum rpbp_msg_type {
	RPBP_MSG_HELLO         = 0x00,
	RPBP_MSG_CAPABILITIES  = 0x01,
	RPBP_MSG_CMD_REQUEST   = 0x02,
	RPBP_MSG_CMD_RESPONSE  = 0x03,
	RPBP_MSG_STREAM_DATA   = 0x04,
	RPBP_MSG_STREAM_CREDIT = 0x05,
	RPBP_MSG_EVENT         = 0x06,
	RPBP_MSG_PING          = 0x07,
	RPBP_MSG_PONG          = 0x08,
	RPBP_MSG_ERROR         = 0x09,
	RPBP_MSG_RESET_CHANNEL = 0x0A,
	RPBP_MSG_TIME_SYNC     = 0x0B,
};

/* ───── Header flag bits (low 6 — bits 6/7 reserved/zero in v1) ──── */

#define RPBP_FLAG_CBOR           BIT(0)
#define RPBP_FLAG_COMPRESSED     BIT(1)
#define RPBP_FLAG_URGENT         BIT(2)
#define RPBP_FLAG_FRAGMENT       BIT(3)
#define RPBP_FLAG_LAST           BIT(4)
#define RPBP_FLAG_CONTINUATION   BIT(5)
#define RPBP_FLAG_RESERVED_MASK  (BIT(6) | BIT(7))

/* ───── Subsystem IDs ────────────────────────────────────────────── */

enum rpbp_subsys {
	RPBP_SUBSYS_SYS     = 0x00,
	RPBP_SUBSYS_I2C     = 0x01,
	RPBP_SUBSYS_SPI     = 0x02,
	RPBP_SUBSYS_GPIO    = 0x03,
	RPBP_SUBSYS_PWM     = 0x04,
	RPBP_SUBSYS_ADC     = 0x05,
	RPBP_SUBSYS_UART    = 0x06,
	RPBP_SUBSYS_LED     = 0x07,
	RPBP_SUBSYS_DMX     = 0x08,
	RPBP_SUBSYS_ONEWIRE = 0x09,
	RPBP_SUBSYS_I2S     = 0x0A,
};

/* ───── Status codes (mirrors firmware enum rpbp_status) ─────────── */

enum rpbp_status {
	RPBP_OK         = 0,
	RPBP_EAGAIN     = 1,
	RPBP_EINVAL     = 2,
	RPBP_EPERM      = 3,
	RPBP_ENOENT     = 4,
	RPBP_EIO        = 5,
	RPBP_ETIMEDOUT  = 6,
	RPBP_EMSGSIZE   = 7,
	RPBP_ECRC       = 8,
	RPBP_EPROTO     = 9,
	RPBP_ENOTSUP    = 10,
	RPBP_EBUSY      = 11,
	RPBP_ECANCELED  = 12,
	RPBP_ENOSPC     = 13,
	RPBP_EOVERFLOW  = 14,
	RPBP_ENODEV     = 15,
};

/* ───── Subsystem opcode namespaces ──────────────────────────────── */

/* SYS subsystem (0x00) */
#define RPBP_SYS_GET_CAPABILITIES  0x00u
#define RPBP_SYS_ECHO              0x01u
#define RPBP_SYS_REBOOT_BOOTSEL    0x02u
#define RPBP_SYS_UPTIME            0x03u
#define RPBP_SYS_GET_VBUS_MV       0x04u
#define RPBP_SYS_SET_LED           0x05u
#define RPBP_SYS_SELFTEST          0x06u
#define RPBP_SYS_GET_IDENTITY      0x07u
#define RPBP_SYS_RESET             0x08u

/* I2C subsystem (0x01) */
#define RPBP_I2C_PROBE             0x00u
#define RPBP_I2C_XFER              0x01u
#define RPBP_I2C_SCAN              0x02u
#define RPBP_I2C_SET_FREQ          0x03u
#define RPBP_I2C_GET_FREQ          0x04u
#define RPBP_I2C_FLAG_NO_STOP      BIT(0)

/* SPI subsystem (0x02) */
#define RPBP_SPI_XFER              0x00u
#define RPBP_SPI_SET_MODE          0x01u
#define RPBP_SPI_SET_FREQ          0x02u
#define RPBP_SPI_GET_FREQ          0x03u
#define RPBP_SPI_CS_ASSERT         0x04u
#define RPBP_SPI_CS_RELEASE        0x05u
#define RPBP_SPI_FLAG_HOLD_CS      BIT(0)
#define RPBP_SPI_MODE_CPHA         BIT(0)
#define RPBP_SPI_MODE_CPOL         BIT(1)
#define RPBP_SPI_MODE_LSB_FIRST    BIT(2)

/* GPIO subsystem (0x03) */
#define RPBP_GPIO_LIST             0x00u
#define RPBP_GPIO_CONFIGURE        0x01u
#define RPBP_GPIO_SET              0x02u
#define RPBP_GPIO_GET              0x03u
#define RPBP_GPIO_SET_MASK         0x04u
#define RPBP_GPIO_GET_MASK         0x05u
#define RPBP_GPIO_WATCH            0x06u
#define RPBP_GPIO_UNWATCH          0x07u
#define RPBP_GPIO_DIR_INPUT        0u
#define RPBP_GPIO_DIR_OUTPUT       1u
#define RPBP_GPIO_PULL_NONE        0u
#define RPBP_GPIO_PULL_UP          1u
#define RPBP_GPIO_PULL_DOWN        2u
#define RPBP_GPIO_WATCH_RISING     BIT(0)
#define RPBP_GPIO_WATCH_FALLING    BIT(1)
#define RPBP_GPIO_WATCH_BOTH       (RPBP_GPIO_WATCH_RISING \
                                     | RPBP_GPIO_WATCH_FALLING)

/* PWM subsystem (0x04) */
#define RPBP_PWM_CONFIGURE         0x00u
#define RPBP_PWM_SET_DUTY          0x01u
#define RPBP_PWM_START             0x02u
#define RPBP_PWM_STOP              0x03u
#define RPBP_PWM_GET_STATE         0x04u
#define RPBP_PWM_FLAG_INVERT       BIT(0)

/* ADC subsystem (0x05) */
#define RPBP_ADC_READ              0x00u
#define RPBP_ADC_READ_MANY         0x01u
#define RPBP_ADC_GET_REF           0x02u
#define RPBP_ADC_TEMP_READ         0x03u
#define RPBP_ADC_STREAM_START      0x04u
#define RPBP_ADC_STREAM_STOP       0x05u

/* LED subsystem (0x07) */
#define RPBP_LED_GET_INFO          0x00u
#define RPBP_LED_CONFIGURE         0x01u
#define RPBP_LED_WRITE             0x02u
#define RPBP_LED_CLEAR             0x03u

/* DMX subsystem (0x08) */
#define RPBP_DMX_GET_INFO          0x00u
#define RPBP_DMX_SET_SLOTS         0x01u
#define RPBP_DMX_START             0x02u
#define RPBP_DMX_STOP              0x03u
#define RPBP_DMX_SET_BREAK         0x04u

/* 1-Wire subsystem (0x09) */
#define RPBP_ONEWIRE_GET_INFO      0x00u
#define RPBP_ONEWIRE_RESET         0x01u
#define RPBP_ONEWIRE_WRITE         0x02u
#define RPBP_ONEWIRE_READ          0x03u
#define RPBP_ONEWIRE_TRIPLET       0x04u

/* I2S subsystem (0x0A) */
#define RPBP_I2S_GET_INFO          0x00u
#define RPBP_I2S_CONFIGURE         0x01u
#define RPBP_I2S_START             0x02u
#define RPBP_I2S_STOP              0x03u
#define RPBP_I2S_WRITE             0x04u

/* ───── Vendor IDs ───────────────────────────────────────────────── */

/*
 * PRE-PRODUCTION VID/PID — keep in lock-step with
 * firmware/src/usb/usb_descriptors.c. The current PID is from the
 * pid.codes testing range (0xFFF0..0xFFFF) and WILL collide with
 * any other developer's testing device on the same host. Tracked in
 * docs/usb-vid-pid.md and firmware/TODO.md. Replace before shipping
 * production hardware.
 */
#define RPBRIDGE_USB_VID         0x1209u   /* pid.codes Open Hardware VID */
#define RPBRIDGE_USB_PID         0xFFFEu   /* TESTING-RANGE — replace pre-release */

/*
 * Map an RPBP status code to a Linux errno.
 * Defined in rpbp_framing.c so it stays close to the wire constants.
 */
int rpbp_status_to_errno(enum rpbp_status s);

#endif /* RPBRIDGE_RPBP_H */
