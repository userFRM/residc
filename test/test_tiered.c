/*
 * test_tiered.c — Unit tests for tiered residual coding
 */
#include "test_framework.h"
#include "residc.h"
#include <stdint.h>

/* Helper: encode a value, decode it, verify roundtrip */
static int64_t roundtrip_tiered(int64_t value, int k)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);
    residc_encode_residual(&bw, value, k);
    int len = residc_bw_finish(&bw);

    residc_bitreader_t br;
    residc_br_init(&br, bw.buf, len);
    return residc_decode_residual(&br, k);
}

/* ---- Tier 0: value=0, k=3 ---- */
static void test_tier0_zero(void)
{
    ASSERT_EQ(roundtrip_tiered(0, 3), 0);
}

/* ---- Tier 0: small positive, k=3. Values with |zigzag| < 2^k ---- */
static void test_tier0_small_positive(void)
{
    /* zigzag(1)=2, 2 < 2^3=8, so tier 0 */
    ASSERT_EQ(roundtrip_tiered(1, 3), 1);
    ASSERT_EQ(roundtrip_tiered(2, 3), 2);
    ASSERT_EQ(roundtrip_tiered(3, 3), 3);
    ASSERT_EQ(roundtrip_tiered(-1, 3), -1);
    ASSERT_EQ(roundtrip_tiered(-2, 3), -2);
    ASSERT_EQ(roundtrip_tiered(-3, 3), -3);
}

/* ---- Tier 1: values where zigzag >= 2^k but < 2^(k+2) ---- */
static void test_tier1(void)
{
    /* k=3: tier0 < 8, tier1 < 32 */
    /* zigzag(4) = 8, zigzag(5)=10, zigzag(15)=30 */
    ASSERT_EQ(roundtrip_tiered(4, 3), 4);
    ASSERT_EQ(roundtrip_tiered(-4, 3), -4);
    ASSERT_EQ(roundtrip_tiered(15, 3), 15);
    ASSERT_EQ(roundtrip_tiered(-15, 3), -15);
}

/* ---- Tier 2: values where zigzag >= 2^(k+2) but < 2^(k+5) ---- */
static void test_tier2(void)
{
    /* k=3: tier1 < 32, tier2 < 256 */
    ASSERT_EQ(roundtrip_tiered(16, 3), 16);
    ASSERT_EQ(roundtrip_tiered(-16, 3), -16);
    ASSERT_EQ(roundtrip_tiered(100, 3), 100);
    ASSERT_EQ(roundtrip_tiered(-100, 3), -100);
    ASSERT_EQ(roundtrip_tiered(127, 3), 127);
    ASSERT_EQ(roundtrip_tiered(-127, 3), -127);
}

/* ---- Tier 3: values where zigzag >= 2^(k+5) but < 2^(k+10) ---- */
static void test_tier3(void)
{
    /* k=3: tier2 < 256, tier3 < 8192 */
    ASSERT_EQ(roundtrip_tiered(128, 3), 128);
    ASSERT_EQ(roundtrip_tiered(-128, 3), -128);
    ASSERT_EQ(roundtrip_tiered(1000, 3), 1000);
    ASSERT_EQ(roundtrip_tiered(-1000, 3), -1000);
    ASSERT_EQ(roundtrip_tiered(4095, 3), 4095);
    ASSERT_EQ(roundtrip_tiered(-4095, 3), -4095);
}

/* ---- Tier 4: extreme values that don't fit in tier 3 ---- */
static void test_tier4_extreme(void)
{
    ASSERT_EQ(roundtrip_tiered(INT64_MAX, 3), INT64_MAX);
    ASSERT_EQ(roundtrip_tiered(INT64_MIN, 3), INT64_MIN);
    ASSERT_EQ(roundtrip_tiered(1000000000LL, 3), 1000000000LL);
    ASSERT_EQ(roundtrip_tiered(-1000000000LL, 3), -1000000000LL);
}

/* ---- Various k values ---- */
static void test_k0(void)
{
    /* k=0: tier0 < 1 (only 0), tier1 < 4, tier2 < 32, tier3 < 1024 */
    ASSERT_EQ(roundtrip_tiered(0, 0), 0);
    ASSERT_EQ(roundtrip_tiered(1, 0), 1);
    ASSERT_EQ(roundtrip_tiered(-1, 0), -1);
    ASSERT_EQ(roundtrip_tiered(500, 0), 500);
    ASSERT_EQ(roundtrip_tiered(-500, 0), -500);
    ASSERT_EQ(roundtrip_tiered(100000, 0), 100000);
}

