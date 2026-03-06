/*
 * experiment_golomb.c — Golomb-Rice and Exp-Golomb vs Tiered residual coding
 *
 * Compares three residual coding strategies:
 *   1. Current tiered prefix coding (5 discrete tiers)
 *   2. Golomb-Rice (unary quotient + k-bit remainder)
 *   3. Exponential-Golomb (H.264-style, order-k)
 *
 * Tests on 4 message types: Quote, Trade, Order, BookUpdate
 * Measures compression ratio and encode/decode latency for each.
 *
 * Build:
 *   cc -O2 -march=native -o experiment_golomb bench/experiment_golomb.c core/residc.c -Icore
 *
 * Run:
 *   ./experiment_golomb
 */
#define _POSIX_C_SOURCE 199309L
#include "residc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define N_MSGS  100000
#define N_ITERS 10

/* ================================================================
 * Simple LCG (same as bench_compression.c for reproducibility)
 * ================================================================ */

static uint64_t rng_state = 12345678901ULL;

static uint32_t rng_next(void) {
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(rng_state >> 32);
}

static uint32_t rng_range(uint32_t lo, uint32_t hi) {
    return lo + rng_next() % (hi - lo + 1);
}

/* ================================================================
 * Message structs + schemas (identical to bench_compression.c)
 * ================================================================ */

typedef struct {
    uint64_t timestamp;
    uint16_t instrument_id;
    uint32_t price;
    uint32_t quantity;
    uint8_t  side;
} Quote;

static const residc_field_t quote_fields[] = {
    { RESIDC_TIMESTAMP,  offsetof(Quote, timestamp),     8, -1 },
    { RESIDC_INSTRUMENT, offsetof(Quote, instrument_id), 2, -1 },
    { RESIDC_PRICE,      offsetof(Quote, price),         4, -1 },
    { RESIDC_QUANTITY,   offsetof(Quote, quantity),       4, -1 },
    { RESIDC_BOOL,       offsetof(Quote, side),           1, -1 },
};

static const residc_schema_t quote_schema = {
    .fields = quote_fields, .num_fields = 5, .msg_size = sizeof(Quote),
};

typedef struct {
    uint64_t timestamp;
    uint16_t instrument_id;
    uint32_t price;
    uint32_t quantity;
    uint64_t trade_id;
    uint32_t buyer_id;
    uint32_t seller_id;
    uint8_t  aggressor_side;
} Trade;

static const residc_field_t trade_fields[] = {
    { RESIDC_TIMESTAMP,     offsetof(Trade, timestamp),       8, -1 },
    { RESIDC_INSTRUMENT,    offsetof(Trade, instrument_id),   2, -1 },
    { RESIDC_PRICE,         offsetof(Trade, price),           4, -1 },
    { RESIDC_QUANTITY,      offsetof(Trade, quantity),         4, -1 },
    { RESIDC_SEQUENTIAL_ID, offsetof(Trade, trade_id),        8, -1 },
    { RESIDC_CATEGORICAL,   offsetof(Trade, buyer_id),        4, -1 },
    { RESIDC_CATEGORICAL,   offsetof(Trade, seller_id),       4, -1 },
    { RESIDC_BOOL,          offsetof(Trade, aggressor_side),  1, -1 },
};

static const residc_schema_t trade_schema = {
    .fields = trade_fields, .num_fields = 8, .msg_size = sizeof(Trade),
};

typedef struct {
    uint64_t timestamp;
    uint16_t instrument_id;
    uint32_t price;
    uint32_t quantity;
    uint64_t order_id;
    uint32_t account_id;
    uint8_t  side;
    uint8_t  order_type;
    uint8_t  time_in_force;
    uint8_t  flags;
} Order;

static const residc_field_t order_fields[] = {
    { RESIDC_TIMESTAMP,     offsetof(Order, timestamp),      8, -1 },
    { RESIDC_INSTRUMENT,    offsetof(Order, instrument_id),  2, -1 },
    { RESIDC_PRICE,         offsetof(Order, price),          4, -1 },
    { RESIDC_QUANTITY,      offsetof(Order, quantity),        4, -1 },
    { RESIDC_SEQUENTIAL_ID, offsetof(Order, order_id),       8, -1 },
    { RESIDC_CATEGORICAL,   offsetof(Order, account_id),     4, -1 },
    { RESIDC_BOOL,          offsetof(Order, side),           1, -1 },
    { RESIDC_ENUM,          offsetof(Order, order_type),     1, -1 },
    { RESIDC_ENUM,          offsetof(Order, time_in_force),  1, -1 },
    { RESIDC_ENUM,          offsetof(Order, flags),          1, -1 },
};

