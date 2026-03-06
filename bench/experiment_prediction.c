/*
 * experiment_prediction.c — Test alternative prediction strategies
 *
 * Tests 5 prediction improvements ONE AT A TIME against the baseline,
 * measuring compression ratio impact on 100K synthetic messages.
 *
 * Build:
 *   cc -O2 -march=native -o experiment_prediction bench/experiment_prediction.c core/residc.c -Icore
 */
#define _POSIX_C_SOURCE 199309L
#include "residc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define N_MSGS 100000

/* ================================================================
 * Same LCG and data generation as bench_compression.c
 * ================================================================ */

static uint64_t rng_state;

static uint32_t rng_next(void) {
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(rng_state >> 32);
}

static uint32_t rng_range(uint32_t lo, uint32_t hi) {
    return lo + rng_next() % (hi - lo + 1);
}

/* k parameter accessors (same as residc.c) */
static inline int k_ts(int regime) { return regime == RESIDC_REGIME_VOLATILE ? 8 : 10; }
static inline int k_pr(int regime) { return regime == RESIDC_REGIME_VOLATILE ? 7 : 3; }
static inline int k_qt(int regime) { return regime == RESIDC_REGIME_VOLATILE ? 7 : 4; }
static inline int k_sq(int regime) { return regime == RESIDC_REGIME_VOLATILE ? 5 : 3; }

typedef struct {
    uint64_t timestamp;
    uint16_t instrument_id;
    uint32_t price;
    uint32_t quantity;
    uint8_t  side;
} Quote;

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

/* ================================================================
 * Helper: measure baseline compression using stock residc
 * ================================================================ */

static double measure_baseline(const residc_schema_t *schema, const void *msgs,
                               int n, int msg_size)
{
    residc_state_t enc;
    residc_init(&enc, schema);
    uint8_t buf[128];
    long total = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
        int len = residc_encode(&enc, msg, buf, 128);
        total += len;
    }
    int raw_size = residc_raw_size(schema);
    return (double)raw_size / ((double)total / n);
}

/* ================================================================
 * Common: commit quote state (identical to residc's commit_state
 * for the Quote schema). Must be called after each manual encode.
 * ================================================================ */

static void commit_quote(residc_state_t *s, const Quote *q)
{
    uint16_t id = q->instrument_id;
    residc_instrument_state_t *is = (id < RESIDC_MAX_INSTRUMENTS)
        ? &s->instruments[id] : NULL;

    /* Timestamp */
    int64_t gap = (int64_t)(q->timestamp - s->last_timestamp);
    int64_t pgap = s->timestamp_gap_ema >> 16;
    if (pgap < 0) pgap = 0;
    int64_t res = gap - pgap;
    uint64_t zz = residc_zigzag_enc(res);
    residc_adaptive_update(&s->ts_adapt_sum, &s->ts_adapt_count, zz);
    int64_t gq16 = (int64_t)((uint64_t)gap << 16);
    s->timestamp_gap_ema += (gq16 - s->timestamp_gap_ema) >> 2;
    s->last_timestamp = q->timestamp;
    s->last_timestamp_gap = (uint64_t)gap;

    /* Instrument */
    residc_mfu_update(&s->mfu, id);
    s->mfu_decay_counter++;
    if (s->mfu_decay_counter >= 10000) {
        for (int j = 0; j < s->mfu.num_entries; j++)
            s->mfu.entries[j].count >>= 1;
        s->mfu_decay_counter = 0;
    }
    s->last_instrument_id = id;

    /* Price/regime */
    if (is) {
        uint32_t pp = (is->msg_count > 0) ? is->last_price : 0;
        int64_t pr = (int64_t)q->price - (int64_t)pp;
        uint64_t abs_r = (uint64_t)(pr < 0 ? -pr : pr);
        s->recent_abs_price_sum += (uint32_t)abs_r;
        s->regime_counter++;
        if (s->regime_counter >= RESIDC_REGIME_WINDOW) {
            uint32_t avg = s->recent_abs_price_sum / RESIDC_REGIME_WINDOW;
            s->regime = (avg > 30) ? RESIDC_REGIME_VOLATILE : RESIDC_REGIME_CALM;
            s->recent_abs_price_sum = 0;
            s->regime_counter = 0;
        }
        is->last_price = q->price;
        is->last_qty = q->quantity;
        is->msg_count++;
    }
    s->msg_count++;
}

/* ================================================================
 * Manual "stock" encode for Quote — must match residc_encode exactly.
 * Returns frame size.
 * ================================================================ */

