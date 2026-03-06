/*
 * test_zigzag.c — Unit tests for zigzag encode/decode
 */
#include "test_framework.h"
#include "residc.h"
#include <limits.h>
#include <stdint.h>

static void test_zero(void)
{
    ASSERT_EQ(residc_zigzag_enc(0), 0ULL);
    ASSERT_EQ(residc_zigzag_dec(0), 0LL);
}

static void test_neg1(void)
{
    ASSERT_EQ(residc_zigzag_enc(-1), 1ULL);
    ASSERT_EQ(residc_zigzag_dec(1), -1LL);
}

static void test_pos1(void)
{
    ASSERT_EQ(residc_zigzag_enc(1), 2ULL);
    ASSERT_EQ(residc_zigzag_dec(2), 1LL);
}

static void test_neg2(void)
{
    ASSERT_EQ(residc_zigzag_enc(-2), 3ULL);
    ASSERT_EQ(residc_zigzag_dec(3), -2LL);
}

static void test_pos2(void)
{
    ASSERT_EQ(residc_zigzag_enc(2), 4ULL);
    ASSERT_EQ(residc_zigzag_dec(4), 2LL);
}

static void test_int64_max(void)
{
    int64_t v = INT64_MAX;
    uint64_t zz = residc_zigzag_enc(v);
    int64_t back = residc_zigzag_dec(zz);
    ASSERT_EQ(back, v);
}

static void test_int64_min(void)
{
    int64_t v = INT64_MIN;
    uint64_t zz = residc_zigzag_enc(v);
    int64_t back = residc_zigzag_dec(zz);
    ASSERT_EQ(back, v);
}

static void test_roundtrip_range(void)
{
    for (int64_t v = -1000; v <= 1000; v++) {
        uint64_t zz = residc_zigzag_enc(v);
        int64_t back = residc_zigzag_dec(zz);
        ASSERT_EQ(back, v);
    }
}

/* Verify zigzag mapping is monotonic for absolute values:
 * |v| <= |w| implies zigzag(v) <= zigzag(w) for same-sign values */
static void test_ordering_property(void)
{
    /* Positive values should map to even numbers: 0->0, 1->2, 2->4, ... */
    for (int64_t v = 0; v < 100; v++) {
        ASSERT_EQ(residc_zigzag_enc(v), (uint64_t)(v * 2));
    }
    /* Negative values should map to odd numbers: -1->1, -2->3, -3->5, ... */
    for (int64_t v = -1; v > -100; v--) {
        ASSERT_EQ(residc_zigzag_enc(v), (uint64_t)(-v * 2 - 1));
    }
}

/* Large positive and negative values */
static void test_large_values(void)
{
    int64_t vals[] = {1000000, -1000000, 1LL << 40, -(1LL << 40),
                      1LL << 62, -(1LL << 62)};
    for (int i = 0; i < 6; i++) {
        uint64_t zz = residc_zigzag_enc(vals[i]);
        int64_t back = residc_zigzag_dec(zz);
        ASSERT_EQ(back, vals[i]);
    }
}

int main(void)
{
    printf("=== test_zigzag ===\n");
    RUN_TEST(test_zero);
    RUN_TEST(test_neg1);
    RUN_TEST(test_pos1);
    RUN_TEST(test_neg2);
    RUN_TEST(test_pos2);
    RUN_TEST(test_int64_max);
    RUN_TEST(test_int64_min);
    RUN_TEST(test_roundtrip_range);
    RUN_TEST(test_ordering_property);
    RUN_TEST(test_large_values);
    TEST_SUMMARY();
}