static const residc_schema_t order_schema = {
    .fields = order_fields, .num_fields = 10, .msg_size = sizeof(Order),
};

typedef struct {
    uint64_t timestamp;
    uint16_t instrument_id;
    uint32_t price;
    uint32_t quantity;
    uint8_t  side;
    uint8_t  action;
    uint8_t  level;
} BookUpdate;

static const residc_field_t book_fields[] = {
    { RESIDC_TIMESTAMP,  offsetof(BookUpdate, timestamp),     8, -1 },
    { RESIDC_INSTRUMENT, offsetof(BookUpdate, instrument_id), 2, -1 },
    { RESIDC_PRICE,      offsetof(BookUpdate, price),         4, -1 },
    { RESIDC_QUANTITY,   offsetof(BookUpdate, quantity),       4, -1 },
    { RESIDC_BOOL,       offsetof(BookUpdate, side),          1, -1 },
    { RESIDC_ENUM,       offsetof(BookUpdate, action),        1, -1 },
    { RESIDC_ENUM,       offsetof(BookUpdate, level),         1, -1 },
};

static const residc_schema_t book_schema = {
    .fields = book_fields, .num_fields = 7, .msg_size = sizeof(BookUpdate),
};

/* ================================================================
 * Data generators (identical to bench_compression.c)
 * ================================================================ */

static void gen_quotes(Quote *msgs, int n) {
    memset(msgs, 0, n * sizeof(Quote));
    uint64_t ts = 34200000000000ULL;
    for (int i = 0; i < n; i++) {
        ts += 500 + rng_range(0, 50000);
        msgs[i].timestamp = ts;
        msgs[i].instrument_id = (uint16_t)rng_range(0, 49);
        msgs[i].price = 1500000 + (uint32_t)(rng_range(0, 2000)) - 1000;
        msgs[i].quantity = (uint32_t)(rng_range(1, 20) * 100);
        msgs[i].side = (uint8_t)(rng_next() & 1);
    }
}

static void gen_trades(Trade *msgs, int n) {
    memset(msgs, 0, n * sizeof(Trade));
    uint64_t ts = 34200000000000ULL;
    uint64_t tid = 1000000;
    uint32_t firms[] = {
        1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010,
        2001, 2002, 2003, 2004, 2005, 3001, 3002, 3003, 4001, 5001
    };
    for (int i = 0; i < n; i++) {
        ts += 2000 + rng_range(0, 200000);
        tid += 1 + rng_range(0, 3);
        msgs[i].timestamp = ts;
        msgs[i].instrument_id = (uint16_t)rng_range(0, 49);
        msgs[i].price = 1500000 + (uint32_t)(rng_range(0, 2000)) - 1000;
        msgs[i].quantity = (uint32_t)(rng_range(1, 50) * 100);
        msgs[i].trade_id = tid;
        uint32_t r = rng_range(0, 99);
        int buyer_idx  = (r < 60) ? (int)rng_range(0, 4) : (int)rng_range(5, 19);
        r = rng_range(0, 99);
        int seller_idx = (r < 60) ? (int)rng_range(0, 4) : (int)rng_range(5, 19);
        msgs[i].buyer_id = firms[buyer_idx];
        msgs[i].seller_id = firms[seller_idx];
        msgs[i].aggressor_side = (uint8_t)(rng_next() & 1);
    }
}