static int encode_quote_stock(residc_state_t *s, const Quote *q)
{
    residc_bitwriter_t bw;
    residc_bw_init(&bw);

    uint16_t id = q->instrument_id;
    residc_instrument_state_t *is = (id < RESIDC_MAX_INSTRUMENTS)
        ? &s->instruments[id] : NULL;

    /* Timestamp */
    int64_t gap = (int64_t)(q->timestamp - s->last_timestamp);
    int64_t pgap = s->timestamp_gap_ema >> 16;
    if (pgap < 0) pgap = 0;
    int64_t tres = gap - pgap;
    int kt = k_ts(s->regime);
    int k = residc_adaptive_k(s->ts_adapt_sum, s->ts_adapt_count, kt, kt + 10);
    residc_encode_residual(&bw, tres, k);

    /* Instrument */
    if (id == s->last_instrument_id && s->msg_count > 0) {
        residc_bw_write(&bw, 0, 1);
    } else {
        residc_bw_write(&bw, 1, 1);
        int mi = residc_mfu_lookup(&s->mfu, id);
        if (mi >= 0) {
            residc_bw_write(&bw, 0, 1);
            residc_bw_write(&bw, (uint64_t)mi, RESIDC_MFU_INDEX_BITS);
        } else {
            residc_bw_write(&bw, 1, 1);
            residc_bw_write(&bw, id, 14);
        }
    }

    /* Price */
    uint32_t pr = q->price;
    uint32_t pp = (is && is->msg_count > 0) ? is->last_price : 0;
    int64_t pres = (int64_t)pr - (int64_t)pp;
    int kp = k_pr(s->regime);
    if (pr % 100 == 0 && pp % 100 == 0 && (pr > 0 || pp > 0)) {
        residc_bw_write(&bw, 0, 1);
        residc_encode_residual(&bw, pres / 100, kp);
    } else {
        residc_bw_write(&bw, 1, 1);
        residc_encode_residual(&bw, pres, kp);
    }

    /* Quantity */
    uint32_t qt = q->quantity;
    uint32_t qp = (is && is->msg_count > 0) ? is->last_qty : 100;
    int64_t qres = (int64_t)qt - (int64_t)qp;
    int kq = k_qt(s->regime);
    if (qres == 0) {
        residc_bw_write(&bw, 0, 1);
    } else {
        residc_bw_write(&bw, 1, 1);
        if (qt % 100 == 0 && qp % 100 == 0) {
            residc_bw_write(&bw, 0, 1);
            residc_encode_residual(&bw, qres / 100, kq);
        } else {
            residc_bw_write(&bw, 1, 1);
            residc_encode_residual(&bw, qres, kq);
        }
    }

    /* Bool */
    residc_bw_write(&bw, q->side & 1, 1);

    int plen = residc_bw_finish(&bw);
    int raw = residc_raw_size(&quote_schema);
    if (plen >= raw || plen >= 254) return 1 + raw;
    return 1 + plen;
}

/* ================================================================
 * EXPERIMENT 1: Timestamp — last-gap prediction vs EMA
 *
 * Replaces EMA prediction with: predicted_gap = last_gap
 * ================================================================ */

static double experiment_timestamp_lastgap(Quote *quotes, int n)
{
    residc_state_t enc;
    residc_init(&enc, &quote_schema);
    long total = 0;

    /* Shadow state for last-gap timestamp prediction */
    uint64_t lg_last_gap = 0;
    uint64_t lg_adapt_sum = 0;
    uint32_t lg_adapt_count = 0;

    for (int i = 0; i < n; i++) {
        residc_bitwriter_t bw;
        residc_bw_init(&bw);

        uint16_t id = quotes[i].instrument_id;
        residc_instrument_state_t *is = (id < RESIDC_MAX_INSTRUMENTS)
            ? &enc.instruments[id] : NULL;

        /* MODIFIED: Timestamp with last-gap prediction */
        int64_t gap = (int64_t)(quotes[i].timestamp - enc.last_timestamp);
        int64_t pgap = (enc.msg_count == 0) ? 0 : (int64_t)lg_last_gap;
        int64_t tres = gap - pgap;
        int kt = k_ts(enc.regime);
        int k = residc_adaptive_k(lg_adapt_sum, lg_adapt_count, kt, kt + 10);
        residc_encode_residual(&bw, tres, k);

        /* Stock: instrument */
        if (id == enc.last_instrument_id && enc.msg_count > 0) {
            residc_bw_write(&bw, 0, 1);
        } else {
            residc_bw_write(&bw, 1, 1);
            int mi = residc_mfu_lookup(&enc.mfu, id);
            if (mi >= 0) {
                residc_bw_write(&bw, 0, 1);
                residc_bw_write(&bw, (uint64_t)mi, RESIDC_MFU_INDEX_BITS);
            } else {
                residc_bw_write(&bw, 1, 1);
                residc_bw_write(&bw, id, 14);
            }
        }

        /* Stock: price */
        uint32_t pr = quotes[i].price;
        uint32_t pp = (is && is->msg_count > 0) ? is->last_price : 0;
        int64_t pres = (int64_t)pr - (int64_t)pp;
        int kp = k_pr(enc.regime);
        if (pr % 100 == 0 && pp % 100 == 0 && (pr > 0 || pp > 0)) {
            residc_bw_write(&bw, 0, 1);
            residc_encode_residual(&bw, pres / 100, kp);
        } else {
            residc_bw_write(&bw, 1, 1);
            residc_encode_residual(&bw, pres, kp);
        }

        /* Stock: quantity */
        uint32_t qt = quotes[i].quantity;
        uint32_t qp = (is && is->msg_count > 0) ? is->last_qty : 100;
        int64_t qres = (int64_t)qt - (int64_t)qp;
        int kq = k_qt(enc.regime);
        if (qres == 0) {
            residc_bw_write(&bw, 0, 1);
        } else {
            residc_bw_write(&bw, 1, 1);
            if (qt % 100 == 0 && qp % 100 == 0) {
                residc_bw_write(&bw, 0, 1);
                residc_encode_residual(&bw, qres / 100, kq);
            } else {
                residc_bw_write(&bw, 1, 1);
                residc_encode_residual(&bw, qres, kq);
            }
        }

        residc_bw_write(&bw, quotes[i].side & 1, 1);

        int plen = residc_bw_finish(&bw);
        int raw = residc_raw_size(&quote_schema);
        if (plen >= raw || plen >= 254) total += 1 + raw;
        else total += 1 + plen;

        /* Update shadow last-gap state */
        uint64_t zz = residc_zigzag_enc(tres);
        residc_adaptive_update(&lg_adapt_sum, &lg_adapt_count, zz);
        lg_last_gap = (uint64_t)(gap > 0 ? gap : 0);

        /* Stock commit for all other state */
        commit_quote(&enc, &quotes[i]);
    }

    int raw = residc_raw_size(&quote_schema);
    return (double)raw / ((double)total / n);
}

