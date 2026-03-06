/*
 * test_bitio.c — Unit tests for bit writer and bit reader
 */
#include "test_framework.h"
#include "residc.h"

/* ---- Write N bits, read back, verify ---- */

static void test_write_read_1bit(void)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);
    residc_bw_write(&bw, 1, 1);
    int len = residc_bw_finish(&bw);
    ASSERT_TRUE(len >= 1);

    residc_bitreader_t br;
    residc_br_init(&br, bw.buf, len);
    uint64_t val = residc_br_read(&br, 1);
    ASSERT_EQ(val, 1);
    ASSERT_EQ(br.error, 0);
}

static void test_write_read_7bits(void)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);
    residc_bw_write(&bw, 0x5A, 7);  /* 1011010 */
    int len = residc_bw_finish(&bw);

    residc_bitreader_t br;
    residc_br_init(&br, bw.buf, len);
    uint64_t val = residc_br_read(&br, 7);
    ASSERT_EQ(val, 0x5A);
    ASSERT_EQ(br.error, 0);
}

static void test_write_read_8bits(void)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);
    residc_bw_write(&bw, 0xAB, 8);
    int len = residc_bw_finish(&bw);
    ASSERT_EQ(len, 1);

    residc_bitreader_t br;
    residc_br_init(&br, bw.buf, len);
    uint64_t val = residc_br_read(&br, 8);
    ASSERT_EQ(val, 0xAB);
    ASSERT_EQ(br.error, 0);
}

static void test_write_read_13bits(void)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);
    residc_bw_write(&bw, 0x1ABC, 13);
    int len = residc_bw_finish(&bw);

    residc_bitreader_t br;
    residc_br_init(&br, bw.buf, len);
    uint64_t val = residc_br_read(&br, 13);
    ASSERT_EQ(val, 0x1ABC);
    ASSERT_EQ(br.error, 0);
}

static void test_write_read_16bits(void)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);
    residc_bw_write(&bw, 0xDEAD, 16);
    int len = residc_bw_finish(&bw);
    ASSERT_EQ(len, 2);

    residc_bitreader_t br;
    residc_br_init(&br, bw.buf, len);
    uint64_t val = residc_br_read(&br, 16);
    ASSERT_EQ(val, 0xDEAD);
    ASSERT_EQ(br.error, 0);
}

static void test_write_read_31bits(void)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);
    residc_bw_write(&bw, 0x7ABCDEF0ULL, 31);
    int len = residc_bw_finish(&bw);

    residc_bitreader_t br;
    residc_br_init(&br, bw.buf, len);
    uint64_t val = residc_br_read(&br, 31);
    ASSERT_EQ(val, 0x7ABCDEF0ULL & ((1ULL << 31) - 1));
    ASSERT_EQ(br.error, 0);
}

static void test_write_read_32bits(void)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);
    residc_bw_write(&bw, 0xDEADBEEFULL, 32);
    int len = residc_bw_finish(&bw);
    ASSERT_EQ(len, 4);

    residc_bitreader_t br;
    residc_br_init(&br, bw.buf, len);
    uint64_t val = residc_br_read(&br, 32);
    ASSERT_EQ(val, 0xDEADBEEFULL);
    ASSERT_EQ(br.error, 0);
}

/* ---- Multiple small writes that trigger flush ---- */

static void test_multiple_small_writes(void)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);

    /* Write 40 x 8-bit values = 320 bits = 40 bytes */
    for (int i = 0; i < 40; i++)
        residc_bw_write(&bw, (uint64_t)(i & 0xFF), 8);

    int len = residc_bw_finish(&bw);
    ASSERT_EQ(len, 40);

    residc_bitreader_t br;
    residc_br_init(&br, bw.buf, len);
    for (int i = 0; i < 40; i++) {
        uint64_t val = residc_br_read(&br, 8);
        ASSERT_EQ(val, (uint64_t)(i & 0xFF));
    }
    ASSERT_EQ(br.error, 0);
}

/* ---- Write ~280 bytes worth of data (near RESIDC_SCRATCH_BYTES=320 limit) ---- */