static void gen_orders(Order *msgs, int n) {
    memset(msgs, 0, n * sizeof(Order));
    uint64_t ts = 34200000000000ULL;
    uint64_t oid = 5000000;
    uint32_t accounts[] = { 100, 101, 102, 103, 104, 200, 201, 300, 400, 500 };
    for (int i = 0; i < n; i++) {
        ts += 1000 + rng_range(0, 100000);
        oid += 1;
        msgs[i].timestamp = ts;
        msgs[i].instrument_id = (uint16_t)rng_range(0, 49);
        msgs[i].price = 1500000 + (uint32_t)(rng_range(0, 4000)) - 2000;
        msgs[i].quantity = (uint32_t)(rng_range(1, 100) * 100);
        msgs[i].order_id = oid;
        uint32_t r = rng_range(0, 99);
        int idx = (r < 70) ? (int)rng_range(0, 2) : (int)rng_range(3, 9);
        msgs[i].account_id = accounts[idx];
        msgs[i].side = (uint8_t)(rng_next() & 1);
        r = rng_range(0, 99);
        msgs[i].order_type = (r < 80) ? 0 : (r < 95) ? 1 : (uint8_t)rng_range(2, 3);
        r = rng_range(0, 99);
        msgs[i].time_in_force = (r < 60) ? 0 : (r < 85) ? 1 : (r < 95) ? 2 : 3;
        msgs[i].flags = 0;
    }
}

static void gen_book_updates(BookUpdate *msgs, int n) {
    memset(msgs, 0, n * sizeof(BookUpdate));
    uint64_t ts = 34200000000000ULL;
    for (int i = 0; i < n; i++) {
        ts += 100 + rng_range(0, 10000);
        msgs[i].timestamp = ts;
        msgs[i].instrument_id = (uint16_t)rng_range(0, 49);
        msgs[i].price = 1500000 + (uint32_t)(rng_range(0, 1000)) - 500;
        msgs[i].quantity = (uint32_t)(rng_range(0, 200) * 100);
        msgs[i].side = (uint8_t)(rng_next() & 1);
        uint32_t r = rng_range(0, 99);
        msgs[i].action = (r < 30) ? 0 : (r < 80) ? 1 : 2;
        msgs[i].level = (uint8_t)rng_range(0, 9);
    }
}

/* ================================================================
 * Golomb-Rice coding
 *
 * value v with parameter k (m = 2^k):
 *   q = v >> k (quotient, encoded in unary: q ones + 1 zero)
 *   r = v & ((1 << k) - 1) (remainder, encoded in k bits)
 *
 * Escape: if q > 31, write 32 ones + 0 + raw 64-bit value.
 * ================================================================ */

static inline void golomb_rice_encode(residc_bitwriter_t *bw,
                                       int64_t value, int k)
{
    uint64_t zz = residc_zigzag_enc(value);
    uint64_t q = zz >> k;

    if (q > 31) {
        /* Escape: 32 ones + 0 + raw 64 bits */
        residc_bw_write(bw, 0xFFFFFFFF, 32);
        residc_bw_write(bw, 0, 1);
        residc_bw_write(bw, zz >> 32, 32);
        residc_bw_write(bw, zz & 0xFFFFFFFF, 32);
        return;
    }

    /* Unary: q ones then one zero */
    if (q > 0)
        residc_bw_write(bw, (1ULL << q) - 1, (int)q);
    residc_bw_write(bw, 0, 1);

    /* Binary: k-bit remainder */
    if (k > 0)
        residc_bw_write(bw, zz & ((1ULL << k) - 1), k);
}

static inline int64_t golomb_rice_decode(residc_bitreader_t *br, int k)
{
    /* Read unary: count ones until we see a zero */
    uint64_t q = 0;
    while (residc_br_read_bit(br) == 1) {
        q++;
        if (q == 32) {
            /* Escape: consume the 0 terminator, then read raw 64-bit */
            residc_br_read_bit(br);  /* consume the trailing 0 */
            uint64_t hi = residc_br_read(br, 32);
            uint64_t lo = residc_br_read(br, 32);
            return residc_zigzag_dec((hi << 32) | lo);
        }
    }

    /* Read k-bit remainder */
    uint64_t r = 0;
    if (k > 0)
        r = residc_br_read(br, k);

    uint64_t zz = (q << k) | r;
    return residc_zigzag_dec(zz);
}

/* ================================================================
 * Exponential-Golomb coding (order k)
 *
 * For order-k exp-golomb, encode zz as:
 *   adjusted = zz + (1 << k)   [always >= 2^k]
 *   n = floor(log2(adjusted))  [bit length - 1]
 *   prefix_len = n - k         [unary ones before the zero]
 *   suffix = adjusted & ((1 << n) - 1)  [n lower bits]
 *
 * Total bits = prefix_len + 1 + n = 2*(n-k) + 1 + k
 * ================================================================ */