/* ================================================================
 * EXPERIMENT 2: Price — stride prediction (linear extrapolation)
 *
 * Instead of last_price, predict: last_price + (last_price - 2nd_last_price)
 * ================================================================ */

typedef struct { uint32_t last; uint32_t second_last; uint32_t cnt; } stride_t;

static double experiment_price_stride(Quote *quotes, int n)
{
    residc_state_t enc;
    residc_init(&enc, &quote_schema);
    long total = 0;

    static stride_t st[RESIDC_MAX_INSTRUMENTS];
    memset(st, 0, sizeof(st));

    for (int i = 0; i < n; i++) {
        residc_bitwriter_t bw;
        residc_bw_init(&bw);

        uint16_t id = quotes[i].instrument_id;
        residc_instrument_state_t *is = (id < RESIDC_MAX_INSTRUMENTS)
            ? &enc.instruments[id] : NULL;
        stride_t *si = (id < RESIDC_MAX_INSTRUMENTS) ? &st[id] : NULL;

        /* Stock: timestamp */
        int64_t gap = (int64_t)(quotes[i].timestamp - enc.last_timestamp);
        int64_t pgap = enc.timestamp_gap_ema >> 16;
        if (pgap < 0) pgap = 0;
        int64_t tres = gap - pgap;
        int kt = k_ts(enc.regime);
        int k = residc_adaptive_k(enc.ts_adapt_sum, enc.ts_adapt_count, kt, kt + 10);
        residc_encode_residual(&bw, tres, k);

        /* Stock: instrument */
        if (id == enc.last_instrument_id && enc.msg_count > 0) {
            residc_bw_write(&bw, 0, 1);
        } else {
            residc_bw_write(&bw, 1, 1);
            int mi = residc_mfu_lookup(&enc.mfu, id);
            if (mi >= 0) {
                residc_bw_write(&bw, 0, 1);
                residc_bw_write(&bw, (uint64_t)mi, RESIDC_MFU_INDEX_BITS);
            } else {
                residc_bw_write(&bw, 1, 1);
                residc_bw_write(&bw, id, 14);
            }
        }

        /* MODIFIED: price with stride prediction */
        uint32_t pr = quotes[i].price;
        uint32_t pp;
        if (si && si->cnt >= 2) {
            int64_t stride = (int64_t)si->last + ((int64_t)si->last - (int64_t)si->second_last);
            pp = (uint32_t)(stride > 0 ? stride : 0);
        } else if (si && si->cnt > 0) {
            pp = si->last;
        } else {
            pp = 0;
        }
        int64_t pres = (int64_t)pr - (int64_t)pp;
        int kp = k_pr(enc.regime);
        if (pr % 100 == 0 && pp % 100 == 0 && (pr > 0 || pp > 0)) {
            residc_bw_write(&bw, 0, 1);
            residc_encode_residual(&bw, pres / 100, kp);
        } else {
            residc_bw_write(&bw, 1, 1);
            residc_encode_residual(&bw, pres, kp);
        }

        /* Stock: quantity */
        uint32_t qt = quotes[i].quantity;
        uint32_t qp = (is && is->msg_count > 0) ? is->last_qty : 100;
        int64_t qres = (int64_t)qt - (int64_t)qp;
        int kq = k_qt(enc.regime);
        if (qres == 0) {
            residc_bw_write(&bw, 0, 1);
        } else {
            residc_bw_write(&bw, 1, 1);
            if (qt % 100 == 0 && qp % 100 == 0) {
                residc_bw_write(&bw, 0, 1);
                residc_encode_residual(&bw, qres / 100, kq);
            } else {
                residc_bw_write(&bw, 1, 1);
                residc_encode_residual(&bw, qres, kq);
            }
        }

        residc_bw_write(&bw, quotes[i].side & 1, 1);

        int plen = residc_bw_finish(&bw);
        int raw = residc_raw_size(&quote_schema);
        if (plen >= raw || plen >= 254) total += 1 + raw;
        else total += 1 + plen;

        /* Update stride state */
        if (si) { si->second_last = si->last; si->last = pr; si->cnt++; }

        commit_quote(&enc, &quotes[i]);
    }

    int raw = residc_raw_size(&quote_schema);
    return (double)raw / ((double)total / n);
}