static void test_near_scratch_limit(void)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);

    /* Write 280 bytes = 2240 bits using 8-bit writes */
    int num_bytes = 280;
    for (int i = 0; i < num_bytes; i++)
        residc_bw_write(&bw, (uint64_t)(i & 0xFF), 8);

    int len = residc_bw_finish(&bw);
    ASSERT_EQ(len, num_bytes);

    residc_bitreader_t br;
    residc_br_init(&br, bw.buf, len);
    for (int i = 0; i < num_bytes; i++) {
        uint64_t val = residc_br_read(&br, 8);
        ASSERT_EQ(val, (uint64_t)(i & 0xFF));
    }
    ASSERT_EQ(br.error, 0);
}

/* ---- Read from truncated stream sets br.error ---- */

static void test_truncated_read(void)
{
    uint8_t data[1] = {0xAB};
    residc_bitreader_t br;
    residc_br_init(&br, data, 1);

    /* Read 8 bits — should succeed */
    uint64_t val = residc_br_read(&br, 8);
    ASSERT_EQ(val, 0xAB);
    ASSERT_EQ(br.error, 0);

    /* Now try to read 1 more bit — should fail */
    (void)residc_br_read(&br, 1);
    ASSERT_EQ(br.error, 1);
}

static void test_truncated_read_bit(void)
{
    uint8_t data[1] = {0x80};  /* 10000000 */
    residc_bitreader_t br;
    residc_br_init(&br, data, 1);

    /* Read 8 individual bits */
    for (int i = 0; i < 8; i++) {
        int bit = residc_br_read_bit(&br);
        if (i == 0) ASSERT_EQ(bit, 1);
        else ASSERT_EQ(bit, 0);
    }
    ASSERT_EQ(br.error, 0);

    /* 9th bit should trigger error */
    (void)residc_br_read_bit(&br);
    ASSERT_EQ(br.error, 1);
}

/* ---- Empty reader ---- */

static void test_empty_reader(void)
{
    residc_bitreader_t br;
    residc_br_init(&br, NULL, 0);
    (void)residc_br_read(&br, 1);
    ASSERT_EQ(br.error, 1);
}

/* ---- Mixed width writes/reads ---- */

static void test_mixed_widths(void)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);

    residc_bw_write(&bw, 1, 1);        /* 1 bit */
    residc_bw_write(&bw, 0x3F, 7);     /* 7 bits */
    residc_bw_write(&bw, 0xBEEF, 16);  /* 16 bits */
    residc_bw_write(&bw, 0, 3);        /* 3 bits */
    residc_bw_write(&bw, 0x1F, 5);     /* 5 bits */

    int len = residc_bw_finish(&bw);

    residc_bitreader_t br;
    residc_br_init(&br, bw.buf, len);

    ASSERT_EQ(residc_br_read(&br, 1), 1);
    ASSERT_EQ(residc_br_read(&br, 7), 0x3F);
    ASSERT_EQ(residc_br_read(&br, 16), 0xBEEF);
    ASSERT_EQ(residc_br_read(&br, 3), 0);
    ASSERT_EQ(residc_br_read(&br, 5), 0x1F);
    ASSERT_EQ(br.error, 0);
}

/* ---- Large writes spanning multiple flushes ---- */

static void test_large_writes(void)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);

    /* Write two 32-bit values back to back */
    residc_bw_write(&bw, 0xCAFEBABEULL, 32);
    residc_bw_write(&bw, 0x12345678ULL, 32);

    int len = residc_bw_finish(&bw);
    ASSERT_EQ(len, 8);

    residc_bitreader_t br;
    residc_br_init(&br, bw.buf, len);
    ASSERT_EQ(residc_br_read(&br, 32), 0xCAFEBABEULL);
    ASSERT_EQ(residc_br_read(&br, 32), 0x12345678ULL);
    ASSERT_EQ(br.error, 0);
}

int main(void)
{
    printf("=== test_bitio ===\n");
    RUN_TEST(test_write_read_1bit);
    RUN_TEST(test_write_read_7bits);
    RUN_TEST(test_write_read_8bits);
    RUN_TEST(test_write_read_13bits);
    RUN_TEST(test_write_read_16bits);
    RUN_TEST(test_write_read_31bits);
    RUN_TEST(test_write_read_32bits);
    RUN_TEST(test_multiple_small_writes);
    RUN_TEST(test_near_scratch_limit);
    RUN_TEST(test_truncated_read);
    RUN_TEST(test_truncated_read_bit);
    RUN_TEST(test_empty_reader);
    RUN_TEST(test_mixed_widths);
    RUN_TEST(test_large_writes);
    TEST_SUMMARY();
}