static inline int ilog2_64(uint64_t v)
{
    return 63 - __builtin_clzll(v);
}

static inline void exp_golomb_encode(residc_bitwriter_t *bw,
                                      int64_t value, int k)
{
    uint64_t zz = residc_zigzag_enc(value);
    uint64_t adjusted = zz + (1ULL << k);

    int n = ilog2_64(adjusted);
    int prefix_len = n - k;

    if (prefix_len > 31) {
        /* Escape: 32 ones + 0 + raw 64 bits */
        residc_bw_write(bw, 0xFFFFFFFF, 32);
        residc_bw_write(bw, 0, 1);
        residc_bw_write(bw, zz >> 32, 32);
        residc_bw_write(bw, zz & 0xFFFFFFFF, 32);
        return;
    }

    /* Unary prefix: prefix_len ones + 1 zero */
    if (prefix_len > 0)
        residc_bw_write(bw, (1ULL << prefix_len) - 1, prefix_len);
    residc_bw_write(bw, 0, 1);

    /* Suffix: n lower bits of adjusted */
    if (n > 0) {
        uint64_t suffix = adjusted & ((1ULL << n) - 1);
        if (n > 32) {
            residc_bw_write(bw, suffix >> 32, n - 32);
            residc_bw_write(bw, suffix & 0xFFFFFFFF, 32);
        } else {
            residc_bw_write(bw, suffix, n);
        }
    }
}

static inline int64_t exp_golomb_decode(residc_bitreader_t *br, int k)
{
    int prefix_len = 0;
    while (residc_br_read_bit(br) == 1) {
        prefix_len++;
        if (prefix_len == 32) {
            /* Escape: consume the 0 terminator, then read raw 64-bit */
            residc_br_read_bit(br);
            uint64_t hi = residc_br_read(br, 32);
            uint64_t lo = residc_br_read(br, 32);
            return residc_zigzag_dec((hi << 32) | lo);
        }
    }

    int n = prefix_len + k;
    uint64_t suffix = 0;
    if (n > 0) {
        if (n > 32) {
            uint64_t hi = residc_br_read(br, n - 32);
            uint64_t lo = residc_br_read(br, 32);
            suffix = (hi << 32) | lo;
        } else {
            suffix = residc_br_read(br, n);
        }
    }

    uint64_t adjusted = (1ULL << n) | suffix;
    uint64_t zz = adjusted - (1ULL << k);
    return residc_zigzag_dec(zz);
}

/* ================================================================
 * Coder types
 * ================================================================ */

typedef enum {
    CODER_TIERED,
    CODER_GOLOMB_RICE,
    CODER_EXP_GOLOMB,
} coder_type_t;

static inline void encode_one(residc_bitwriter_t *bw, int64_t value,
                               int k, coder_type_t coder)
{
    switch (coder) {
    case CODER_TIERED:      residc_encode_residual(bw, value, k); break;
    case CODER_GOLOMB_RICE: golomb_rice_encode(bw, value, k);     break;
    case CODER_EXP_GOLOMB:  exp_golomb_encode(bw, value, k);      break;
    }
}

static inline int64_t decode_one(residc_bitreader_t *br, int k,
                                  coder_type_t coder)
{
    switch (coder) {
    case CODER_TIERED:      return residc_decode_residual(br, k);
    case CODER_GOLOMB_RICE: return golomb_rice_decode(br, k);
    case CODER_EXP_GOLOMB:  return exp_golomb_decode(br, k);
    }
    return 0;
}

/* ================================================================
 * Encode N residuals, measuring total bits.
 *
 * Encodes in batches that fit in the 256-byte scratch buffer
 * to avoid overflow. Each batch: init writer, encode up to BATCH
 * values, finish, accumulate bytes.
 * ================================================================ */

#define BATCH_SIZE 16  /* conservative: worst case ~16*68 bits = 136 bytes */

static long encode_residuals_total_bits(const int64_t *residuals, int n,
                                         int k, coder_type_t coder)
{
    long total_bytes = 0;
    for (int i = 0; i < n; i += BATCH_SIZE) {
        int batch = (n - i < BATCH_SIZE) ? (n - i) : BATCH_SIZE;
        residc_bitwriter_t bw;
        residc_bw_init(&bw);
        for (int j = 0; j < batch; j++)
            encode_one(&bw, residuals[i + j], k, coder);
        total_bytes += residc_bw_finish(&bw);
    }
    return total_bytes * 8;
}