/* ================================================================
 * EXPERIMENT 3: Quantity — frequency table (top-4 per instrument)
 *
 * If qty matches one of the 4 most frequent, encode as 3 bits
 * (1-bit hit flag + 2-bit index). Otherwise 1-bit miss flag +
 * stock residual coding.
 * ================================================================ */

typedef struct {
    uint32_t qty[4];
    uint16_t cnt[4];
    uint32_t updates;
} qfreq_t;

static int qf_lookup(const qfreq_t *f, uint32_t q) {
    for (int i = 0; i < 4; i++)
        if (f->cnt[i] > 0 && f->qty[i] == q) return i;
    return -1;
}

static void qf_update(qfreq_t *f, uint32_t q) {
    for (int i = 0; i < 4; i++) {
        if (f->cnt[i] > 0 && f->qty[i] == q) {
            f->cnt[i]++;
            while (i > 0 && f->cnt[i] > f->cnt[i-1]) {
                uint32_t tq = f->qty[i-1]; uint16_t tc = f->cnt[i-1];
                f->qty[i-1] = f->qty[i]; f->cnt[i-1] = f->cnt[i];
                f->qty[i] = tq; f->cnt[i] = tc; i--;
            }
            goto done;
        }
    }
    if (f->cnt[3] <= 1) { f->qty[3] = q; f->cnt[3] = 1; }
done:
    f->updates++;
    if (f->updates >= 5000) {
        for (int i = 0; i < 4; i++) f->cnt[i] >>= 1;
        f->updates = 0;
    }
}

static double experiment_quantity_freq(Quote *quotes, int n)
{
    residc_state_t enc;
    residc_init(&enc, &quote_schema);
    long total = 0;

    static qfreq_t qft[RESIDC_MAX_INSTRUMENTS];
    memset(qft, 0, sizeof(qft));

    for (int i = 0; i < n; i++) {
        residc_bitwriter_t bw;
        residc_bw_init(&bw);

        uint16_t id = quotes[i].instrument_id;
        residc_instrument_state_t *is = (id < RESIDC_MAX_INSTRUMENTS)
            ? &enc.instruments[id] : NULL;
        qfreq_t *qf = (id < RESIDC_MAX_INSTRUMENTS) ? &qft[id] : NULL;

        /* Stock: timestamp */
        int64_t gap = (int64_t)(quotes[i].timestamp - enc.last_timestamp);
        int64_t pgap = enc.timestamp_gap_ema >> 16;
        if (pgap < 0) pgap = 0;
        int64_t tres = gap - pgap;
        int kt = k_ts(enc.regime);
        int k = residc_adaptive_k(enc.ts_adapt_sum, enc.ts_adapt_count, kt, kt + 10);
        residc_encode_residual(&bw, tres, k);

        /* Stock: instrument */
        if (id == enc.last_instrument_id && enc.msg_count > 0) {
            residc_bw_write(&bw, 0, 1);
        } else {
            residc_bw_write(&bw, 1, 1);
            int mi = residc_mfu_lookup(&enc.mfu, id);
            if (mi >= 0) {
                residc_bw_write(&bw, 0, 1);
                residc_bw_write(&bw, (uint64_t)mi, RESIDC_MFU_INDEX_BITS);
            } else {
                residc_bw_write(&bw, 1, 1);
                residc_bw_write(&bw, id, 14);
            }
        }

        /* Stock: price */
        uint32_t pr = quotes[i].price;
        uint32_t pp = (is && is->msg_count > 0) ? is->last_price : 0;
        int64_t pres = (int64_t)pr - (int64_t)pp;
        int kp = k_pr(enc.regime);
        if (pr % 100 == 0 && pp % 100 == 0 && (pr > 0 || pp > 0)) {
            residc_bw_write(&bw, 0, 1);
            residc_encode_residual(&bw, pres / 100, kp);
        } else {
            residc_bw_write(&bw, 1, 1);
            residc_encode_residual(&bw, pres, kp);
        }

        /* MODIFIED: quantity with frequency table */
        uint32_t qt = quotes[i].quantity;
        int fi = (qf && is && is->msg_count > 0) ? qf_lookup(qf, qt) : -1;
        if (fi >= 0) {
            residc_bw_write(&bw, 0, 1);  /* hit */
            residc_bw_write(&bw, (uint64_t)fi, 2);
        } else {
            residc_bw_write(&bw, 1, 1);  /* miss */
            uint32_t qp = (is && is->msg_count > 0) ? is->last_qty : 100;
            int64_t qres = (int64_t)qt - (int64_t)qp;
            int kq = k_qt(enc.regime);
            if (qres == 0) {
                residc_bw_write(&bw, 0, 1);
            } else {
                residc_bw_write(&bw, 1, 1);
                if (qt % 100 == 0 && qp % 100 == 0) {
                    residc_bw_write(&bw, 0, 1);
                    residc_encode_residual(&bw, qres / 100, kq);
                } else {
                    residc_bw_write(&bw, 1, 1);
                    residc_encode_residual(&bw, qres, kq);
                }
            }
        }

        residc_bw_write(&bw, quotes[i].side & 1, 1);

        int plen = residc_bw_finish(&bw);
        int raw = residc_raw_size(&quote_schema);
        if (plen >= raw || plen >= 254) total += 1 + raw;
        else total += 1 + plen;

        if (qf) qf_update(qf, qt);
        commit_quote(&enc, &quotes[i]);
    }

    int raw = residc_raw_size(&quote_schema);
    return (double)raw / ((double)total / n);
}

