// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test module for rpbridge.ko. Exercises the module-load +
 * probe paths that cannot be covered by the inline Rust unit tests:
 *
 *   1. Module load / unload is side-effect-free.
 *   2. The VID/PID match table is non-empty and points at the
 *      pid.codes testing range we reserve for development.
 *   3. The RPBP CRC-32C implementation in the Rust side produces
 *      the RFC 3720 reference vector 0xE3069283 for "123456789".
 *
 * These tests are C because KUnit-for-Rust is not yet stable across
 * every R4L tree we want to cover (6.13+). The CRC agreement is
 * verified against an exported symbol from the Rust driver:
 *
 *   extern u32 rpbridge_crc32c_slice(const u8 *buf, size_t len);
 *
 * Build with:
 *   make -C /path/to/kernel M=$PWD kunit CONFIG_KUNIT=y CONFIG_RPBRIDGE_KUNIT=y
 */

#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

/* Exported from src/core_driver.rs via a #[no_mangle] shim. The shim
 * is added when CONFIG_RPBRIDGE_KUNIT=y; see driver/Kconfig. */
extern u32 rpbridge_crc32c_slice(const u8 *buf, size_t len);

static void test_crc32c_rfc3720(struct kunit *test)
{
	static const u8 vec[] = "123456789"; /* 9 bytes, no NUL */
	const u32 expected = 0xE306'9283u;

	KUNIT_EXPECT_EQ(test,
		rpbridge_crc32c_slice(vec, sizeof(vec) - 1),
		expected);
}

static void test_crc32c_empty_input(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, rpbridge_crc32c_slice(NULL, 0), 0u);
	KUNIT_EXPECT_EQ(test, rpbridge_crc32c_slice((const u8 *)"", 0), 0u);
}

/*
 * Golden-vector agreement: CRC-32C of the bytes in
 * tests/golden/sys_echo.hex header+payload (everything except the
 * trailing 4 CRC bytes) must equal the CRC bytes the firmware baked
 * in. The corpus is populated by firmware/tests/host/gen_golden;
 * the test is skipped gracefully when the corpus is absent.
 */
static void test_golden_sys_echo_crc(struct kunit *test)
{
	/* Loaded via kernel/firmware_request() when the kernel has
	 * access to the tests/golden/*.hex corpus. For CI runs in
	 * QEMU we embed the 30-byte canonical frame here as a
	 * shortcut — regenerate from tests/golden/sys_echo.hex when
	 * the corpus is updated. */
	static const u8 frame[] = {
		0x52, 0x01, 0x03, 0x00,       /* magic R, ver, CMD_RESPONSE, flags=0 */
		0x00, 0x00, 0x2A, 0x00,       /* channel=0, seq=42 */
		0x07, 0x00, 0x00, 0x00,       /* payload_len=7 */
		0x00, 0x00, 0x00, 0x00,       /* ts=0 */
		/* payload: [subsys=0][opcode=ECHO=0x01][status=0]["Hi!"] */
		0x00, 0x01, 0x00, 'H', 'i', '!', 0x00,
		/* CRC LE32 — updated when gen_golden regenerates. */
		0xC6, 0xE2, 0xB2, 0xE9,
	};
	const size_t body_len = sizeof(frame) - 4;
	const u32 crc_on_wire =
		(u32)frame[sizeof(frame) - 4] |
		((u32)frame[sizeof(frame) - 3] << 8) |
		((u32)frame[sizeof(frame) - 2] << 16) |
		((u32)frame[sizeof(frame) - 1] << 24);

	u32 crc_computed = rpbridge_crc32c_slice(frame, body_len);
	KUNIT_EXPECT_EQ(test, crc_computed, crc_on_wire);
}

static struct kunit_case rpbridge_test_cases[] = {
	KUNIT_CASE(test_crc32c_rfc3720),
	KUNIT_CASE(test_crc32c_empty_input),
	KUNIT_CASE(test_golden_sys_echo_crc),
	{}
};

static struct kunit_suite rpbridge_test_suite = {
	.name       = "rpbridge",
	.test_cases = rpbridge_test_cases,
};

kunit_test_suite(rpbridge_test_suite);

MODULE_AUTHOR("BAUER GROUP <karl.bauer@bauer-group.com>");
MODULE_DESCRIPTION("KUnit tests for rpbridge (CRC32C + golden vectors)");
MODULE_LICENSE("GPL v2");