/* Encode + decode, return number of errors */
static int roundtrip_residuals(const int64_t *residuals, int n, int k,
                                coder_type_t coder)
{
    int errors = 0;
    for (int i = 0; i < n; i += BATCH_SIZE) {
        int batch = (n - i < BATCH_SIZE) ? (n - i) : BATCH_SIZE;
        residc_bitwriter_t bw;
        residc_bw_init(&bw);
        for (int j = 0; j < batch; j++)
            encode_one(&bw, residuals[i + j], k, coder);
        int len = residc_bw_finish(&bw);

        residc_bitreader_t br;
        residc_br_init(&br, bw.buf, len);
        for (int j = 0; j < batch; j++) {
            int64_t decoded = decode_one(&br, k, coder);
            if (decoded != residuals[i + j]) errors++;
        }
    }
    return errors;
}

/* ================================================================
 * Benchmark helpers
 * ================================================================ */

typedef struct {
    const char *name;
    int raw_bytes;
    long compressed_bytes;
    double avg_compressed;
    double ratio;
    double enc_ns;
    double dec_ns;
    int errors;
} codec_result_t;

/* Benchmark the standard tiered codec (baseline) */
static void bench_tiered_codec(const residc_schema_t *schema, const void *msgs,
                                int n, codec_result_t *r)
{
    int raw_size = residc_raw_size(schema);
    int msg_size = schema->msg_size;
    /* Heap-allocate: residc_state_t is ~332KB each */
    residc_state_t *enc = malloc(sizeof(residc_state_t));
    residc_state_t *dec = malloc(sizeof(residc_state_t));
    uint8_t buf[128];

    int *lens = malloc(n * sizeof(int));
    uint8_t (*bufs)[128] = malloc(n * 128);
    residc_init(enc, schema);
    long total = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
        lens[i] = residc_encode(enc, msg, bufs[i], 128);
        total += lens[i];
    }

    double best_enc = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(enc, schema);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < n; i++) {
            const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
            residc_encode(enc, msg, buf, 128);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        if (ns / n < best_enc) best_enc = ns / n;
    }

    double best_dec = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(dec, schema);
        uint8_t decoded[256];
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < n; i++)
            residc_decode(dec, bufs[i], lens[i], decoded);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        if (ns / n < best_dec) best_dec = ns / n;
    }

    /* Roundtrip verify */
    residc_init(enc, schema);
    residc_init(dec, schema);
    int errors = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
        int len = residc_encode(enc, msg, buf, 128);
        uint8_t decoded[256];
        memset(decoded, 0, sizeof(decoded));
        residc_decode(dec, buf, len, decoded);
        if (memcmp(msg, decoded, msg_size) != 0) errors++;
    }

    r->name = "Tiered (full codec)";
    r->raw_bytes = raw_size;
    r->compressed_bytes = total;
    r->avg_compressed = (double)total / n;
    r->ratio = (double)((long)n * raw_size) / total;
    r->enc_ns = best_enc;
    r->dec_ns = best_dec;
    r->errors = errors;

    free(lens);
    free(bufs);
    free(enc);
    free(dec);
}

/* ================================================================
 * Generate realistic residual distributions
 * ================================================================ */

