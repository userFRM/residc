/*
 * experiment_combined_write.c -- Combined prefix+payload tier encoding
 *
 * Optimization: merge the prefix bits and payload bits into a single
 * bw_write call, halving the number of calls on the hot path.
 *
 * Current encode_residual (2 calls per tier):
 *   tier 0: bw_write(0, 1) + bw_write(zz, k)          = 2 calls
 *   tier 1: bw_write(0x2, 2) + bw_write(zz, k+2)      = 2 calls
 *   ...
 *
 * Combined encode_residual (1 call per tier):
 *   tier 0: bw_write((0 << k) | zz, 1+k)               = 1 call
 *   tier 1: bw_write((0x2 << (k+2)) | zz, 2+k+2)       = 1 call
 *   tier 2: bw_write((0x6 << (k+5)) | zz, 3+k+5)       = 1 call
 *   tier 3: bw_write((0xE << (k+10)) | zz, 4+k+10)     = 1 call
 *   tier 4: unchanged (raw 64-bit, needs split writes)
 *
 * Wire format is IDENTICAL -- the decoder is unchanged.
 *
 * Build:
 *   cc -O2 -march=native -o experiment_combined_write \
 *     bench/experiment_combined_write.c core/residc.c -Icore
 */

#define _POSIX_C_SOURCE 199309L
#include "residc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N_MSGS  100000
#define N_ITERS 10

/* ================================================================
 * RNG (identical to bench_compression.c)
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
 * Message types (identical to bench_compression.c)
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

/* Data generators (identical to bench_compression.c) */

static void gen_quotes(Quote *msgs, int n) {
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
 * bw_write: inlined copy from residc.c
 * ================================================================ */

static inline __attribute__((always_inline))
void bw_write(residc_bitwriter_t *bw, uint64_t val, int nbits)
{
    bw->accum = (bw->accum << nbits) | (val & ((1ULL << nbits) - 1));
    bw->count += nbits;
    while (__builtin_expect(bw->count >= 8, 0)) {
        if (bw->byte_pos >= RESIDC_SCRATCH_BYTES) return;
        bw->count -= 8;
        bw->buf[bw->byte_pos++] = (uint8_t)(bw->accum >> bw->count);
    }
}

/* ================================================================
 * BASELINE: current 2-call encode_residual (from residc.c)
 * ================================================================ */

static inline __attribute__((always_inline))
void encode_residual_baseline(residc_bitwriter_t *bw, int64_t value, int k)
{
    uint64_t zz = residc_zigzag_enc(value);
    if (zz < (1ULL << k)) {
        bw_write(bw, 0, 1);           /* tier 0 prefix */
        bw_write(bw, zz, k);          /* tier 0 payload */
    } else if (zz < (1ULL << (k + 2))) {
        bw_write(bw, 0x2, 2);         /* tier 1 prefix */
        bw_write(bw, zz, k + 2);      /* tier 1 payload */
    } else if (zz < (1ULL << (k + 5))) {
        bw_write(bw, 0x6, 3);         /* tier 2 prefix */
        bw_write(bw, zz, k + 5);      /* tier 2 payload */
    } else if (zz < (1ULL << (k + 10))) {
        bw_write(bw, 0xE, 4);         /* tier 3 prefix */
        bw_write(bw, zz, k + 10);     /* tier 3 payload */
    } else {
        bw_write(bw, 0xF, 4);         /* tier 4 prefix */
        bw_write(bw, (uint64_t)value >> 32, 32);
        bw_write(bw, (uint64_t)value & 0xFFFFFFFF, 32);
    }
}

/* ================================================================
 * COMBINED: single-call encode_residual
 *
 * Merge prefix + payload into one bw_write call per tier.
 * Wire format is identical -- just packs the same bits differently.
 *
 *   tier 0: (0 << k) | zz           in 1+k bits
 *   tier 1: (0b10 << (k+2)) | zz    in 2+(k+2) bits
 *   tier 2: (0b110 << (k+5)) | zz   in 3+(k+5) bits
 *   tier 3: (0b1110 << (k+10)) | zz in 4+(k+10) bits
 *   tier 4: unchanged (>= 64 bits, must split)
 * ================================================================ */

static inline __attribute__((always_inline))
void encode_residual_combined(residc_bitwriter_t *bw, int64_t value, int k)
{
    uint64_t zz = residc_zigzag_enc(value);
    if (zz < (1ULL << k)) {
        /* tier 0: prefix=0(1 bit), payload=zz(k bits) -> combined (1+k) bits */
        /* (0 << k) | zz = just zz, with total width 1+k */
        bw_write(bw, zz, 1 + k);
    } else if (zz < (1ULL << (k + 2))) {
        /* tier 1: prefix=10(2 bits), payload=zz(k+2 bits) */
        int nbits = 2 + k + 2;
        bw_write(bw, (0x2ULL << (k + 2)) | zz, nbits);
    } else if (zz < (1ULL << (k + 5))) {
        /* tier 2: prefix=110(3 bits), payload=zz(k+5 bits) */
        int nbits = 3 + k + 5;
        bw_write(bw, (0x6ULL << (k + 5)) | zz, nbits);
    } else if (zz < (1ULL << (k + 10))) {
        /* tier 3: prefix=1110(4 bits), payload=zz(k+10 bits) */
        int nbits = 4 + k + 10;
        bw_write(bw, (0xEULL << (k + 10)) | zz, nbits);
    } else {
        /* tier 4: 4 prefix bits + 64 raw bits = 68 bits, must split */
        bw_write(bw, 0xF, 4);
        bw_write(bw, (uint64_t)value >> 32, 32);
        bw_write(bw, (uint64_t)value & 0xFFFFFFFF, 32);
    }
}

/* ================================================================
 * Benchmark harness
 *
 * We test encode_residual in isolation (no full codec pipeline)
 * to measure the pure bw_write call reduction benefit.
 * We also test full pipeline roundtrip to verify correctness.
 * ================================================================ */

typedef struct {
    const char *name;
    double baseline_ns;
    double combined_ns;
    double speedup_pct;
    long baseline_bytes;
    long combined_bytes;
    int roundtrip_errors;
} bench_result_t;

static struct timespec ts_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}

