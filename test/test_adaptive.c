/*
 * test_adaptive.c — Unit tests for adaptive k computation
 */
#include "test_framework.h"
#include "residc.h"

/* ---- count=0 returns min_k ---- */
static void test_count_zero(void)
{
    ASSERT_EQ(residc_adaptive_k(100, 0, 3, 20), 3);
    ASSERT_EQ(residc_adaptive_k(0, 0, 5, 15), 5);
    ASSERT_EQ(residc_adaptive_k(999, 0, 0, 10), 0);
}

/* ---- sum=0 returns min_k ---- */
static void test_sum_zero(void)
{
    ASSERT_EQ(residc_adaptive_k(0, 1, 3, 20), 3);
    ASSERT_EQ(residc_adaptive_k(0, 8, 5, 15), 5);
}

/* ---- sum=8, count=1 -> k=3 (floor(log2(8)) - floor(log2(1)) = 3 - 0 = 3) ---- */
static void test_sum8_count1(void)
{
    ASSERT_EQ(residc_adaptive_k(8, 1, 0, 20), 3);
}

/* ---- Various sum/count combinations ---- */
static void test_various_sums(void)
{
    /* sum=1, count=1 -> log2(1)-log2(1) = 0-0 = 0, clamped to min_k */
    ASSERT_EQ(residc_adaptive_k(1, 1, 3, 20), 3);

    /* sum=16, count=1 -> log2(16)-log2(1) = 4-0 = 4 */
    ASSERT_EQ(residc_adaptive_k(16, 1, 0, 20), 4);

    /* sum=32, count=2 -> log2(32)-log2(2) = 5-1 = 4 */
    ASSERT_EQ(residc_adaptive_k(32, 2, 0, 20), 4);

    /* sum=256, count=1 -> log2(256)-log2(1) = 8-0 = 8 */
    ASSERT_EQ(residc_adaptive_k(256, 1, 0, 20), 8);

    /* sum=1024, count=8 -> log2(1024)-log2(8) = 10-3 = 7 */
    ASSERT_EQ(residc_adaptive_k(1024, 8, 0, 20), 7);
}

/* ---- Clamping to min_k ---- */
static void test_clamp_min(void)
{
    /* sum=1, count=1 -> k=0, but min_k=5 -> result=5 */
    ASSERT_EQ(residc_adaptive_k(1, 1, 5, 20), 5);
}

/* ---- Clamping to max_k ---- */
static void test_clamp_max(void)
{
    /* sum=2^20=1048576, count=1 -> k=20, but max_k=10 -> result=10 */
    ASSERT_EQ(residc_adaptive_k(1048576, 1, 0, 10), 10);
}

/* ---- adaptive_update: basic accumulation ---- */
static void test_update_accumulate(void)
{
    uint64_t sum = 0;
    uint32_t count = 0;

    residc_adaptive_update(&sum, &count, 10);
    ASSERT_EQ(sum, 10ULL);
    ASSERT_EQ(count, 1);

    residc_adaptive_update(&sum, &count, 20);
    ASSERT_EQ(sum, 30ULL);
    ASSERT_EQ(count, 2);
}

/* ---- adaptive_update: window rollover at RESIDC_ADAPT_WINDOW (8) ---- */
static void test_update_window_rollover(void)
{
    uint64_t sum = 0;
    uint32_t count = 0;

    /* Add 7 values (count goes to 7, no rollover) */
    for (int i = 0; i < 7; i++)
        residc_adaptive_update(&sum, &count, 10);

    ASSERT_EQ(count, 7);
    ASSERT_EQ(sum, 70ULL);

    /* 8th update triggers rollover: sum >>= 1, count >>= 1 */
    residc_adaptive_update(&sum, &count, 10);
    /* Before rollover: sum=80, count=8. After: sum=40, count=4 */
    ASSERT_EQ(sum, 40ULL);
    ASSERT_EQ(count, 4);
}

/* ---- adaptive_update: multiple rollovers ---- */
static void test_update_multiple_rollovers(void)
{
    uint64_t sum = 0;
    uint32_t count = 0;

    /* Fill to window, trigger rollover, fill again */
    for (int i = 0; i < 16; i++)
        residc_adaptive_update(&sum, &count, 4);

    /* After 8 updates: sum=32, count=8 -> sum=16, count=4
     * After 4 more (total 12): sum=32, count=8 -> sum=16, count=4
     * After 4 more (total 16): sum=32, count=8 -> sum=16, count=4 */
    ASSERT_LE(count, (long long)RESIDC_ADAPT_WINDOW);
}

/* ---- k computation with realistic financial data pattern ---- */
static void test_realistic_pattern(void)
{
    uint64_t sum = 0;
    uint32_t count = 0;

    /* Simulate small price residuals (zigzag values 0-4) */
    uint64_t vals[] = {0, 2, 4, 0, 2, 0, 2, 0};
    for (int i = 0; i < 8; i++)
        residc_adaptive_update(&sum, &count, vals[i]);

    int k = residc_adaptive_k(sum, count, 3, 13);
    /* Sum of vals = 10, after rollover sum=5, count=4 */
    /* log2(5)=2, log2(4)=2, k=0, clamped to min=3 */
    ASSERT_GE(k, 3);
    ASSERT_LE(k, 13);
}

int main(void)
{
    printf("=== test_adaptive ===\n");
    RUN_TEST(test_count_zero);
    RUN_TEST(test_sum_zero);
    RUN_TEST(test_sum8_count1);
    RUN_TEST(test_various_sums);
    RUN_TEST(test_clamp_min);
    RUN_TEST(test_clamp_max);
    RUN_TEST(test_update_accumulate);
    RUN_TEST(test_update_window_rollover);
    RUN_TEST(test_update_multiple_rollovers);
    RUN_TEST(test_realistic_pattern);
    TEST_SUMMARY();
}