static void gen_residual_distribution(int64_t *out, int n, const char *type) {
    if (strcmp(type, "timestamp") == 0) {
        /* Timestamp gap residuals: mostly small, some outliers */
        for (int i = 0; i < n; i++) {
            uint32_t x = rng_range(0, 99);
            if (x < 50) out[i] = (int64_t)rng_range(0, 6) - 3;
            else if (x < 80) out[i] = (int64_t)rng_range(0, 100) - 50;
            else if (x < 95) out[i] = (int64_t)rng_range(0, 2000) - 1000;
            else out[i] = (int64_t)rng_range(0, 50000) - 25000;
        }
    } else if (strcmp(type, "price") == 0) {
        /* Price residuals: heavily concentrated near 0 */
        for (int i = 0; i < n; i++) {
            uint32_t x = rng_range(0, 99);
            if (x < 40) out[i] = 0;
            else if (x < 70) out[i] = (int64_t)rng_range(0, 2) - 1;
            else if (x < 90) out[i] = (int64_t)rng_range(0, 20) - 10;
            else out[i] = (int64_t)rng_range(0, 200) - 100;
        }
    } else if (strcmp(type, "quantity") == 0) {
        /* Quantity residuals: often 0, sometimes moderate */
        for (int i = 0; i < n; i++) {
            uint32_t x = rng_range(0, 99);
            if (x < 50) out[i] = 0;
            else if (x < 80) out[i] = (int64_t)rng_range(0, 10) - 5;
            else out[i] = (int64_t)rng_range(0, 100) - 50;
        }
    } else { /* seqid */
        /* Sequential ID deltas: small positive */
        for (int i = 0; i < n; i++) {
            uint32_t x = rng_range(0, 99);
            if (x < 60) out[i] = 0;
            else if (x < 85) out[i] = (int64_t)rng_range(0, 4);
            else out[i] = (int64_t)rng_range(0, 30);
        }
    }
}

/* ================================================================
 * Isolated residual coding benchmark
 * ================================================================ */

typedef struct {
    const char *coder_name;
    const char *field_type;
    int k;
    long total_bits;
    double bits_per_value;
    double enc_ns;
    double dec_ns;
    int errors;
} residual_result_t;

static void bench_residual_coder(const int64_t *residuals, int n,
                                  int k, coder_type_t coder,
                                  const char *coder_name,
                                  const char *field_type,
                                  residual_result_t *r)
{
    /* Measure size */
    long total_bits = encode_residuals_total_bits(residuals, n, k, coder);

    /* Encode timing: encode all residuals in batches */
    double best_enc = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < n; i += BATCH_SIZE) {
            int batch = (n - i < BATCH_SIZE) ? (n - i) : BATCH_SIZE;
            residc_bitwriter_t bw;
            residc_bw_init(&bw);
            for (int j = 0; j < batch; j++)
                encode_one(&bw, residuals[i + j], k, coder);
            residc_bw_finish(&bw);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        if (ns / n < best_enc) best_enc = ns / n;
    }

    /* Decode timing: pre-encode batches, then decode */
    /* Pre-encode all batches */
    int n_batches = (n + BATCH_SIZE - 1) / BATCH_SIZE;
    uint8_t (*batch_bufs)[RESIDC_SCRATCH_BYTES] = malloc(n_batches * RESIDC_SCRATCH_BYTES);
    int *batch_lens = malloc(n_batches * sizeof(int));
    for (int b = 0; b < n_batches; b++) {
        int start = b * BATCH_SIZE;
        int batch = (n - start < BATCH_SIZE) ? (n - start) : BATCH_SIZE;
        residc_bitwriter_t bw;
        residc_bw_init(&bw);
        for (int j = 0; j < batch; j++)
            encode_one(&bw, residuals[start + j], k, coder);
        batch_lens[b] = residc_bw_finish(&bw);
        memcpy(batch_bufs[b], bw.buf, batch_lens[b]);
    }

    double best_dec = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int b = 0; b < n_batches; b++) {
            int start = b * BATCH_SIZE;
            int batch = (n - start < BATCH_SIZE) ? (n - start) : BATCH_SIZE;
            residc_bitreader_t br;
            residc_br_init(&br, batch_bufs[b], batch_lens[b]);
            for (int j = 0; j < batch; j++)
                decode_one(&br, k, coder);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        if (ns / n < best_dec) best_dec = ns / n;
    }

    /* Verify roundtrip */
    int errors = roundtrip_residuals(residuals, n, k, coder);

    r->coder_name = coder_name;
    r->field_type = field_type;
    r->k = k;
    r->total_bits = total_bits;
    r->bits_per_value = (double)total_bits / n;
    r->enc_ns = best_enc;
    r->dec_ns = best_dec;
    r->errors = errors;

    free(batch_bufs);
    free(batch_lens);
}

/* ================================================================
 * K-sweep
 * ================================================================ */

typedef struct {
    int k;
    double bits_per_value;
} k_sweep_point_t;