/* ================================================================
 * EXPERIMENT 4: Instrument — 2-back prediction
 *
 * After same-as-last check, try same-as-2-back before MFU.
 * Costs: 1 extra bit on the "not same-as-last" path for the
 * 2-back flag, but saves ~10 bits when 2-back hits.
 * ================================================================ */

static double experiment_instrument_2back(Quote *quotes, int n)
{
    residc_state_t enc;
    residc_init(&enc, &quote_schema);
    long total = 0;
    uint16_t second_last = 0;

    for (int i = 0; i < n; i++) {
        residc_bitwriter_t bw;
        residc_bw_init(&bw);

        uint16_t id = quotes[i].instrument_id;
        residc_instrument_state_t *is = (id < RESIDC_MAX_INSTRUMENTS)
            ? &enc.instruments[id] : NULL;

        /* Stock: timestamp */
        int64_t gap = (int64_t)(quotes[i].timestamp - enc.last_timestamp);
        int64_t pgap = enc.timestamp_gap_ema >> 16;
        if (pgap < 0) pgap = 0;
        int64_t tres = gap - pgap;
        int kt = k_ts(enc.regime);
        int k = residc_adaptive_k(enc.ts_adapt_sum, enc.ts_adapt_count, kt, kt + 10);
        residc_encode_residual(&bw, tres, k);

        /* MODIFIED: instrument with 2-back */
        if (id == enc.last_instrument_id && enc.msg_count > 0) {
            residc_bw_write(&bw, 0, 1);
        } else if (id == second_last && enc.msg_count > 1) {
            residc_bw_write(&bw, 1, 1);
            residc_bw_write(&bw, 0, 1);  /* 2-back hit: 2 bits total */
        } else {
            residc_bw_write(&bw, 1, 1);
            residc_bw_write(&bw, 1, 1);  /* not 2-back: +1 bit overhead */
            int mi = residc_mfu_lookup(&enc.mfu, id);
            if (mi >= 0) {
                residc_bw_write(&bw, 0, 1);
                residc_bw_write(&bw, (uint64_t)mi, RESIDC_MFU_INDEX_BITS);
            } else {
                residc_bw_write(&bw, 1, 1);
                residc_bw_write(&bw, id, 14);
            }
        }

        /* Stock: price */
        uint32_t pr = quotes[i].price;
        uint32_t pp = (is && is->msg_count > 0) ? is->last_price : 0;
        int64_t pres = (int64_t)pr - (int64_t)pp;
        int kp = k_pr(enc.regime);
        if (pr % 100 == 0 && pp % 100 == 0 && (pr > 0 || pp > 0)) {
            residc_bw_write(&bw, 0, 1);
            residc_encode_residual(&bw, pres / 100, kp);
        } else {
            residc_bw_write(&bw, 1, 1);
            residc_encode_residual(&bw, pres, kp);
        }

        /* Stock: quantity */
        uint32_t qt = quotes[i].quantity;
        uint32_t qp = (is && is->msg_count > 0) ? is->last_qty : 100;
        int64_t qres = (int64_t)qt - (int64_t)qp;
        int kq = k_qt(enc.regime);
        if (qres == 0) {
            residc_bw_write(&bw, 0, 1);
        } else {
            residc_bw_write(&bw, 1, 1);
            if (qt % 100 == 0 && qp % 100 == 0) {
                residc_bw_write(&bw, 0, 1);
                residc_encode_residual(&bw, qres / 100, kq);
            } else {
                residc_bw_write(&bw, 1, 1);
                residc_encode_residual(&bw, qres, kq);
            }
        }

        residc_bw_write(&bw, quotes[i].side & 1, 1);

        int plen = residc_bw_finish(&bw);
        int raw = residc_raw_size(&quote_schema);
        if (plen >= raw || plen >= 254) total += 1 + raw;
        else total += 1 + plen;

        second_last = enc.last_instrument_id;
        commit_quote(&enc, &quotes[i]);
    }

    int raw = residc_raw_size(&quote_schema);
    return (double)raw / ((double)total / n);
}

/* ================================================================
 * EXPERIMENT 5: Sequential ID — per-instrument typical delta
 *
 * Uses Trade messages. Predicts: id = last_id + typical_delta
 * where typical_delta is an EMA of recent deltas.
 * ================================================================ */

typedef struct { uint64_t last; int64_t typical; uint32_t cnt; } seqid_t;

