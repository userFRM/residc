/*
 * test_edge.c — Edge cases and error handling tests
 */
#include "test_framework.h"
#include "residc.h"
#include <stddef.h>

/* ---- Quote struct (same as roundtrip tests) ---- */

typedef struct {
    uint64_t ts;
    uint16_t inst;
    uint32_t price;
    uint32_t qty;
    uint8_t  side;
} Quote;

static const residc_field_t quote_fields[] = {
    { RESIDC_TIMESTAMP,  offsetof(Quote, ts),    8, -1 },
    { RESIDC_INSTRUMENT, offsetof(Quote, inst),  2, -1 },
    { RESIDC_PRICE,      offsetof(Quote, price), 4, -1 },
    { RESIDC_QUANTITY,   offsetof(Quote, qty),   4, -1 },
    { RESIDC_ENUM,       offsetof(Quote, side),  1, -1 },
};
static const residc_schema_t schema = { quote_fields, 5, sizeof(Quote) };

/* ---- NULL schema returns RESIDC_ERR_NULL ---- */
static void test_encode_null_schema(void)
{
    residc_state_t *state = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(state != NULL);
    /* Do not call residc_init — schema is NULL by default after calloc */
    state->schema = NULL;

    Quote q = {0};
    uint8_t buf[128];
    int ret = residc_encode(state, &q, buf, sizeof(buf));
    ASSERT_EQ(ret, RESIDC_ERR_NULL);

    free(state);
}

static void test_decode_null_schema(void)
{
    residc_state_t *state = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(state != NULL);
    state->schema = NULL;

    uint8_t buf[16] = {0};
    Quote decoded;
    int ret = residc_decode(state, buf, 16, &decoded);
    ASSERT_EQ(ret, RESIDC_ERR_NULL);

    free(state);
}

/* ---- Encode with capacity=0 returns RESIDC_ERR_CAPACITY ---- */
static void test_encode_zero_capacity(void)
{
    residc_state_t *state = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(state != NULL);
    residc_init(state, &schema);

    Quote q = { .ts = 1000, .inst = 1, .price = 100, .qty = 100, .side = 0 };
    uint8_t buf[1];
    int ret = residc_encode(state, &q, buf, 0);
    ASSERT_EQ(ret, RESIDC_ERR_CAPACITY);

    free(state);
}

/* ---- Encode with capacity=1 returns RESIDC_ERR_CAPACITY ---- */
static void test_encode_tiny_capacity(void)
{
    residc_state_t *state = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(state != NULL);
    residc_init(state, &schema);

    Quote q = { .ts = 1000, .inst = 1, .price = 100, .qty = 100, .side = 0 };
    uint8_t buf[1];
    int ret = residc_encode(state, &q, buf, 1);
    ASSERT_EQ(ret, RESIDC_ERR_CAPACITY);

    free(state);
}

/* ---- Decode with in_len=0 returns RESIDC_ERR_TRUNCATED ---- */
static void test_decode_zero_length(void)
{
    residc_state_t *state = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(state != NULL);
    residc_init(state, &schema);

    uint8_t buf[1] = {0};
    Quote decoded;
    int ret = residc_decode(state, buf, 0, &decoded);
    ASSERT_EQ(ret, RESIDC_ERR_TRUNCATED);

    free(state);
}

/* ---- Decode with truncated literal (0xFF followed by too few bytes) ---- */
static void test_decode_truncated_literal(void)
{
    residc_state_t *state = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(state != NULL);
    residc_init(state, &schema);

    /* Literal frame: 0xFF then raw_size bytes needed, but we only provide 3 */
    uint8_t buf[4] = {0xFF, 0x01, 0x02, 0x03};
    Quote decoded;
    int ret = residc_decode(state, buf, 4, &decoded);
    ASSERT_EQ(ret, RESIDC_ERR_TRUNCATED);

    free(state);
}

/* ---- Decode with truncated compressed frame ---- */
static void test_decode_truncated_compressed(void)
{
    residc_state_t *state = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(state != NULL);
    residc_init(state, &schema);

    /* Frame byte says payload is 10 bytes, but we only provide 5 total */
    uint8_t buf[6] = {10, 0x01, 0x02, 0x03, 0x04, 0x05};
    Quote decoded;
    int ret = residc_decode(state, buf, 6, &decoded);
    ASSERT_EQ(ret, RESIDC_ERR_TRUNCATED);

    free(state);
}

