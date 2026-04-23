/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * Zero-dependency unit-test harness. Each test .c exposes a `main()`
 * that returns 0 on success, 1 on failure. CTest captures the exit
 * code; that is the entirety of the framework.
 */

#ifndef RPBP_TEST_UTIL_H
#define RPBP_TEST_UTIL_H

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#define RPBP_TEST_FAIL_FMT(fmt, ...) do { \
    fprintf(stderr, "  FAIL %s:%d: " fmt "\n", \
            __FILE__, __LINE__, ##__VA_ARGS__); \
    return 1; \
} while (0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        RPBP_TEST_FAIL_FMT("ASSERT_TRUE(%s)", #cond); \
    } \
} while (0)

#define ASSERT_FALSE(cond) do { \
    if (cond) { \
        RPBP_TEST_FAIL_FMT("ASSERT_FALSE(%s)", #cond); \
    } \
} while (0)

#define ASSERT_EQ_U(a, b) do { \
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    if (_a != _b) { \
        RPBP_TEST_FAIL_FMT("ASSERT_EQ_U(%s=%" PRIu64 ", %s=%" PRIu64 ")", \
                           #a, _a, #b, _b); \
    } \
} while (0)

#define ASSERT_EQ_HEX(a, b) do { \
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    if (_a != _b) { \
        RPBP_TEST_FAIL_FMT("ASSERT_EQ_HEX(%s=0x%" PRIx64 ", %s=0x%" PRIx64 ")", \
                           #a, _a, #b, _b); \
    } \
} while (0)

#define ASSERT_MEM_EQ(a, b, len) do { \
    if (memcmp((a), (b), (len)) != 0) { \
        RPBP_TEST_FAIL_FMT("ASSERT_MEM_EQ(%s, %s, %zu) — mismatch", \
                           #a, #b, (size_t)(len)); \
    } \
} while (0)

#define TEST_CASE(name) \
    static int test_##name(void); \
    static int _invoke_##name(void) { \
        int rc = test_##name(); \
        if (rc == 0) { \
            fprintf(stdout, "  ok: %s\n", #name); \
        } \
        return rc; \
    } \
    static int test_##name(void)

#define RUN(name) do { \
    int _rc = _invoke_##name(); \
    if (_rc != 0) return _rc; \
} while (0)

#endif /* RPBP_TEST_UTIL_H */