static void commit_trade(residc_state_t *s, const Trade *t)
{
    uint16_t id = t->instrument_id;
    residc_instrument_state_t *is = (id < RESIDC_MAX_INSTRUMENTS)
        ? &s->instruments[id] : NULL;

    int64_t gap = (int64_t)(t->timestamp - s->last_timestamp);
    int64_t pgap = s->timestamp_gap_ema >> 16;
    if (pgap < 0) pgap = 0;
    int64_t res = gap - pgap;
    uint64_t zz = residc_zigzag_enc(res);
    residc_adaptive_update(&s->ts_adapt_sum, &s->ts_adapt_count, zz);
    int64_t gq16 = (int64_t)((uint64_t)gap << 16);
    s->timestamp_gap_ema += (gq16 - s->timestamp_gap_ema) >> 2;
    s->last_timestamp = t->timestamp;
    s->last_timestamp_gap = (uint64_t)gap;

    residc_mfu_update(&s->mfu, id);
    s->mfu_decay_counter++;
    if (s->mfu_decay_counter >= 10000) {
        for (int j = 0; j < s->mfu.num_entries; j++)
            s->mfu.entries[j].count >>= 1;
        s->mfu_decay_counter = 0;
    }
    s->last_instrument_id = id;

    if (is) {
        uint32_t pp = (is->msg_count > 0) ? is->last_price : 0;
        int64_t pr = (int64_t)t->price - (int64_t)pp;
        uint64_t abs_r = (uint64_t)(pr < 0 ? -pr : pr);
        s->recent_abs_price_sum += (uint32_t)abs_r;
        s->regime_counter++;
        if (s->regime_counter >= RESIDC_REGIME_WINDOW) {
            uint32_t avg = s->recent_abs_price_sum / RESIDC_REGIME_WINDOW;
            s->regime = (avg > 30) ? RESIDC_REGIME_VOLATILE : RESIDC_REGIME_CALM;
            s->recent_abs_price_sum = 0;
            s->regime_counter = 0;
        }
        is->last_price = t->price;
        is->last_qty = t->quantity;
        is->last_seq_id = t->trade_id;
        is->msg_count++;
    }

    /* Sequential ID adaptive state (field index 4) */
    {
        residc_field_state_t *fs = &s->field_state[4];
        uint64_t predicted = (is && is->last_seq_id > 0)
            ? is->last_seq_id : fs->last_value;
        /* Note: is->last_seq_id was just updated above, so use the value
         * from before that update. We need the pre-commit predicted value.
         * Actually commit_state in residc.c reads is->last_seq_id BEFORE
         * the instrument loop updates it... Let me re-check. */
        /* In residc.c, commit_state processes fields in order.
         * SEQUENTIAL_ID comes after INSTRUMENT, and is->last_seq_id
         * is updated IN the SEQUENTIAL_ID case. So when we reach
         * SEQUENTIAL_ID, is->last_seq_id still has the OLD value.
         * But we already set is->last_seq_id = t->trade_id above.
         * We need to NOT do that above and instead handle it here. */
    }

    s->field_state[4].last_value = t->trade_id;
    s->field_state[5].last_value = t->buyer_id;
    s->field_state[6].last_value = t->seller_id;
    s->msg_count++;
}