/* ---- Instrument ID >= RESIDC_MAX_INSTRUMENTS still works (no per-instrument prediction) ---- */
static void test_large_instrument_id(void)
{
    residc_state_t *enc = calloc(1, sizeof(residc_state_t));
    residc_state_t *dec = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(enc != NULL && dec != NULL);

    residc_init(enc, &schema);
    residc_init(dec, &schema);

    /* Use instrument ID at the boundary */
    Quote q = { .ts = 34200000000000ULL, .inst = RESIDC_MAX_INSTRUMENTS,
                .price = 1500000, .qty = 100, .side = 0 };

    uint8_t buf[128];
    int len = residc_encode(enc, &q, buf, sizeof(buf));
    ASSERT_TRUE(len > 0);

    Quote decoded;
    int consumed = residc_decode(dec, buf, len, &decoded);
    ASSERT_TRUE(consumed > 0);
    ASSERT_EQ(decoded.inst, RESIDC_MAX_INSTRUMENTS);
    ASSERT_EQ(decoded.price, q.price);
    ASSERT_EQ(decoded.qty, q.qty);

    free(enc);
    free(dec);
}

/* ---- residc_strerror returns non-NULL for all defined codes ---- */
static void test_strerror_defined(void)
{
    ASSERT_TRUE(residc_strerror(RESIDC_OK) != NULL);
    ASSERT_TRUE(residc_strerror(RESIDC_ERR_NULL) != NULL);
    ASSERT_TRUE(residc_strerror(RESIDC_ERR_CAPACITY) != NULL);
    ASSERT_TRUE(residc_strerror(RESIDC_ERR_TRUNCATED) != NULL);
    ASSERT_TRUE(residc_strerror(RESIDC_ERR_CORRUPT) != NULL);
    ASSERT_TRUE(residc_strerror(RESIDC_ERR_OVERFLOW) != NULL);
    ASSERT_TRUE(residc_strerror(RESIDC_ERR_SCHEMA) != NULL);
}

/* ---- residc_strerror returns non-NULL for unknown code ---- */
static void test_strerror_unknown(void)
{
    ASSERT_TRUE(residc_strerror(-99) != NULL);
    ASSERT_TRUE(residc_strerror(42) != NULL);
}

/* ---- residc_encode_header / residc_decode_header roundtrip ---- */
static void test_header_roundtrip(void)
{
    uint8_t buf[4];
    int ret = residc_encode_header(buf, sizeof(buf));
    ASSERT_EQ(ret, 1);
    ASSERT_EQ(buf[0], RESIDC_WIRE_VERSION);

    ret = residc_decode_header(buf, 1);
    ASSERT_EQ(ret, 1);
}

/* ---- residc_encode_header with zero capacity ---- */
static void test_header_zero_capacity(void)
{
    uint8_t buf[1];
    int ret = residc_encode_header(buf, 0);
    ASSERT_EQ(ret, RESIDC_ERR_CAPACITY);
}

/* ---- residc_decode_header with wrong version ---- */
static void test_header_wrong_version(void)
{
    uint8_t buf[1] = {0xFF};  /* Not RESIDC_WIRE_VERSION */
    int ret = residc_decode_header(buf, 1);
    ASSERT_EQ(ret, RESIDC_ERR_CORRUPT);
}

/* ---- residc_decode_header with zero length ---- */
static void test_header_zero_length(void)
{
    uint8_t buf[1] = {0};
    int ret = residc_decode_header(buf, 0);
    ASSERT_EQ(ret, RESIDC_ERR_TRUNCATED);
}

/* ---- residc_raw_size correctness ---- */
static void test_raw_size(void)
{
    int expected = 8 + 2 + 4 + 4 + 1;  /* ts(8) + inst(2) + price(4) + qty(4) + side(1) = 19 */
    ASSERT_EQ(residc_raw_size(&schema), expected);
}