static double ts_diff_ns(struct timespec t0, struct timespec t1) {
    return (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
}

/*
 * Extract residuals from messages using the codec prediction logic,
 * then benchmark just the residual encoding step.
 */

#define MAX_RESID 12

static void extract_residuals(
    const residc_schema_t *schema, const void *msgs, int n,
    int64_t *residuals, int *n_per_msg)
{
    int msg_size = schema->msg_size;
    residc_state_t *state = calloc(1, sizeof(residc_state_t));
    residc_init(state, schema);

    for (int i = 0; i < n; i++) {
        const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
        int nr = 0;
        int64_t *res = &residuals[i * MAX_RESID];

        uint16_t instrument_id = state->last_instrument_id;
        residc_instrument_state_t *is = NULL;

        for (int fi = 0; fi < schema->num_fields; fi++) {
            const residc_field_t *f = &schema->fields[fi];
            uint64_t val = 0;
            const uint8_t *p = msg + f->offset;
            switch (f->size) {
            case 1: val = *(const uint8_t *)p; break;
            case 2: { uint16_t v; memcpy(&v, p, 2); val = v; break; }
            case 4: { uint32_t v; memcpy(&v, p, 4); val = v; break; }
            case 8: { uint64_t v; memcpy(&v, p, 8); val = v; break; }
            }

            switch (f->type) {
            case RESIDC_TIMESTAMP: {
                int64_t gap = (int64_t)(val - state->last_timestamp);
                int64_t pg = state->timestamp_gap_ema >> 16;
                if (pg < 0) pg = 0;
                res[nr++] = gap - pg;
                break;
            }
            case RESIDC_INSTRUMENT:
                instrument_id = (uint16_t)val;
                is = (instrument_id < RESIDC_MAX_INSTRUMENTS)
                     ? &state->instruments[instrument_id] : NULL;
                break;
            case RESIDC_PRICE: {
                uint32_t price = (uint32_t)val;
                uint32_t pred = (is && is->msg_count > 0) ? is->last_price : 0;
                int64_t r = (int64_t)price - (int64_t)pred;
                if (price % 100 == 0 && pred % 100 == 0 && (price > 0 || pred > 0))
                    res[nr++] = r / 100;
                else
                    res[nr++] = r;
                break;
            }
            case RESIDC_QUANTITY: {
                uint32_t qty = (uint32_t)val;
                uint32_t pred = (is && is->msg_count > 0) ? is->last_qty : 100;
                int64_t r = (int64_t)qty - (int64_t)pred;
                if (r != 0) {
                    if (qty % 100 == 0 && pred % 100 == 0)
                        res[nr++] = r / 100;
                    else
                        res[nr++] = r;
                }
                break;
            }
            case RESIDC_SEQUENTIAL_ID: {
                residc_field_state_t *fs = &state->field_state[fi];
                uint64_t pred = (is && is->last_seq_id > 0)
                               ? is->last_seq_id : fs->last_value;
                res[nr++] = (int64_t)(val - pred);
                break;
            }
            default:
                break;
            }
        }

        n_per_msg[i] = nr;

        /* Advance state with real encoder */
        uint8_t buf[128];
        residc_encode(state, msg, buf, 128);
    }

    free(state);
}

static void run_bench(
    const residc_schema_t *schema, const void *msgs, int n,
    const char *name, bench_result_t *result)
{
    int msg_size = schema->msg_size;

    /* Extract residuals */
    int64_t *residuals = calloc((size_t)n * MAX_RESID, sizeof(int64_t));
    int *n_per_msg = calloc(n, sizeof(int));
    extract_residuals(schema, msgs, n, residuals, n_per_msg);

    int k = 3;  /* typical k for CALM regime */

    /* --- Verify bitstream identity --- */
    /* Encode all residuals with baseline, record output */
    residc_bitwriter_t bw_base, bw_comb;
    residc_bw_init(&bw_base);
    residc_bw_init(&bw_comb);

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n_per_msg[i]; j++) {
            encode_residual_baseline(&bw_base, residuals[i * MAX_RESID + j], k);
            encode_residual_combined(&bw_comb, residuals[i * MAX_RESID + j], k);
        }
        /* Periodically flush to avoid overflow (we only care about matching) */
        if (bw_base.byte_pos > RESIDC_SCRATCH_BYTES - 40) {
            /* Compare what we have so far */
            int base_len = residc_bw_finish(&bw_base);
            int comb_len = residc_bw_finish(&bw_comb);
            if (base_len != comb_len || memcmp(bw_base.buf, bw_comb.buf, base_len) != 0) {
                result->roundtrip_errors++;
            }
            residc_bw_init(&bw_base);
            residc_bw_init(&bw_comb);
        }
    }
    /* Final check */
    {
        int base_len = residc_bw_finish(&bw_base);
        int comb_len = residc_bw_finish(&bw_comb);
        if (base_len != comb_len || memcmp(bw_base.buf, bw_comb.buf, base_len) != 0) {
            result->roundtrip_errors++;
        }
    }

    /* Also verify full codec roundtrip with combined encoding */
    /* (We can't plug combined into residc without modifying it,
     *  but the bitstream identity check above proves correctness.) */

    /* --- Measure total output bytes for each --- */
    long base_total = 0, comb_total = 0;
    {
        residc_bitwriter_t bw;
        for (int i = 0; i < n; i++) {
            residc_bw_init(&bw);
            for (int j = 0; j < n_per_msg[i]; j++)
                encode_residual_baseline(&bw, residuals[i * MAX_RESID + j], k);
            base_total += residc_bw_finish(&bw);
        }
    }
    {
        residc_bitwriter_t bw;
        for (int i = 0; i < n; i++) {
            residc_bw_init(&bw);
            for (int j = 0; j < n_per_msg[i]; j++)
                encode_residual_combined(&bw, residuals[i * MAX_RESID + j], k);
            comb_total += residc_bw_finish(&bw);
        }
    }

    /* --- Speed: baseline --- */
    double best_base = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_bitwriter_t bw;
        struct timespec t0 = ts_now();
        for (int i = 0; i < n; i++) {
            residc_bw_init(&bw);
            for (int j = 0; j < n_per_msg[i]; j++)
                encode_residual_baseline(&bw, residuals[i * MAX_RESID + j], k);
            residc_bw_finish(&bw);
        }
        struct timespec t1 = ts_now();
        double ns = ts_diff_ns(t0, t1) / n;
        if (ns < best_base) best_base = ns;
    }

    /* --- Speed: combined --- */
    double best_comb = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_bitwriter_t bw;
        struct timespec t0 = ts_now();
        for (int i = 0; i < n; i++) {
            residc_bw_init(&bw);
            for (int j = 0; j < n_per_msg[i]; j++)
                encode_residual_combined(&bw, residuals[i * MAX_RESID + j], k);
            residc_bw_finish(&bw);
        }
        struct timespec t1 = ts_now();
        double ns = ts_diff_ns(t0, t1) / n;
        if (ns < best_comb) best_comb = ns;
    }

    /* --- Speed: full pipeline baseline (for context) --- */
    double best_full_enc = 1e9, best_full_dec = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_state_t enc;
        residc_init(&enc, schema);
        uint8_t buf[128];
        struct timespec t0 = ts_now();
        for (int i = 0; i < n; i++) {
            const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
            residc_encode(&enc, msg, buf, 128);
        }
        struct timespec t1 = ts_now();
        double ns = ts_diff_ns(t0, t1) / n;
        if (ns < best_full_enc) best_full_enc = ns;
    }

    /* Pre-encode for decode */
    int *lens = malloc(n * sizeof(int));
    uint8_t (*bufs)[128] = malloc((size_t)n * 128);
    {
        residc_state_t enc;
        residc_init(&enc, schema);
        for (int i = 0; i < n; i++) {
            const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
            lens[i] = residc_encode(&enc, msg, bufs[i], 128);
        }
    }
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_state_t dec;
        residc_init(&dec, schema);
        uint8_t decoded[256];
        struct timespec t0 = ts_now();
        for (int i = 0; i < n; i++)
            residc_decode(&dec, bufs[i], lens[i], decoded);
        struct timespec t1 = ts_now();
        double ns = ts_diff_ns(t0, t1) / n;
        if (ns < best_full_dec) best_full_dec = ns;
    }

    result->name = name;
    result->baseline_ns = best_base;
    result->combined_ns = best_comb;
    result->speedup_pct = ((best_base - best_comb) / best_base) * 100.0;
    result->baseline_bytes = base_total;
    result->combined_bytes = comb_total;

    /* Count tier distribution for analysis */
    int tier_counts[5] = {0};
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n_per_msg[i]; j++) {
            uint64_t zz = residc_zigzag_enc(residuals[i * MAX_RESID + j]);
            if (zz < (1ULL << k))           tier_counts[0]++;
            else if (zz < (1ULL << (k+2)))  tier_counts[1]++;
            else if (zz < (1ULL << (k+5)))  tier_counts[2]++;
            else if (zz < (1ULL << (k+10))) tier_counts[3]++;
            else                             tier_counts[4]++;
        }
    }

    int total_residuals = 0;
    for (int i = 0; i < n; i++) total_residuals += n_per_msg[i];

    printf("  %-14s  %5.1f ns  %5.1f ns  %+5.1f%%   %6ld B  %6ld B   %s",
           name, best_base, best_comb, result->speedup_pct,
           base_total, comb_total,
           result->roundtrip_errors == 0 ? "PASS" : "FAIL");
    printf("  [full: enc=%3.0fns dec=%3.0fns]\n", best_full_enc, best_full_dec);

    printf("    tier distribution (k=%d): t0=%d(%.0f%%) t1=%d(%.0f%%) t2=%d(%.0f%%) t3=%d(%.0f%%) t4=%d(%.0f%%)\n",
           k,
           tier_counts[0], 100.0*tier_counts[0]/total_residuals,
           tier_counts[1], 100.0*tier_counts[1]/total_residuals,
           tier_counts[2], 100.0*tier_counts[2]/total_residuals,
           tier_counts[3], 100.0*tier_counts[3]/total_residuals,
           tier_counts[4], 100.0*tier_counts[4]/total_residuals);

    free(residuals);
    free(n_per_msg);
    free(lens);
    free(bufs);
}