static double experiment_seqid_perdelta(Trade *trades, int n)
{
    residc_state_t enc;
    residc_init(&enc, &trade_schema);
    long total = 0;

    static seqid_t sq[RESIDC_MAX_INSTRUMENTS];
    memset(sq, 0, sizeof(sq));
    uint64_t g_last = 0;
    int64_t g_typical = 1;

    for (int i = 0; i < n; i++) {
        residc_bitwriter_t bw;
        residc_bw_init(&bw);

        uint16_t id = trades[i].instrument_id;
        residc_instrument_state_t *is = (id < RESIDC_MAX_INSTRUMENTS)
            ? &enc.instruments[id] : NULL;
        seqid_t *si = (id < RESIDC_MAX_INSTRUMENTS) ? &sq[id] : NULL;

        /* Stock: timestamp */
        int64_t gap = (int64_t)(trades[i].timestamp - enc.last_timestamp);
        int64_t pgap = enc.timestamp_gap_ema >> 16;
        if (pgap < 0) pgap = 0;
        int64_t tres = gap - pgap;
        int kt = k_ts(enc.regime);
        int k = residc_adaptive_k(enc.ts_adapt_sum, enc.ts_adapt_count, kt, kt + 10);
        residc_encode_residual(&bw, tres, k);

        /* Stock: instrument */
        if (id == enc.last_instrument_id && enc.msg_count > 0) {
            residc_bw_write(&bw, 0, 1);
        } else {
            residc_bw_write(&bw, 1, 1);
            int mi = residc_mfu_lookup(&enc.mfu, id);
            if (mi >= 0) {
                residc_bw_write(&bw, 0, 1);
                residc_bw_write(&bw, (uint64_t)mi, RESIDC_MFU_INDEX_BITS);
            } else {
                residc_bw_write(&bw, 1, 1);
                residc_bw_write(&bw, id, 14);
            }
        }

        /* Stock: price */
        uint32_t pr = trades[i].price;
        uint32_t pp = (is && is->msg_count > 0) ? is->last_price : 0;
        int64_t pres = (int64_t)pr - (int64_t)pp;
        int kp = k_pr(enc.regime);
        if (pr % 100 == 0 && pp % 100 == 0 && (pr > 0 || pp > 0)) {
            residc_bw_write(&bw, 0, 1);
            residc_encode_residual(&bw, pres / 100, kp);
        } else {
            residc_bw_write(&bw, 1, 1);
            residc_encode_residual(&bw, pres, kp);
        }

        /* Stock: quantity */
        uint32_t qt = trades[i].quantity;
        uint32_t qp = (is && is->msg_count > 0) ? is->last_qty : 100;
        int64_t qres = (int64_t)qt - (int64_t)qp;
        int kq = k_qt(enc.regime);
        if (qres == 0) {
            residc_bw_write(&bw, 0, 1);
        } else {
            residc_bw_write(&bw, 1, 1);
            if (qt % 100 == 0 && qp % 100 == 0) {
                residc_bw_write(&bw, 0, 1);
                residc_encode_residual(&bw, qres / 100, kq);
            } else {
                residc_bw_write(&bw, 1, 1);
                residc_encode_residual(&bw, qres, kq);
            }
        }

        /* MODIFIED: sequential ID with per-instrument typical delta */
        uint64_t seq = trades[i].trade_id;
        uint64_t pred;
        if (si && si->cnt > 0 && si->last > 0) {
            pred = (uint64_t)((int64_t)si->last + si->typical);
        } else if (g_last > 0) {
            pred = (uint64_t)((int64_t)g_last + g_typical);
        } else {
            pred = 0;
        }
        int64_t sres = (int64_t)(seq - pred);
        int ks = k_sq(enc.regime);
        residc_encode_residual(&bw, sres, ks);

        /* Stock: categorical buyer */
        if (trades[i].buyer_id == enc.field_state[5].last_value && enc.msg_count > 0) {
            residc_bw_write(&bw, 0, 1);
        } else {
            residc_bw_write(&bw, 1, 1);
            residc_bw_write(&bw, trades[i].buyer_id >> 16, 16);
            residc_bw_write(&bw, trades[i].buyer_id & 0xFFFF, 16);
        }

        /* Stock: categorical seller */
        if (trades[i].seller_id == enc.field_state[6].last_value && enc.msg_count > 0) {
            residc_bw_write(&bw, 0, 1);
        } else {
            residc_bw_write(&bw, 1, 1);
            residc_bw_write(&bw, trades[i].seller_id >> 16, 16);
            residc_bw_write(&bw, trades[i].seller_id & 0xFFFF, 16);
        }

        /* Stock: bool */
        residc_bw_write(&bw, trades[i].aggressor_side & 1, 1);

        int plen = residc_bw_finish(&bw);
        int raw = residc_raw_size(&trade_schema);
        if (plen >= raw || plen >= 254) total += 1 + raw;
        else total += 1 + plen;

        /* Update per-instrument seq tracking */
        if (si) {
            if (si->last > 0) {
                int64_t ad = (int64_t)(seq - si->last);
                si->typical += (ad - si->typical) >> 2;
            } else {
                si->typical = 1;
            }
            si->last = seq;
            si->cnt++;
        }
        int64_t ga = (g_last > 0) ? (int64_t)(seq - g_last) : 1;
        g_typical += (ga - g_typical) >> 2;
        g_last = seq;

        /* Commit trade state */
        /* Timestamp */
        uint64_t zz = residc_zigzag_enc(tres);
        residc_adaptive_update(&enc.ts_adapt_sum, &enc.ts_adapt_count, zz);
        int64_t gq16 = (int64_t)((uint64_t)gap << 16);
        enc.timestamp_gap_ema += (gq16 - enc.timestamp_gap_ema) >> 2;
        enc.last_timestamp = trades[i].timestamp;
        enc.last_timestamp_gap = (uint64_t)gap;

        /* Instrument */
        residc_mfu_update(&enc.mfu, id);
        enc.mfu_decay_counter++;
        if (enc.mfu_decay_counter >= 10000) {
            for (int j = 0; j < enc.mfu.num_entries; j++)
                enc.mfu.entries[j].count >>= 1;
            enc.mfu_decay_counter = 0;
        }
        enc.last_instrument_id = id;

        /* Price/regime */
        if (is) {
            uint32_t ppx = (is->msg_count > 0) ? is->last_price : 0;
            int64_t prx = (int64_t)pr - (int64_t)ppx;
            uint64_t abs_r = (uint64_t)(prx < 0 ? -prx : prx);
            enc.recent_abs_price_sum += (uint32_t)abs_r;
            enc.regime_counter++;
            if (enc.regime_counter >= RESIDC_REGIME_WINDOW) {
                uint32_t avg = enc.recent_abs_price_sum / RESIDC_REGIME_WINDOW;
                enc.regime = (avg > 30) ? RESIDC_REGIME_VOLATILE : RESIDC_REGIME_CALM;
                enc.recent_abs_price_sum = 0;
                enc.regime_counter = 0;
            }
            is->last_price = pr;
            is->last_qty = qt;
            is->msg_count++;
        }

        /* Sequential ID adaptive (stock uses field_state for adaptive k) */
        {
            residc_field_state_t *fs = &enc.field_state[4];
            uint64_t stock_pred = (is && is->last_seq_id > 0)
                ? is->last_seq_id : fs->last_value;
            int64_t stock_delta = (int64_t)(seq - stock_pred);
            uint64_t szz = residc_zigzag_enc(stock_delta);
            residc_adaptive_update(&fs->adapt_sum, &fs->adapt_count, szz);
            fs->last_value = seq;
            if (is) is->last_seq_id = seq;
        }

        enc.field_state[5].last_value = trades[i].buyer_id;
        enc.field_state[6].last_value = trades[i].seller_id;
        enc.msg_count++;
    }

    int raw = residc_raw_size(&trade_schema);
    return (double)raw / ((double)total / n);
}

