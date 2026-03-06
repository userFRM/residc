/*
 * test_roundtrip.c — Full message encode/decode roundtrip tests
 */
#include "test_framework.h"
#include "residc.h"
#include <stddef.h>

/* ---- Quote struct and schema ---- */

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

/* Simple PRNG for reproducible tests */
static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* ---- 1000-message roundtrip ---- */
static void test_1000_messages(void)
{
    residc_state_t *enc = calloc(1, sizeof(residc_state_t));
    residc_state_t *dec = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(enc != NULL);
    ASSERT_TRUE(dec != NULL);

    residc_init(enc, &schema);
    residc_init(dec, &schema);

    uint32_t rng = 12345;
    uint64_t base_ts = 34200000000000ULL;

    for (int i = 0; i < 1000; i++) {
        Quote q;
        q.ts = base_ts + (uint64_t)i * 1000000;  /* 1ms increments */
        q.inst = (uint16_t)(xorshift32(&rng) % 50);
        q.price = 1500000 + (xorshift32(&rng) % 200) - 100;
        q.qty = (xorshift32(&rng) % 10 + 1) * 100;
        q.side = (uint8_t)(xorshift32(&rng) % 2);

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

    free(enc);
    free(dec);
}

/* ---- First message is always literal (no prior state) ---- */
static void test_first_message_roundtrip(void)
{
    residc_state_t *enc = calloc(1, sizeof(residc_state_t));
    residc_state_t *dec = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(enc != NULL);
    ASSERT_TRUE(dec != NULL);

    residc_init(enc, &schema);
    residc_init(dec, &schema);

    Quote q = { .ts = 34200000000000ULL, .inst = 42,
                .price = 1500250, .qty = 500, .side = 1 };

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

    free(enc);
    free(dec);
}

/* ---- Snapshot/restore ---- */
static void test_snapshot_restore(void)
{
    residc_state_t *enc = calloc(1, sizeof(residc_state_t));
    residc_state_t *dec = calloc(1, sizeof(residc_state_t));
    residc_state_t *enc_snap = calloc(1, sizeof(residc_state_t));
    residc_state_t *dec_snap = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(enc != NULL && dec != NULL);
    ASSERT_TRUE(enc_snap != NULL && dec_snap != NULL);

    residc_init(enc, &schema);
    residc_init(dec, &schema);

    uint32_t rng = 99999;
    uint64_t base_ts = 34200000000000ULL;

    /* Encode 50 messages */
    for (int i = 0; i < 50; i++) {
        Quote q;
        q.ts = base_ts + (uint64_t)i * 1000000;
        q.inst = (uint16_t)(xorshift32(&rng) % 20);
        q.price = 2000000 + (xorshift32(&rng) % 100) - 50;
        q.qty = (xorshift32(&rng) % 5 + 1) * 100;
        q.side = (uint8_t)(xorshift32(&rng) % 2);

        uint8_t buf[128];
        int len = residc_encode(enc, &q, buf, sizeof(buf));
        ASSERT_TRUE(len > 0);
        int consumed = residc_decode(dec, buf, len, &(Quote){0});
        ASSERT_TRUE(consumed > 0);
    }

    /* Take snapshot */
    residc_snapshot(enc, enc_snap);
    residc_snapshot(dec, dec_snap);

    /* Encode 50 more */
    uint32_t rng_save = rng;
    for (int i = 50; i < 100; i++) {
        Quote q;
        q.ts = base_ts + (uint64_t)i * 1000000;
        q.inst = (uint16_t)(xorshift32(&rng) % 20);
        q.price = 2000000 + (xorshift32(&rng) % 100) - 50;
        q.qty = (xorshift32(&rng) % 5 + 1) * 100;
        q.side = (uint8_t)(xorshift32(&rng) % 2);

        uint8_t buf[128];
        int len = residc_encode(enc, &q, buf, sizeof(buf));
        ASSERT_TRUE(len > 0);
    }

    /* Restore from snapshot */
    residc_restore(enc, enc_snap);
    residc_restore(dec, dec_snap);

    /* Re-encode same 50 messages (replay from snapshot) */
    rng = rng_save;
    for (int i = 50; i < 100; i++) {
        Quote q;
        q.ts = base_ts + (uint64_t)i * 1000000;
        q.inst = (uint16_t)(xorshift32(&rng) % 20);
        q.price = 2000000 + (xorshift32(&rng) % 100) - 50;
        q.qty = (xorshift32(&rng) % 5 + 1) * 100;
        q.side = (uint8_t)(xorshift32(&rng) % 2);

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

    free(enc);
    free(dec);
    free(enc_snap);
    free(dec_snap);
}

/* ---- Reset: encode msgs, reset both, encode same msgs — same output ---- */
static void test_reset_deterministic(void)
{
    residc_state_t *enc = calloc(1, sizeof(residc_state_t));
    residc_state_t *dec = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(enc != NULL && dec != NULL);

    residc_init(enc, &schema);
    residc_init(dec, &schema);

    Quote msgs[10];
    uint8_t first_pass[10][128];
    int first_lens[10];

    uint32_t rng = 77777;
    for (int i = 0; i < 10; i++) {
        msgs[i].ts = 34200000000000ULL + (uint64_t)i * 500000;
        msgs[i].inst = (uint16_t)(xorshift32(&rng) % 10);
        msgs[i].price = 1000000 + (xorshift32(&rng) % 50);
        msgs[i].qty = (xorshift32(&rng) % 3 + 1) * 100;
        msgs[i].side = (uint8_t)(xorshift32(&rng) % 2);
    }

    /* First pass */
    for (int i = 0; i < 10; i++) {
        first_lens[i] = residc_encode(enc, &msgs[i], first_pass[i], 128);
        ASSERT_TRUE(first_lens[i] > 0);
    }

    /* Reset */
    residc_reset(enc);
    residc_reset(dec);

    /* Second pass — should produce identical output */
    for (int i = 0; i < 10; i++) {
        uint8_t buf[128];
        int len = residc_encode(enc, &msgs[i], buf, 128);
        ASSERT_EQ(len, first_lens[i]);
        ASSERT_MEM_EQ(buf, first_pass[i], len);

        /* Decode should also work */
        Quote decoded;
        int consumed = residc_decode(dec, buf, len, &decoded);
        ASSERT_TRUE(consumed > 0);
        ASSERT_EQ(decoded.ts, msgs[i].ts);
        ASSERT_EQ(decoded.inst, msgs[i].inst);
        ASSERT_EQ(decoded.price, msgs[i].price);
        ASSERT_EQ(decoded.qty, msgs[i].qty);
        ASSERT_EQ(decoded.side, msgs[i].side);
    }

    free(enc);
    free(dec);
}

/* ---- Compression actually works (compressed < raw) after warm-up ---- */
static void test_compression_ratio(void)
{
    residc_state_t *enc = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(enc != NULL);
    residc_init(enc, &schema);

    uint64_t total_raw = 0;
    uint64_t total_compressed = 0;
    int raw_size = residc_raw_size(&schema);

    for (int i = 0; i < 500; i++) {
        Quote q;
        q.ts = 34200000000000ULL + (uint64_t)i * 1000000;
        q.inst = (uint16_t)(i % 5);
        q.price = 1500000 + (i % 3);
        q.qty = 100;
        q.side = 0;

        uint8_t buf[128];
        int len = residc_encode(enc, &q, buf, sizeof(buf));
        ASSERT_TRUE(len > 0);
        total_compressed += (uint64_t)len;
        total_raw += (uint64_t)raw_size;
    }

    /* Predictable data should compress well below raw */
    ASSERT_TRUE(total_compressed < total_raw);

    free(enc);
}

/* ---- Same instrument repeated (tests per-instrument prediction) ---- */
static void test_same_instrument_prediction(void)
{
    residc_state_t *enc = calloc(1, sizeof(residc_state_t));
    residc_state_t *dec = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(enc != NULL && dec != NULL);

    residc_init(enc, &schema);
    residc_init(dec, &schema);

    /* Same instrument, incrementing price by 1 cent */
    for (int i = 0; i < 100; i++) {
        Quote q;
        q.ts = 34200000000000ULL + (uint64_t)i * 1000000;
        q.inst = 42;
        q.price = 1500000 + (uint32_t)i * 100;  /* +1 cent each */
        q.qty = 100;
        q.side = 0;

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

    free(enc);
    free(dec);
}

/* ---- Exp-Golomb coder roundtrip ---- */
static void test_expgolomb_roundtrip(void)
{
    residc_state_t *enc = calloc(1, sizeof(residc_state_t));
    residc_state_t *dec = calloc(1, sizeof(residc_state_t));
    ASSERT_TRUE(enc != NULL && dec != NULL);

    residc_init(enc, &schema);
    residc_init(dec, &schema);
    residc_set_coder(enc, RESIDC_CODER_EXPGOLOMB);
    residc_set_coder(dec, RESIDC_CODER_EXPGOLOMB);

    uint32_t rng = 54321;
    for (int i = 0; i < 200; i++) {
        Quote q;
        q.ts = 34200000000000ULL + (uint64_t)i * 1000000;
        q.inst = (uint16_t)(xorshift32(&rng) % 20);
        q.price = 1500000 + (xorshift32(&rng) % 200) - 100;
        q.qty = (xorshift32(&rng) % 10 + 1) * 100;
        q.side = (uint8_t)(xorshift32(&rng) % 2);

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

    free(enc);
    free(dec);
}

int main(void)
{
    printf("=== test_roundtrip ===\n");
    RUN_TEST(test_1000_messages);
    RUN_TEST(test_first_message_roundtrip);
    RUN_TEST(test_snapshot_restore);
    RUN_TEST(test_reset_deterministic);
    RUN_TEST(test_compression_ratio);
    RUN_TEST(test_same_instrument_prediction);
    RUN_TEST(test_expgolomb_roundtrip);
    TEST_SUMMARY();
}
