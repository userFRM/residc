/*
 * test_mfu.c — Unit tests for MFU (Most Frequently Used) table
 */
#include "test_framework.h"
#include "residc.h"

/* ---- Init: all hash entries == 0xFFFF ---- */
static void test_init_hash(void)
{
    residc_mfu_table_t mfu;
    residc_mfu_init(&mfu);
    for (int i = 0; i < 256; i++) {
        ASSERT_EQ(mfu.hash[i], 0xFFFF);
    }
    ASSERT_EQ(mfu.num_entries, 0);
}

/* ---- Lookup miss returns -1 ---- */
static void test_lookup_miss(void)
{
    residc_mfu_table_t mfu;
    residc_mfu_init(&mfu);
    ASSERT_EQ(residc_mfu_lookup(&mfu, 42), -1);
    ASSERT_EQ(residc_mfu_lookup(&mfu, 0), -1);
    ASSERT_EQ(residc_mfu_lookup(&mfu, 16383), -1);
}

/* ---- Update + lookup returns valid index ---- */
static void test_update_then_lookup(void)
{
    residc_mfu_table_t mfu;
    residc_mfu_init(&mfu);
    residc_mfu_update(&mfu, 42);
    int idx = residc_mfu_lookup(&mfu, 42);
    ASSERT_TRUE(idx >= 0);
    ASSERT_EQ(mfu.entries[idx].instrument_id, 42);
    ASSERT_EQ(mfu.entries[idx].count, 1);
    ASSERT_EQ(mfu.num_entries, 1);
}

/* ---- Multiple updates increment count ---- */
static void test_update_increments_count(void)
{
    residc_mfu_table_t mfu;
    residc_mfu_init(&mfu);
    residc_mfu_update(&mfu, 100);
    residc_mfu_update(&mfu, 100);
    residc_mfu_update(&mfu, 100);
    int idx = residc_mfu_lookup(&mfu, 100);
    ASSERT_TRUE(idx >= 0);
    ASSERT_EQ(mfu.entries[idx].count, 3);
}

/* ---- Fill 256 entries, verify all findable ---- */
static void test_fill_256(void)
{
    residc_mfu_table_t mfu;
    residc_mfu_init(&mfu);

    for (int i = 0; i < 256; i++)
        residc_mfu_update(&mfu, (uint16_t)i);

    ASSERT_EQ(mfu.num_entries, 256);

    for (int i = 0; i < 256; i++) {
        int idx = residc_mfu_lookup(&mfu, (uint16_t)i);
        ASSERT_TRUE(idx >= 0);
        ASSERT_EQ(mfu.entries[idx].instrument_id, (uint16_t)i);
    }
}

/* ---- Overflow: add 257th entry, verify it replaces min-count ---- */
static void test_overflow_replaces_min(void)
{
    residc_mfu_table_t mfu;
    residc_mfu_init(&mfu);

    /* Fill all 256 entries with count=1 */
    for (int i = 0; i < RESIDC_MFU_SIZE; i++)
        residc_mfu_update(&mfu, (uint16_t)i);

    /* Bump entry 0 to count=10 so it won't be the minimum */
    for (int j = 0; j < 9; j++)
        residc_mfu_update(&mfu, 0);

    ASSERT_EQ(mfu.num_entries, RESIDC_MFU_SIZE);

    /* Add a new entry (ID 9999). It should replace one of the count=1 entries */
    residc_mfu_update(&mfu, 9999);

    int idx = residc_mfu_lookup(&mfu, 9999);
    ASSERT_TRUE(idx >= 0);
    ASSERT_EQ(mfu.entries[idx].instrument_id, 9999);

    /* Entry 0 should still be there (it had count=10) */
    int idx0 = residc_mfu_lookup(&mfu, 0);
    ASSERT_TRUE(idx0 >= 0);

    /* num_entries stays at RESIDC_MFU_SIZE */
    ASSERT_EQ(mfu.num_entries, RESIDC_MFU_SIZE);
}

/* ---- Seed with known data ---- */
static void test_seed(void)
{
    residc_mfu_table_t mfu;
    uint16_t ids[] = {100, 200, 300, 400, 500};
    uint16_t counts[] = {50, 40, 30, 20, 10};

    residc_mfu_seed(&mfu, ids, counts, 5);

    ASSERT_EQ(mfu.num_entries, 5);

    for (int i = 0; i < 5; i++) {
        int idx = residc_mfu_lookup(&mfu, ids[i]);
        ASSERT_TRUE(idx >= 0);
        ASSERT_EQ(mfu.entries[idx].instrument_id, ids[i]);
        ASSERT_EQ(mfu.entries[idx].count, counts[i]);
    }

    /* Verify non-seeded ID not found */
    ASSERT_EQ(residc_mfu_lookup(&mfu, 999), -1);
}

/* ---- Seed respects RESIDC_MFU_SIZE cap ---- */
static void test_seed_cap(void)
{
    residc_mfu_table_t mfu;
    uint16_t ids[300];
    uint16_t counts[300];
    for (int i = 0; i < 300; i++) {
        ids[i] = (uint16_t)(i + 1000);
        counts[i] = (uint16_t)(300 - i);
    }
    residc_mfu_seed(&mfu, ids, counts, 300);
    ASSERT_EQ(mfu.num_entries, RESIDC_MFU_SIZE);
}

/* ---- Hash collision handling ---- */
static void test_hash_collisions(void)
{
    residc_mfu_table_t mfu;
    residc_mfu_init(&mfu);

    /* Find two IDs that hash to the same bucket: hash = (id * 157) & 0xFF */
    /* id=0: hash=0, id=256: hash = (256*157)&0xFF = (40192)&0xFF = 0 */
    /* So 0 and 256 collide */
    residc_mfu_update(&mfu, 0);
    residc_mfu_update(&mfu, 256);

    ASSERT_TRUE(residc_mfu_lookup(&mfu, 0) >= 0);
    ASSERT_TRUE(residc_mfu_lookup(&mfu, 256) >= 0);
    ASSERT_NEQ(residc_mfu_lookup(&mfu, 0), residc_mfu_lookup(&mfu, 256));
}

/* ---- Rank ordering after seed ---- */
static void test_rank_ordering(void)
{
    residc_mfu_table_t mfu;
    uint16_t ids[] = {10, 20, 30};
    uint16_t counts[] = {100, 50, 200};  /* 30 is most frequent */

    residc_mfu_seed(&mfu, ids, counts, 3);

    /* Rank 0 should be the highest count (id=30, count=200) */
    int rank0_idx = mfu.rank_to_idx[0];
    ASSERT_EQ(mfu.entries[rank0_idx].instrument_id, 30);

    /* Rank 1 should be id=10 (count=100) */
    int rank1_idx = mfu.rank_to_idx[1];
    ASSERT_EQ(mfu.entries[rank1_idx].instrument_id, 10);

    /* Rank 2 should be id=20 (count=50) */
    int rank2_idx = mfu.rank_to_idx[2];
    ASSERT_EQ(mfu.entries[rank2_idx].instrument_id, 20);
}

int main(void)
{
    printf("=== test_mfu ===\n");
    RUN_TEST(test_init_hash);
    RUN_TEST(test_lookup_miss);
    RUN_TEST(test_update_then_lookup);
    RUN_TEST(test_update_increments_count);
    RUN_TEST(test_fill_256);
    RUN_TEST(test_overflow_replaces_min);
    RUN_TEST(test_seed);
    RUN_TEST(test_seed_cap);
    RUN_TEST(test_hash_collisions);
    RUN_TEST(test_rank_ordering);
    TEST_SUMMARY();
}