static void test_k10(void)
{
    /* k=10: tier0 < 1024, tier1 < 4096, tier2 < 32768, tier3 < 1048576 */
    ASSERT_EQ(roundtrip_tiered(0, 10), 0);
    ASSERT_EQ(roundtrip_tiered(511, 10), 511);
    ASSERT_EQ(roundtrip_tiered(-511, 10), -511);
    ASSERT_EQ(roundtrip_tiered(2000, 10), 2000);
    ASSERT_EQ(roundtrip_tiered(500000, 10), 500000);
    ASSERT_EQ(roundtrip_tiered(INT64_MAX, 10), INT64_MAX);
}

static void test_k20(void)
{
    /* k=20: much larger tiers */
    ASSERT_EQ(roundtrip_tiered(0, 20), 0);
    ASSERT_EQ(roundtrip_tiered(524287, 20), 524287);
    ASSERT_EQ(roundtrip_tiered(-524287, 20), -524287);
    ASSERT_EQ(roundtrip_tiered(1000000, 20), 1000000);
    ASSERT_EQ(roundtrip_tiered(INT64_MIN, 20), INT64_MIN);
}

/* ---- Negative value roundtrip coverage ---- */
static void test_negative_range(void)
{
    for (int k = 0; k <= 10; k += 5) {
        for (int64_t v = -200; v < 0; v++) {
            ASSERT_EQ(roundtrip_tiered(v, k), v);
        }
    }
}

/* ---- Tier boundary values for k=3 ---- */
static void test_tier_boundaries_k3(void)
{
    /* Tier 0/1 boundary: zigzag value = 2^3 = 8, which is zigzag(4)=8 */
    ASSERT_EQ(roundtrip_tiered(3, 3), 3);   /* zigzag(3)=6 < 8, tier 0 */
    ASSERT_EQ(roundtrip_tiered(4, 3), 4);   /* zigzag(4)=8 >= 8, tier 1 */
    ASSERT_EQ(roundtrip_tiered(-4, 3), -4); /* zigzag(-4)=7 < 8, tier 0 */

    /* Tier 1/2 boundary: zigzag value = 2^5 = 32, which is zigzag(16)=32 */
    ASSERT_EQ(roundtrip_tiered(15, 3), 15);   /* zigzag(15)=30 < 32, tier 1 */
    ASSERT_EQ(roundtrip_tiered(16, 3), 16);   /* zigzag(16)=32 >= 32, tier 2 */

    /* Tier 2/3 boundary: zigzag value = 2^8 = 256, which is zigzag(128)=256 */
    ASSERT_EQ(roundtrip_tiered(127, 3), 127);  /* zigzag(127)=254 < 256, tier 2 */
    ASSERT_EQ(roundtrip_tiered(128, 3), 128);  /* zigzag(128)=256 >= 256, tier 3 */

    /* Tier 3/4 boundary: zigzag value = 2^13 = 8192, which is zigzag(4096)=8192 */
    ASSERT_EQ(roundtrip_tiered(4095, 3), 4095);  /* zigzag(4095)=8190 < 8192, tier 3 */
    ASSERT_EQ(roundtrip_tiered(4096, 3), 4096);  /* zigzag(4096)=8192 >= 8192, tier 4 */
}

/* ---- Exp-Golomb roundtrip ---- */
static void test_expgolomb_roundtrip(void)
{
    residc_bitwriter_t bw;
    residc_bitreader_t br;

    int64_t vals[] = {0, 1, -1, 2, -2, 100, -100, 10000, -10000,
                      INT64_MAX, INT64_MIN, 1000000000LL};
    int ks[] = {0, 3, 5, 10};

    for (int ki = 0; ki < 4; ki++) {
        int k = ks[ki];
        for (int vi = 0; vi < 12; vi++) {
            residc_bw_init(&bw);
            residc_encode_residual_expg(&bw, vals[vi], k);
            int len = residc_bw_finish(&bw);
            residc_br_init(&br, bw.buf, len);
            int64_t decoded = residc_decode_residual_expg(&br, k);
            ASSERT_EQ(decoded, vals[vi]);
        }
    }
}

int main(void)
{
    printf("=== test_tiered ===\n");
    RUN_TEST(test_tier0_zero);
    RUN_TEST(test_tier0_small_positive);
    RUN_TEST(test_tier1);
    RUN_TEST(test_tier2);
    RUN_TEST(test_tier3);
    RUN_TEST(test_tier4_extreme);
    RUN_TEST(test_k0);
    RUN_TEST(test_k10);
    RUN_TEST(test_k20);
    RUN_TEST(test_negative_range);
    RUN_TEST(test_tier_boundaries_k3);
    RUN_TEST(test_expgolomb_roundtrip);
    TEST_SUMMARY();
}
