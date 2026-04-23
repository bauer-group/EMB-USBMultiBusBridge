/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * CRC-32C tests — table correctness vs known vectors.
 */

#include "test_util.h"
#include "rpbp/rpbp_crc32c.h"

TEST_CASE(empty_string_is_zero)
{
    ASSERT_EQ_HEX(rpbp_crc32c((const uint8_t *)"", 0), 0u);
    return 0;
}

TEST_CASE(rfc3720_reference_vector)
{
    /* RFC 3720 §B.4 — CRC-32C("123456789") == 0xE3069283. */
    const uint8_t v[] = "123456789";
    ASSERT_EQ_HEX(rpbp_crc32c(v, 9), 0xE3069283u);
    return 0;
}

TEST_CASE(streaming_matches_oneshot)
{
    const uint8_t data[] = "the quick brown fox jumps over the lazy dog";
    const size_t  n = sizeof(data) - 1;

    uint32_t one = rpbp_crc32c(data, n);

    uint32_t run = RPBP_CRC32C_INIT;
    for (size_t off = 0; off < n; off += 7) {
        size_t chunk = (n - off) < 7u ? (n - off) : 7u;
        run = rpbp_crc32c_update(run, data + off, chunk);
    }
    ASSERT_EQ_HEX(run ^ 0xFFFFFFFFu, one);
    return 0;
}

TEST_CASE(update_then_finalize_equivalence)
{
    /* Appending data to a running accumulator yields the same final
     * CRC as a single-shot compute. This catches table-offset bugs. */
    const uint8_t a[] = { 0xAA, 0xBB };
    const uint8_t b[] = { 0xCC, 0xDD, 0xEE };
    uint8_t       ab[5] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };

    uint32_t one = rpbp_crc32c(ab, sizeof(ab));

    uint32_t run = rpbp_crc32c_update(RPBP_CRC32C_INIT, a, sizeof(a));
    run          = rpbp_crc32c_update(run,              b, sizeof(b));
    ASSERT_EQ_HEX(run ^ 0xFFFFFFFFu, one);
    return 0;
}

TEST_CASE(small_vector_cross_checked_with_kernel_libcrc32c)
{
    /*
     * CRC-32C of the ASCII bytes "abc" is 0x364B3FB7. This matches:
     *   - Linux kernel libcrc32c (crc32c_le("abc", 3, ~0) ^ ~0)
     *   - Python `crc32c` package
     *   - Intel SSE4.2 CRC32C instruction
     * so any of the above is a suitable reference when re-generating
     * the vector in the future.
     */
    const uint8_t v[] = "abc";
    ASSERT_EQ_HEX(rpbp_crc32c(v, 3), 0x364B3FB7u);
    return 0;
}

int main(void)
{
    fprintf(stdout, "test_crc32c\n");
    RUN(empty_string_is_zero);
    RUN(rfc3720_reference_vector);
    RUN(streaming_matches_oneshot);
    RUN(update_then_finalize_equivalence);
    RUN(small_vector_cross_checked_with_kernel_libcrc32c);
    fprintf(stdout, "PASS\n");
    return 0;
}