/* ================================================================
 * Main: verify manual baseline, run experiments, rank results
 * ================================================================ */

int main(void)
{
    printf("residc prediction strategy experiments (%d messages)\n", N_MSGS);
    printf("================================================================\n\n");

    rng_state = 12345678901ULL;
    Quote *quotes = malloc(N_MSGS * sizeof(Quote));
    Trade *trades = malloc(N_MSGS * sizeof(Trade));
    gen_quotes(quotes, N_MSGS);
    gen_trades(trades, N_MSGS);

    /* Baselines using stock residc_encode */
    double base_quote = measure_baseline(&quote_schema, quotes, N_MSGS, sizeof(Quote));
    double base_trade = measure_baseline(&trade_schema, trades, N_MSGS, sizeof(Trade));

    /* Verify manual baseline matches stock */
    {
        residc_state_t enc;
        residc_init(&enc, &quote_schema);
        long manual_total = 0;
        for (int i = 0; i < N_MSGS; i++) {
            int len = encode_quote_stock(&enc, &quotes[i]);
            manual_total += len;
            commit_quote(&enc, &quotes[i]);
        }
        double manual_ratio = (double)residc_raw_size(&quote_schema) /
                              ((double)manual_total / N_MSGS);
        printf("Baseline verification:\n");
        printf("  Stock residc_encode: %.4f:1\n", base_quote);
        printf("  Manual replication:  %.4f:1\n", manual_ratio);
        printf("  Match: %s (delta=%.4f%%)\n\n",
               (manual_ratio - base_quote) / base_quote * 100.0 < 0.1 ? "CLOSE" : "MISMATCH",
               (manual_ratio - base_quote) / base_quote * 100.0);
    }

    printf("BASELINES:\n");
    printf("  Quote: %.3f:1  (%d B raw)\n", base_quote, residc_raw_size(&quote_schema));
    printf("  Trade: %.3f:1  (%d B raw)\n\n", base_trade, residc_raw_size(&trade_schema));

    /* Run experiments, compare to MANUAL baseline (not stock) for fair comparison */
    /* Since manual = stock (verified above), we compare to stock */

    double exp1 = experiment_timestamp_lastgap(quotes, N_MSGS);
    double exp1_pct = (exp1 - base_quote) / base_quote * 100.0;
    printf("EXP 1: Timestamp last-gap prediction (Quote)\n");
    printf("  Ratio: %.3f:1  vs baseline %.3f:1  (%+.2f%%)\n\n",
           exp1, base_quote, exp1_pct);

    double exp2 = experiment_price_stride(quotes, N_MSGS);
    double exp2_pct = (exp2 - base_quote) / base_quote * 100.0;
    printf("EXP 2: Price stride prediction (Quote)\n");
    printf("  Ratio: %.3f:1  vs baseline %.3f:1  (%+.2f%%)\n\n",
           exp2, base_quote, exp2_pct);

    double exp3 = experiment_quantity_freq(quotes, N_MSGS);
    double exp3_pct = (exp3 - base_quote) / base_quote * 100.0;
    printf("EXP 3: Quantity frequency table (Quote)\n");
    printf("  Ratio: %.3f:1  vs baseline %.3f:1  (%+.2f%%)\n\n",
           exp3, base_quote, exp3_pct);

    double exp4 = experiment_instrument_2back(quotes, N_MSGS);
    double exp4_pct = (exp4 - base_quote) / base_quote * 100.0;
    printf("EXP 4: Instrument 2-back (Quote)\n");
    printf("  Ratio: %.3f:1  vs baseline %.3f:1  (%+.2f%%)\n\n",
           exp4, base_quote, exp4_pct);

    double exp5 = experiment_seqid_perdelta(trades, N_MSGS);
    double exp5_pct = (exp5 - base_trade) / base_trade * 100.0;
    printf("EXP 5: SeqID per-instrument delta (Trade)\n");
    printf("  Ratio: %.3f:1  vs baseline %.3f:1  (%+.2f%%)\n\n",
           exp5, base_trade, exp5_pct);

    /* Ranking */
    printf("================================================================\n");
    printf("RANKING (sorted by compression improvement, positive = better):\n");
    printf("================================================================\n");

    struct { const char *name; double pct; const char *msg_type; } r[5] = {
        {"Timestamp last-gap",    exp1_pct, "Quote"},
        {"Price stride",          exp2_pct, "Quote"},
        {"Quantity freq table",   exp3_pct, "Quote"},
        {"Instrument 2-back",     exp4_pct, "Quote"},
        {"SeqID per-inst delta",  exp5_pct, "Trade"},
    };

    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 5; j++)
            if (r[j].pct > r[i].pct) {
                typeof(r[0]) t = r[i]; r[i] = r[j]; r[j] = t;
            }

    for (int i = 0; i < 5; i++) {
        const char *verdict = r[i].pct > 0.5 ? "HELPS" :
                              r[i].pct > -0.5 ? "NEUTRAL" : "HURTS";
        printf("  #%d  %-28s  %+.2f%%  [%s]  (on %s)\n",
               i + 1, r[i].name, r[i].pct, verdict, r[i].msg_type);
    }
    printf("\n");

    free(quotes);
    free(trades);
    return 0;
}