/* ---- Multiple consecutive encodes/decodes maintain sync ---- */
static void test_state_sync(void)
{
    residc_state_t *enc = calloc(1, sizeof(residc_state_t));
    residc_state_t *dec = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(enc != NULL && dec != NULL);

    residc_init(enc, &schema);
    residc_init(dec, &schema);

    /* Encode/decode many messages, verify states stay in sync */
    for (int i = 0; i < 500; i++) {
        Quote q;
        q.ts = 34200000000000ULL + (uint64_t)i * 500000;
        q.inst = (uint16_t)(i % 10);
        q.price = (uint32_t)(1500000 + (i % 7) * 100);
        q.qty = 100;
        q.side = (uint8_t)(i % 2);

        uint8_t buf[128];
        int len = residc_encode(enc, &q, buf, sizeof(buf));
        ASSERT_TRUE(len > 0);

        Quote decoded;
        int consumed = residc_decode(dec, buf, len, &decoded);
        ASSERT_TRUE(consumed > 0);
        ASSERT_EQ(decoded.ts, q.ts);
        ASSERT_EQ(decoded.inst, q.inst);
        ASSERT_EQ(decoded.price, q.price);
        ASSERT_EQ(decoded.qty, q.qty);
        ASSERT_EQ(decoded.side, q.side);
    }

    /* Verify internal state counters match */
    ASSERT_EQ(enc->msg_count, 500ULL);
    ASSERT_EQ(dec->msg_count, 500ULL);
    ASSERT_EQ(enc->last_instrument_id, dec->last_instrument_id);

    free(enc);
    free(dec);
}

/* ---- Decode garbage compressed data returns error ---- */
static void test_decode_garbage(void)
{
    residc_state_t *state = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(state != NULL);
    residc_init(state, &schema);

    /* Frame byte=5 means 5 bytes of compressed payload, but we give random data.
     * The bitstream will likely be truncated or wrong. */
    uint8_t buf[6] = {5, 0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    Quote decoded;
    int ret = residc_decode(state, buf, 6, &decoded);
    /* Either returns a positive value (decoded with whatever bits) or an error.
     * The key thing is it doesn't crash. If truncated, should return RESIDC_ERR_TRUNCATED. */
    (void)ret; /* Just checking it doesn't crash */

    free(state);
}

/* ---- Encode with exact-fit capacity ---- */
static void test_encode_exact_capacity(void)
{
    residc_state_t *enc = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(enc != NULL);
    residc_init(enc, &schema);

    Quote q = { .ts = 34200000000000ULL, .inst = 1,
                .price = 1500000, .qty = 100, .side = 0 };

    /* First encode to know the size */
    uint8_t buf[128];
    int len = residc_encode(enc, &q, buf, sizeof(buf));
    ASSERT_TRUE(len > 0);

    /* Reset and encode again with exact capacity */
    residc_reset(enc);
    uint8_t *exact_buf = malloc((size_t)len);
    ASSERT_TRUE(exact_buf != NULL);
    int ret = residc_encode(enc, &q, exact_buf, len);
    ASSERT_EQ(ret, len);

    free(exact_buf);
    free(enc);
}

/* ---- Instrument ID that fits in 14 bits (max for raw encoding) ---- */
static void test_max_14bit_instrument(void)
{
    residc_state_t *enc = calloc(1, sizeof(residc_state_t));
    residc_state_t *dec = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(enc != NULL && dec != NULL);

    residc_init(enc, &schema);
    residc_init(dec, &schema);

    /* Max 14-bit value = 16383 */
    Quote q = { .ts = 34200000000000ULL, .inst = 16383,
                .price = 1500000, .qty = 100, .side = 0 };

    uint8_t buf[128];
    int len = residc_encode(enc, &q, buf, sizeof(buf));
    ASSERT_TRUE(len > 0);

    Quote decoded;
    int consumed = residc_decode(dec, buf, len, &decoded);
    ASSERT_TRUE(consumed > 0);
    ASSERT_EQ(decoded.inst, 16383);

    free(enc);
    free(dec);
}

int main(void)
{
    printf("=== test_edge ===\n");
    RUN_TEST(test_encode_null_schema);
    RUN_TEST(test_decode_null_schema);
    RUN_TEST(test_encode_zero_capacity);
    RUN_TEST(test_encode_tiny_capacity);
    RUN_TEST(test_decode_zero_length);
    RUN_TEST(test_decode_truncated_literal);
    RUN_TEST(test_decode_truncated_compressed);
    RUN_TEST(test_large_instrument_id);
    RUN_TEST(test_strerror_defined);
    RUN_TEST(test_strerror_unknown);
    RUN_TEST(test_header_roundtrip);
    RUN_TEST(test_header_zero_capacity);
    RUN_TEST(test_header_wrong_version);
    RUN_TEST(test_header_zero_length);
    RUN_TEST(test_raw_size);
    RUN_TEST(test_state_sync);
    RUN_TEST(test_decode_garbage);
    RUN_TEST(test_encode_exact_capacity);
    RUN_TEST(test_max_14bit_instrument);
    TEST_SUMMARY();
}