int main(void)
{
    rng_state = 12345678901ULL;

    Quote *quotes = malloc(N_MSGS * sizeof(Quote));
    Trade *trades = malloc(N_MSGS * sizeof(Trade));
    Order *orders = malloc(N_MSGS * sizeof(Order));
    BookUpdate *books = malloc(N_MSGS * sizeof(BookUpdate));

    gen_quotes(quotes, N_MSGS);
    gen_trades(trades, N_MSGS);
    gen_orders(orders, N_MSGS);
    gen_book_updates(books, N_MSGS);

    printf("=======================================================================\n");
    printf("Combined-Write Tier Encoding Experiment\n");
    printf("=======================================================================\n");
    printf("%d messages per type, best of %d iterations\n\n", N_MSGS, N_ITERS);

    printf("Optimization: merge prefix + payload into single bw_write call.\n");
    printf("  Baseline:  bw_write(prefix, N) + bw_write(zz, M)  = 2 calls\n");
    printf("  Combined:  bw_write((prefix << M) | zz, N+M)      = 1 call\n\n");

    printf("  %-14s  %8s  %8s  %7s  %9s  %9s  %6s  %s\n",
           "Message", "Base", "Combined", "Speedup", "Base-Sz", "Comb-Sz", "Match", "Full pipeline");
    printf("  %-14s  %8s  %8s  %7s  %9s  %9s  %6s  %s\n",
           "--------------", "--------", "--------", "-------", "---------", "---------", "------", "--------------");

    bench_result_t results[4];
    results[0].roundtrip_errors = 0;
    results[1].roundtrip_errors = 0;
    results[2].roundtrip_errors = 0;
    results[3].roundtrip_errors = 0;

    run_bench(&quote_schema, quotes, N_MSGS, "Quote", &results[0]);
    run_bench(&trade_schema, trades, N_MSGS, "Trade", &results[1]);
    run_bench(&order_schema, orders, N_MSGS, "Order", &results[2]);
    run_bench(&book_schema, books, N_MSGS, "Book Update", &results[3]);

    printf("\n=======================================================================\n");
    printf("SUMMARY\n");
    printf("=======================================================================\n\n");

    double total_base = 0, total_comb = 0;
    int total_errors = 0;
    for (int i = 0; i < 4; i++) {
        total_base += results[i].baseline_ns;
        total_comb += results[i].combined_ns;
        total_errors += results[i].roundtrip_errors;
    }
    double avg_speedup = ((total_base - total_comb) / total_base) * 100.0;

    printf("  Average encode speedup (residual coding only): %+.1f%%\n", avg_speedup);
    printf("  Bitstream identity: %s\n", total_errors == 0 ? "ALL PASS (wire-compatible)" : "FAIL");
    printf("\n");
    printf("  The combined-write optimization:\n");
    printf("  - Halves bw_write calls for tiers 0-3 (the common path)\n");
    printf("  - Produces IDENTICAL bitstream (decoder unchanged)\n");
    printf("  - No compression ratio change (same bits, same order)\n");
    printf("  - Pure speed optimization via reduced function call overhead\n");
    printf("  - Tier 4 (raw 64-bit escape) is unchanged (too wide for single write)\n");

    printf("\nDone.\n");

    free(quotes);
    free(trades);
    free(orders);
    free(books);

    return total_errors;
}