static void k_sweep(const int64_t *residuals, int n, coder_type_t coder,
                    k_sweep_point_t *points, int max_k)
{
    for (int k = 0; k <= max_k; k++) {
        long bits = encode_residuals_total_bits(residuals, n, k, coder);
        points[k].k = k;
        points[k].bits_per_value = (double)bits / n;
    }
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void)
{
    printf("================================================================\n");
    printf("Golomb-Rice & Exp-Golomb vs Tiered Residual Coding Experiment\n");
    printf("================================================================\n\n");

    /* ---- Part 1: Full-codec baseline ---- */
    printf("PART 1: Full-codec baseline (Tiered, %d messages)\n", N_MSGS);
    printf("----------------------------------------------------------------\n");

    rng_state = 12345678901ULL;
    Quote *quotes = malloc(N_MSGS * sizeof(Quote));
    gen_quotes(quotes, N_MSGS);

    rng_state = 12345678901ULL;
    Trade *trades = malloc(N_MSGS * sizeof(Trade));
    gen_trades(trades, N_MSGS);

    rng_state = 12345678901ULL;
    Order *orders = malloc(N_MSGS * sizeof(Order));
    gen_orders(orders, N_MSGS);

    rng_state = 12345678901ULL;
    BookUpdate *books = malloc(N_MSGS * sizeof(BookUpdate));
    gen_book_updates(books, N_MSGS);

    codec_result_t baseline[4];
    bench_tiered_codec(&quote_schema, quotes, N_MSGS, &baseline[0]);
    bench_tiered_codec(&trade_schema, trades, N_MSGS, &baseline[1]);
    bench_tiered_codec(&order_schema, orders, N_MSGS, &baseline[2]);
    bench_tiered_codec(&book_schema, books, N_MSGS, &baseline[3]);

    const char *msg_names[] = {"Quote", "Trade", "Order", "BookUpdate"};
    printf("%-12s  %5s  %7s  %7s  %7s  %7s  %s\n",
           "Message", "Raw", "Avg", "Ratio", "Enc", "Dec", "");
    printf("%-12s  %5s  %7s  %7s  %7s  %7s\n",
           "------------", "-----", "-------", "-------", "-------", "-------");
    int total_baseline_errors = 0;
    for (int i = 0; i < 4; i++) {
        printf("%-12s  %3d B  %5.1f B  %5.2f:1  %4.0f ns  %4.0f ns%s\n",
               msg_names[i], baseline[i].raw_bytes, baseline[i].avg_compressed,
               baseline[i].ratio, baseline[i].enc_ns, baseline[i].dec_ns,
               baseline[i].errors ? "  [ERRORS]" : "");
        total_baseline_errors += baseline[i].errors;
    }

    /* ---- Part 2: Isolated residual coding comparison ---- */
    printf("\n\nPART 2: Isolated Residual Coding — Default k\n");
    printf("----------------------------------------------------------------\n");
    printf("%d residuals per distribution, best of %d iters.\n\n", N_MSGS, N_ITERS);

    const char *field_types[] = {"timestamp", "price", "quantity", "seqid"};
    int default_k[] = {10, 3, 4, 3};
    int n_fields = 4;

    printf("%-10s  %-12s  %3s  %8s  %7s  %7s  %6s\n",
           "Field", "Coder", "k", "bits/val", "Enc", "Dec", "Errs");
    printf("%-10s  %-12s  %3s  %8s  %7s  %7s  %6s\n",
           "----------", "------------", "---", "--------", "-------", "-------", "------");

    int64_t *residuals = malloc(N_MSGS * sizeof(int64_t));
    int total_errors = 0;

    const char *coder_names[] = {"Tiered", "Golomb-Rice", "Exp-Golomb"};
    coder_type_t coders[] = {CODER_TIERED, CODER_GOLOMB_RICE, CODER_EXP_GOLOMB};

    for (int fi = 0; fi < n_fields; fi++) {
        rng_state = 42 + fi;
        gen_residual_distribution(residuals, N_MSGS, field_types[fi]);

        for (int ci = 0; ci < 3; ci++) {
            residual_result_t r;
            bench_residual_coder(residuals, N_MSGS, default_k[fi],
                                 coders[ci], coder_names[ci],
                                 field_types[fi], &r);
            printf("%-10s  %-12s  %3d  %8.2f  %4.0f ns  %4.0f ns  %4d\n",
                   r.field_type, r.coder_name, r.k,
                   r.bits_per_value, r.enc_ns, r.dec_ns, r.errors);
            total_errors += r.errors;
        }
        printf("\n");
    }

    /* ---- Part 3: K-sweep ---- */
    printf("\nPART 3: K-Sweep — bits/value for k=0..12\n");
    printf("----------------------------------------------------------------\n\n");

    int max_k = 12;
    k_sweep_point_t *sweep = malloc((max_k + 1) * sizeof(k_sweep_point_t));

    for (int fi = 0; fi < n_fields; fi++) {
        rng_state = 42 + fi;
        gen_residual_distribution(residuals, N_MSGS, field_types[fi]);

        printf("  Distribution: %s\n", field_types[fi]);
        printf("  %-12s  ", "k ->");
        for (int k = 0; k <= max_k; k++) printf("%6d", k);
        printf("  best\n");

        for (int ci = 0; ci < 3; ci++) {
            k_sweep(residuals, N_MSGS, coders[ci], sweep, max_k);

            int best_k = 0;
            double best_bpv = 999.0;
            for (int k = 0; k <= max_k; k++) {
                if (sweep[k].bits_per_value < best_bpv) {
                    best_bpv = sweep[k].bits_per_value;
                    best_k = k;
                }
            }

            printf("  %-12s  ", coder_names[ci]);
            for (int k = 0; k <= max_k; k++) {
                if (k == best_k)
                    printf("%5.1f*", sweep[k].bits_per_value);
                else
                    printf("%6.1f", sweep[k].bits_per_value);
            }
            printf("  k=%d (%.2f)\n", best_k, best_bpv);
        }
        printf("\n");
    }

    /* ---- Part 4: Latency at optimal k ---- */
    printf("\nPART 4: Latency at Optimal k\n");
    printf("----------------------------------------------------------------\n");
    printf("%-10s  %-12s  %3s  %8s  %7s  %7s\n",
           "Field", "Coder", "k*", "bits/val", "Enc", "Dec");
    printf("%-10s  %-12s  %3s  %8s  %7s  %7s\n",
           "----------", "------------", "---", "--------", "-------", "-------");

    for (int fi = 0; fi < n_fields; fi++) {
        rng_state = 42 + fi;
        gen_residual_distribution(residuals, N_MSGS, field_types[fi]);

        for (int ci = 0; ci < 3; ci++) {
            k_sweep(residuals, N_MSGS, coders[ci], sweep, max_k);
            int best_k = 0;
            double best_bpv = 999.0;
            for (int k = 0; k <= max_k; k++) {
                if (sweep[k].bits_per_value < best_bpv) {
                    best_bpv = sweep[k].bits_per_value;
                    best_k = k;
                }
            }

            residual_result_t r;
            bench_residual_coder(residuals, N_MSGS, best_k,
                                 coders[ci], coder_names[ci],
                                 field_types[fi], &r);
            printf("%-10s  %-12s  %3d  %8.2f  %4.0f ns  %4.0f ns\n",
                   r.field_type, r.coder_name, best_k,
                   r.bits_per_value, r.enc_ns, r.dec_ns);
            total_errors += r.errors;
        }
        printf("\n");
    }

    /* ---- Summary ---- */
    printf("\nSUMMARY\n");
    printf("================================================================\n");
    printf("Full-codec baseline roundtrip errors: %d\n", total_baseline_errors);
    printf("Isolated residual coder errors: %d\n", total_errors);
    printf("\nAnalysis:\n");
    printf("- Tiered: 5 discrete tiers, branch-based decode, O(1) per tier\n");
    printf("- Golomb-Rice: smooth coding, unary loop in decode (data-dependent)\n");
    printf("- Exp-Golomb: H.264-style, adapts suffix to magnitude\n");
    printf("- For peaked distributions (many zeros), low k favors all coders\n");
    printf("- For spread distributions, higher k reduces unary prefix overhead\n");
    printf("- Tiered's fixed boundaries waste bits at tier edges\n");
    printf("- Golomb-Rice is near-optimal for geometric distributions\n");
    printf("================================================================\n");

    free(quotes);
    free(trades);
    free(orders);
    free(books);
    free(residuals);
    free(sweep);

    return total_baseline_errors + total_errors;
}
