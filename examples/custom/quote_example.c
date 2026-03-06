/*
 * quote_example.c — Minimal example of residc schema-driven compression
 *
 * Defines a simple quote message, compresses a stream of quotes,
 * verifies roundtrip, and reports compression ratio.
 *
 * Build:
 *   cc -O2 -o quote_example quote_example.c ../../core/residc.c -I../../core
 *
 * Run:
 *   ./quote_example
 */

#include "residc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Step 1: Define your message struct
 * ================================================================ */

typedef struct {
    uint64_t timestamp;       /* nanoseconds since midnight */
    uint16_t instrument_id;   /* security identifier */
    uint32_t bid_price;       /* bid price * 10000 */
    uint32_t ask_price;       /* ask price * 10000 */
    uint32_t bid_size;        /* bid quantity */
    uint32_t ask_size;        /* ask quantity */
    uint8_t  condition;       /* quote condition code */
} Quote;

/* ================================================================
 * Step 2: Define a schema
 *
 * Map each field to a residc field type. Order matters:
 * INSTRUMENT must come before PRICE/QUANTITY fields.
 * ================================================================ */

static const residc_field_t quote_fields[] = {
    { RESIDC_TIMESTAMP,    offsetof(Quote, timestamp),     8, -1 },
    { RESIDC_INSTRUMENT,   offsetof(Quote, instrument_id), 2, -1 },
    { RESIDC_PRICE,        offsetof(Quote, bid_price),     4, -1 },
    { RESIDC_DELTA_PRICE,  offsetof(Quote, ask_price),     4,  2 }, /* delta from bid */
    { RESIDC_QUANTITY,     offsetof(Quote, bid_size),      4, -1 },
    { RESIDC_QUANTITY,     offsetof(Quote, ask_size),      4, -1 },
    { RESIDC_ENUM,         offsetof(Quote, condition),     1, -1 },
};

static const residc_schema_t quote_schema = {
    .fields     = quote_fields,
    .num_fields = sizeof(quote_fields) / sizeof(quote_fields[0]),
    .msg_size   = sizeof(Quote),
};

/* ================================================================
 * Step 3: Generate sample data and compress
 * ================================================================ */

/* Simple PRNG */
static uint64_t rng = 0xCAFEBABE12345678ULL;
static uint32_t rng_next(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 32);
}

int main(void)
{
    const int N = 100000;

    /* Create encoder and decoder (heap-allocated — residc_state_t is ~331KB) */
    residc_state_t *enc = calloc(1, sizeof(residc_state_t));
    residc_state_t *dec = calloc(1, sizeof(residc_state_t));
    if (!enc || !dec) { fprintf(stderr, "allocation failed\n"); return 1; }
    residc_init(enc, &quote_schema);
    residc_init(dec, &quote_schema);

    long total_raw = 0;
    long total_compressed = 0;
    int errors = 0;

    uint64_t ts = 34200000000000ULL;  /* 9:30:00.000 AM */

    printf("residc schema-driven compression example\n");
    printf("=========================================\n");
    printf("Message: Quote (7 fields, %d bytes raw)\n",
           residc_raw_size(&quote_schema));
    printf("Messages: %d\n\n", N);

    for (int i = 0; i < N; i++) {
        /* Generate a realistic quote */
        Quote q;
        ts += 500 + (rng_next() % 50000);  /* ~0.5-50 μs gap */
        q.timestamp = ts;

        /* Instrument: Zipf-like (a few instruments dominate) */
        uint32_t r = rng_next() % 100;
        if (r < 40)      q.instrument_id = rng_next() % 5;
        else if (r < 70) q.instrument_id = 5 + rng_next() % 20;
        else              q.instrument_id = 25 + rng_next() % 200;

        /* Price: small movements around $150 */
        q.bid_price = 1500000 + (rng_next() % 2000) - 1000;
        /* Ask = bid + small spread (1-10 cents) */
        q.ask_price = q.bid_price + 100 + (rng_next() % 900);
        /* Sizes: typically round lots */
        q.bid_size = (1 + rng_next() % 20) * 100;
        q.ask_size = (1 + rng_next() % 20) * 100;
        /* Condition: usually 'R' (regular) */
        q.condition = (rng_next() % 10 == 0) ? 'C' : 'R';

        /* Encode */
        uint8_t buf[64];
        int clen = residc_encode(enc, &q, buf, sizeof(buf));
        if (clen < 0) { errors++; continue; }

        total_raw += residc_raw_size(&quote_schema);
        total_compressed += clen;

        /* Decode */
        Quote decoded;
        int dlen = residc_decode(dec, buf, clen, &decoded);
        if (dlen < 0) { errors++; continue; }

        /* Verify roundtrip */
        if (decoded.timestamp != q.timestamp ||
            decoded.instrument_id != q.instrument_id ||
            decoded.bid_price != q.bid_price ||
            decoded.ask_price != q.ask_price ||
            decoded.bid_size != q.bid_size ||
            decoded.ask_size != q.ask_size ||
            decoded.condition != q.condition) {
            errors++;
            if (errors <= 3) {
                printf("MISMATCH at msg %d:\n", i);
                printf("  timestamp: %lu vs %lu\n", decoded.timestamp, q.timestamp);
                printf("  instrument: %u vs %u\n", decoded.instrument_id, q.instrument_id);
                printf("  bid_price: %u vs %u\n", decoded.bid_price, q.bid_price);
                printf("  ask_price: %u vs %u\n", decoded.ask_price, q.ask_price);
                printf("  bid_size: %u vs %u\n", decoded.bid_size, q.bid_size);
                printf("  ask_size: %u vs %u\n", decoded.ask_size, q.ask_size);
                printf("  condition: %u vs %u\n", decoded.condition, q.condition);
            }
        }
    }

    /* Report */
    double ratio = (double)total_raw / total_compressed;
    printf("Results:\n");
    printf("  Raw total:        %ld bytes (%.1f bytes/msg)\n",
           total_raw, (double)total_raw / N);
    printf("  Compressed total: %ld bytes (%.1f bytes/msg)\n",
           total_compressed, (double)total_compressed / N);
    printf("  Compression ratio: %.2f:1\n", ratio);
    printf("  Roundtrip errors: %d\n", errors);

    if (errors == 0)
        printf("\n  All %d messages verified. Zero roundtrip errors.\n", N);
    else
        printf("\n  WARNING: %d roundtrip errors!\n", errors);

    free(enc);
    free(dec);
    return errors > 0 ? 1 : 0;
}
