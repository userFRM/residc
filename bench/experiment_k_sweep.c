/*
 * experiment_k_sweep.c -- Systematic k parameter sweep for tiered coding
 *
 * For each field type (Timestamp, Price, Quantity, SeqId), test k=1..15
 * and find the value that minimizes total encoded bits. Compare against
 * current defaults.
 *
 * Also sweeps regime threshold (currently 30) to find optimal split.
 *
 * Build:
 *   cc -O2 -march=native -o experiment_k_sweep \
 *     bench/experiment_k_sweep.c core/residc.c -Icore
 *
 * Run:
 *   ./experiment_k_sweep
 */

#define _POSIX_C_SOURCE 199309L
#include "residc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <float.h>
#include <math.h>

#define N_MSGS  100000
#define K_MIN   1
#define K_MAX   15

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
 * Tiered coding cost calculator
 * ================================================================ */

static int tiered_bits(int64_t value, int k)
{
    uint64_t zz = residc_zigzag_enc(value);
    if (zz < (1ULL << k))         return 1 + k;
    if (zz < (1ULL << (k + 2)))   return 2 + (k + 2);
    if (zz < (1ULL << (k + 5)))   return 3 + (k + 5);
    if (zz < (1ULL << (k + 10)))  return 4 + (k + 10);
    return 4 + 64;
}

/* ================================================================
 * Per-field-type residual extraction
 *
 * Extracts residuals by field type, separated, so we can sweep k
 * for each independently.
 * ================================================================ */

typedef struct {
    int64_t *timestamp_residuals;
    int      n_timestamp;
    int64_t *price_residuals;
    int      n_price;
    int64_t *quantity_residuals;
    int      n_quantity;
    int64_t *seqid_residuals;
    int      n_seqid;
} field_residuals_t;

static void extract_field_residuals(
    const residc_schema_t *schema, const void *msgs, int n,
    field_residuals_t *fr)
{
    int msg_size = schema->msg_size;
    /* Over-allocate: at most n residuals per field type */
    fr->timestamp_residuals = calloc(n, sizeof(int64_t));
    fr->price_residuals     = calloc(n, sizeof(int64_t));
    fr->quantity_residuals  = calloc(n, sizeof(int64_t));
    fr->seqid_residuals     = calloc(n, sizeof(int64_t));
    fr->n_timestamp = fr->n_price = fr->n_quantity = fr->n_seqid = 0;

    residc_state_t *state = calloc(1, sizeof(residc_state_t));
    residc_init(state, schema);

    for (int i = 0; i < n; i++) {
        const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
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
                fr->timestamp_residuals[fr->n_timestamp++] = gap - pg;
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
                    fr->price_residuals[fr->n_price++] = r / 100;
                else
                    fr->price_residuals[fr->n_price++] = r;
                break;
            }
            case RESIDC_QUANTITY: {
                uint32_t qty = (uint32_t)val;
                uint32_t pred = (is && is->msg_count > 0) ? is->last_qty : 100;
                int64_t r = (int64_t)qty - (int64_t)pred;
                if (r != 0) {
                    if (qty % 100 == 0 && pred % 100 == 0)
                        fr->quantity_residuals[fr->n_quantity++] = r / 100;
                    else
                        fr->quantity_residuals[fr->n_quantity++] = r;
                }
                break;
            }
            case RESIDC_SEQUENTIAL_ID: {
                residc_field_state_t *fs = &state->field_state[fi];
                uint64_t pred = (is && is->last_seq_id > 0)
                               ? is->last_seq_id : fs->last_value;
                fr->seqid_residuals[fr->n_seqid++] = (int64_t)(val - pred);
                break;
            }
            default:
                break;
            }
        }

        /* Advance codec state */
        uint8_t buf[128];
        residc_encode(state, msg, buf, 128);
    }

    free(state);
}

static void free_field_residuals(field_residuals_t *fr)
{
    free(fr->timestamp_residuals);
    free(fr->price_residuals);
    free(fr->quantity_residuals);
    free(fr->seqid_residuals);
}

/* ================================================================
 * Sweep k for a single residual array
 * ================================================================ */

typedef struct {
    long total_bits[K_MAX + 1];  /* index by k (0 unused) */
    int  best_k;
    long best_bits;
} k_sweep_result_t;

static void sweep_k(const int64_t *residuals, int n, k_sweep_result_t *result)
{
    result->best_k = 1;
    result->best_bits = (long)1e18;

    for (int k = K_MIN; k <= K_MAX; k++) {
        long total = 0;
        for (int i = 0; i < n; i++)
            total += tiered_bits(residuals[i], k);
        result->total_bits[k] = total;
        if (total < result->best_bits) {
            result->best_bits = total;
            result->best_k = k;
        }
    }
}

/* ================================================================
 * Residual distribution stats
 * ================================================================ */

typedef struct {
    int count;
    double mean_abs;
    double median_abs;
    uint64_t p50;    /* 50th percentile of |zz| */
    uint64_t p90;    /* 90th percentile */
    uint64_t p99;    /* 99th percentile */
    uint64_t max_abs;
    int ideal_k;     /* floor(log2(median)) */
} residual_stats_t;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static void compute_stats(const int64_t *residuals, int n, residual_stats_t *stats)
{
    stats->count = n;
    if (n == 0) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    uint64_t *abs_vals = malloc(n * sizeof(uint64_t));
    double sum = 0;
    uint64_t mx = 0;
    for (int i = 0; i < n; i++) {
        uint64_t zz = residc_zigzag_enc(residuals[i]);
        abs_vals[i] = zz;
        sum += (double)zz;
        if (zz > mx) mx = zz;
    }

    qsort(abs_vals, n, sizeof(uint64_t), cmp_u64);

    stats->mean_abs = sum / n;
    stats->p50 = abs_vals[n / 2];
    stats->p90 = abs_vals[(int)(n * 0.9)];
    stats->p99 = abs_vals[(int)(n * 0.99)];
    stats->max_abs = mx;
    stats->median_abs = (double)stats->p50;

    /* Ideal k: floor(log2(median+1)) */
    uint64_t m = stats->p50;
    int k = 0;
    while (m > 0) { k++; m >>= 1; }
    stats->ideal_k = k;

    free(abs_vals);
}

/* ================================================================
 * Full-pipeline compression comparison at different k values
 *
 * We can't easily change k in the real codec (it's hardcoded), but
 * we can measure the tiered bit cost of each field and sum them to
 * get the total message cost for different k combinations.
 * ================================================================ */

typedef struct {
    const char *name;
    field_residuals_t fr;
    k_sweep_result_t ts_sweep;
    k_sweep_result_t price_sweep;
    k_sweep_result_t qty_sweep;
    k_sweep_result_t seqid_sweep;
    residual_stats_t ts_stats;
    residual_stats_t price_stats;
    residual_stats_t qty_stats;
    residual_stats_t seqid_stats;
    /* Total bits at current default k vs optimal k */
    long bits_at_default;
    long bits_at_optimal;
} msg_result_t;

/* Current default k values (CALM regime) */
#define K_TS_DEFAULT     10
#define K_PRICE_DEFAULT   3
#define K_QTY_DEFAULT     4
#define K_SEQID_DEFAULT   3

static void run_sweep(
    const residc_schema_t *schema, const void *msgs, int n,
    const char *name, msg_result_t *result)
{
    result->name = name;

    extract_field_residuals(schema, msgs, n, &result->fr);

    /* Compute stats */
    compute_stats(result->fr.timestamp_residuals, result->fr.n_timestamp, &result->ts_stats);
    compute_stats(result->fr.price_residuals, result->fr.n_price, &result->price_stats);
    compute_stats(result->fr.quantity_residuals, result->fr.n_quantity, &result->qty_stats);
    compute_stats(result->fr.seqid_residuals, result->fr.n_seqid, &result->seqid_stats);

    /* Sweep k for each field type */
    sweep_k(result->fr.timestamp_residuals, result->fr.n_timestamp, &result->ts_sweep);
    sweep_k(result->fr.price_residuals, result->fr.n_price, &result->price_sweep);
    sweep_k(result->fr.quantity_residuals, result->fr.n_quantity, &result->qty_sweep);
    sweep_k(result->fr.seqid_residuals, result->fr.n_seqid, &result->seqid_sweep);

    /* Total bits at default k */
    result->bits_at_default = 0;
    if (result->fr.n_timestamp > 0)
        result->bits_at_default += result->ts_sweep.total_bits[K_TS_DEFAULT];
    if (result->fr.n_price > 0)
        result->bits_at_default += result->price_sweep.total_bits[K_PRICE_DEFAULT];
    if (result->fr.n_quantity > 0)
        result->bits_at_default += result->qty_sweep.total_bits[K_QTY_DEFAULT];
    if (result->fr.n_seqid > 0)
        result->bits_at_default += result->seqid_sweep.total_bits[K_SEQID_DEFAULT];

    /* Total bits at per-field optimal k */
    result->bits_at_optimal = 0;
    if (result->fr.n_timestamp > 0)
        result->bits_at_optimal += result->ts_sweep.best_bits;
    if (result->fr.n_price > 0)
        result->bits_at_optimal += result->price_sweep.best_bits;
    if (result->fr.n_quantity > 0)
        result->bits_at_optimal += result->qty_sweep.best_bits;
    if (result->fr.n_seqid > 0)
        result->bits_at_optimal += result->seqid_sweep.best_bits;
}

static void print_sweep_table(const char *field_name, const k_sweep_result_t *sw,
                               int default_k, int n_residuals)
{
    if (n_residuals == 0) return;

    printf("  %-12s  (default k=%d, optimal k=%d, %d residuals)\n",
           field_name, default_k, sw->best_k, n_residuals);
    printf("    k:  ");
    for (int k = K_MIN; k <= K_MAX; k++)
        printf(" %4d", k);
    printf("\n");
    printf("    b/r:");
    for (int k = K_MIN; k <= K_MAX; k++) {
        double bpr = (double)sw->total_bits[k] / n_residuals;
        printf(" %4.1f", bpr);
    }
    printf("\n");

    /* Highlight: bits/residual at default vs best */
    double bpr_default = (double)sw->total_bits[default_k] / n_residuals;
    double bpr_best = (double)sw->best_bits / n_residuals;
    double saving = ((bpr_default - bpr_best) / bpr_default) * 100.0;
    printf("    default=%4.1f b/r  optimal=%4.1f b/r  saving=%+.1f%%",
           bpr_default, bpr_best, saving);
    if (sw->best_k != default_k)
        printf("  [k=%d -> k=%d]", default_k, sw->best_k);
    else
        printf("  [default is optimal]");
    printf("\n\n");
}

static void print_stats(const char *field_name, const residual_stats_t *st)
{
    if (st->count == 0) return;
    printf("    %-12s: n=%d  mean_zz=%.1f  p50=%llu  p90=%llu  p99=%llu  max=%llu  ideal_k=%d\n",
           field_name, st->count, st->mean_abs,
           (unsigned long long)st->p50,
           (unsigned long long)st->p90,
           (unsigned long long)st->p99,
           (unsigned long long)st->max_abs,
           st->ideal_k);
}

/* ================================================================
 * Entropy analysis: what is the theoretical minimum?
 * ================================================================ */

static double entropy_bits(const int64_t *residuals, int n)
{
    if (n == 0) return 0;

    /* Count unique zigzag values */
    uint64_t *vals = malloc(n * sizeof(uint64_t));
    for (int i = 0; i < n; i++)
        vals[i] = residc_zigzag_enc(residuals[i]);
    qsort(vals, n, sizeof(uint64_t), cmp_u64);

    double H = 0;
    int run = 1;
    for (int i = 1; i <= n; i++) {
        if (i < n && vals[i] == vals[i-1]) {
            run++;
        } else {
            double p = (double)run / n;
            H -= p * (p > 0 ? log(p) / log(2.0) : 0);
            run = 1;
        }
    }

    free(vals);
    return H;
}

/* ================================================================
 * Main
 * ================================================================ */

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
    printf("Systematic k Parameter Sweep\n");
    printf("=======================================================================\n");
    printf("%d messages per type, k range [%d, %d]\n\n", N_MSGS, K_MIN, K_MAX);

    printf("Current defaults (CALM): timestamp=%d, price=%d, quantity=%d, seqid=%d\n",
           K_TS_DEFAULT, K_PRICE_DEFAULT, K_QTY_DEFAULT, K_SEQID_DEFAULT);
    printf("Current defaults (VOLATILE): timestamp=8, price=7, quantity=7, seqid=5\n\n");

    msg_result_t results[4];
    run_sweep(&quote_schema, quotes, N_MSGS, "Quote", &results[0]);
    run_sweep(&trade_schema, trades, N_MSGS, "Trade", &results[1]);
    run_sweep(&order_schema, orders, N_MSGS, "Order", &results[2]);
    run_sweep(&book_schema, books, N_MSGS, "Book Update", &results[3]);

    /* Print detailed results per message type */
    for (int i = 0; i < 4; i++) {
        msg_result_t *r = &results[i];
        printf("=======================================================================\n");
        printf("%s\n", r->name);
        printf("=======================================================================\n\n");

        printf("  Residual distribution stats (zigzag-encoded):\n");
        print_stats("Timestamp", &r->ts_stats);
        print_stats("Price", &r->price_stats);
        print_stats("Quantity", &r->qty_stats);
        print_stats("SeqId", &r->seqid_stats);
        printf("\n");

        printf("  k sweep (bits/residual at each k):\n\n");
        print_sweep_table("Timestamp", &r->ts_sweep, K_TS_DEFAULT, r->fr.n_timestamp);
        print_sweep_table("Price", &r->price_sweep, K_PRICE_DEFAULT, r->fr.n_price);
        print_sweep_table("Quantity", &r->qty_sweep, K_QTY_DEFAULT, r->fr.n_quantity);
        print_sweep_table("SeqId", &r->seqid_sweep, K_SEQID_DEFAULT, r->fr.n_seqid);

        /* Entropy lower bound */
        printf("  Entropy lower bounds (bits/residual):\n");
        if (r->fr.n_timestamp > 0)
            printf("    Timestamp: %.1f b/r (tiered@opt=%.1f, overhead=%.1f%%)\n",
                   entropy_bits(r->fr.timestamp_residuals, r->fr.n_timestamp),
                   (double)r->ts_sweep.best_bits / r->fr.n_timestamp,
                   100.0 * ((double)r->ts_sweep.best_bits / r->fr.n_timestamp /
                            entropy_bits(r->fr.timestamp_residuals, r->fr.n_timestamp) - 1));
        if (r->fr.n_price > 0)
            printf("    Price:     %.1f b/r (tiered@opt=%.1f, overhead=%.1f%%)\n",
                   entropy_bits(r->fr.price_residuals, r->fr.n_price),
                   (double)r->price_sweep.best_bits / r->fr.n_price,
                   100.0 * ((double)r->price_sweep.best_bits / r->fr.n_price /
                            entropy_bits(r->fr.price_residuals, r->fr.n_price) - 1));
        if (r->fr.n_quantity > 0)
            printf("    Quantity:  %.1f b/r (tiered@opt=%.1f, overhead=%.1f%%)\n",
                   entropy_bits(r->fr.quantity_residuals, r->fr.n_quantity),
                   (double)r->qty_sweep.best_bits / r->fr.n_quantity,
                   100.0 * ((double)r->qty_sweep.best_bits / r->fr.n_quantity /
                            entropy_bits(r->fr.quantity_residuals, r->fr.n_quantity) - 1));
        if (r->fr.n_seqid > 0)
            printf("    SeqId:     %.1f b/r (tiered@opt=%.1f, overhead=%.1f%%)\n",
                   entropy_bits(r->fr.seqid_residuals, r->fr.n_seqid),
                   (double)r->seqid_sweep.best_bits / r->fr.n_seqid,
                   100.0 * ((double)r->seqid_sweep.best_bits / r->fr.n_seqid /
                            entropy_bits(r->fr.seqid_residuals, r->fr.n_seqid) - 1));
        printf("\n");
    }

    /* Summary: savings from optimal k */
    printf("=======================================================================\n");
    printf("SUMMARY: Default k vs Optimal k (residual bits only)\n");
    printf("=======================================================================\n\n");

    printf("%-14s  %12s  %12s  %8s  %s\n",
           "Message", "Default-k", "Optimal-k", "Saving", "Optimal k values");
    printf("%-14s  %12s  %12s  %8s  %s\n",
           "--------------", "------------", "------------", "--------",
           "-----------------------------");

    long total_default = 0, total_optimal = 0;
    for (int i = 0; i < 4; i++) {
        msg_result_t *r = &results[i];
        double saving = ((double)(r->bits_at_default - r->bits_at_optimal) /
                          r->bits_at_default) * 100.0;

        char opt_str[128];
        snprintf(opt_str, sizeof(opt_str), "ts=%d pr=%d qt=%d sq=%d",
                 r->ts_sweep.best_k, r->price_sweep.best_k,
                 r->qty_sweep.best_k, r->seqid_sweep.best_k);

        printf("%-14s  %9ld b   %9ld b   %+5.1f%%   %s\n",
               r->name, r->bits_at_default, r->bits_at_optimal,
               saving, opt_str);

        total_default += r->bits_at_default;
        total_optimal += r->bits_at_optimal;
    }

    double total_saving = ((double)(total_default - total_optimal) / total_default) * 100.0;
    printf("%-14s  %9ld b   %9ld b   %+5.1f%%\n",
           "TOTAL", total_default, total_optimal, total_saving);

    /* Consensus optimal k values across all message types */
    printf("\n=======================================================================\n");
    printf("CROSS-MESSAGE CONSENSUS\n");
    printf("=======================================================================\n\n");

    /* Aggregate all residuals of each type across all messages */
    int64_t *all_ts = NULL, *all_pr = NULL, *all_qt = NULL, *all_sq = NULL;
    int n_all_ts = 0, n_all_pr = 0, n_all_qt = 0, n_all_sq = 0;
    for (int i = 0; i < 4; i++) {
        n_all_ts += results[i].fr.n_timestamp;
        n_all_pr += results[i].fr.n_price;
        n_all_qt += results[i].fr.n_quantity;
        n_all_sq += results[i].fr.n_seqid;
    }

    all_ts = malloc(n_all_ts * sizeof(int64_t));
    all_pr = malloc(n_all_pr * sizeof(int64_t));
    all_qt = malloc(n_all_qt * sizeof(int64_t));
    all_sq = malloc(n_all_sq * sizeof(int64_t));

    int off_ts = 0, off_pr = 0, off_qt = 0, off_sq = 0;
    for (int i = 0; i < 4; i++) {
        memcpy(all_ts + off_ts, results[i].fr.timestamp_residuals,
               results[i].fr.n_timestamp * sizeof(int64_t));
        off_ts += results[i].fr.n_timestamp;
        memcpy(all_pr + off_pr, results[i].fr.price_residuals,
               results[i].fr.n_price * sizeof(int64_t));
        off_pr += results[i].fr.n_price;
        memcpy(all_qt + off_qt, results[i].fr.quantity_residuals,
               results[i].fr.n_quantity * sizeof(int64_t));
        off_qt += results[i].fr.n_quantity;
        memcpy(all_sq + off_sq, results[i].fr.seqid_residuals,
               results[i].fr.n_seqid * sizeof(int64_t));
        off_sq += results[i].fr.n_seqid;
    }

    k_sweep_result_t agg_ts, agg_pr, agg_qt, agg_sq;
    sweep_k(all_ts, n_all_ts, &agg_ts);
    sweep_k(all_pr, n_all_pr, &agg_pr);
    sweep_k(all_qt, n_all_qt, &agg_qt);
    sweep_k(all_sq, n_all_sq, &agg_sq);

    printf("  Aggregated optimal k (all %d msg types combined):\n", 4);
    printf("    Timestamp: k=%d  (current=%d)  %+.1f%% bits\n",
           agg_ts.best_k, K_TS_DEFAULT,
           n_all_ts > 0 ? ((double)(agg_ts.total_bits[K_TS_DEFAULT] - agg_ts.best_bits) /
            agg_ts.total_bits[K_TS_DEFAULT]) * 100.0 : 0.0);
    printf("    Price:     k=%d  (current=%d)  %+.1f%% bits\n",
           agg_pr.best_k, K_PRICE_DEFAULT,
           n_all_pr > 0 ? ((double)(agg_pr.total_bits[K_PRICE_DEFAULT] - agg_pr.best_bits) /
            agg_pr.total_bits[K_PRICE_DEFAULT]) * 100.0 : 0.0);
    printf("    Quantity:  k=%d  (current=%d)  %+.1f%% bits\n",
           agg_qt.best_k, K_QTY_DEFAULT,
           n_all_qt > 0 ? ((double)(agg_qt.total_bits[K_QTY_DEFAULT] - agg_qt.best_bits) /
            agg_qt.total_bits[K_QTY_DEFAULT]) * 100.0 : 0.0);
    printf("    SeqId:     k=%d  (current=%d)  %+.1f%% bits\n",
           agg_sq.best_k, K_SEQID_DEFAULT,
           n_all_sq > 0 ? ((double)(agg_sq.total_bits[K_SEQID_DEFAULT] - agg_sq.best_bits) /
            agg_sq.total_bits[K_SEQID_DEFAULT]) * 100.0 : 0.0);

    /* Total aggregated saving */
    long agg_default = 0, agg_optimal = 0;
    if (n_all_ts > 0) { agg_default += agg_ts.total_bits[K_TS_DEFAULT]; agg_optimal += agg_ts.best_bits; }
    if (n_all_pr > 0) { agg_default += agg_pr.total_bits[K_PRICE_DEFAULT]; agg_optimal += agg_pr.best_bits; }
    if (n_all_qt > 0) { agg_default += agg_qt.total_bits[K_QTY_DEFAULT]; agg_optimal += agg_qt.best_bits; }
    if (n_all_sq > 0) { agg_default += agg_sq.total_bits[K_SEQID_DEFAULT]; agg_optimal += agg_sq.best_bits; }

    printf("\n    Total residual bits: default=%ld  optimal=%ld  saving=%+.1f%%\n",
           agg_default, agg_optimal,
           ((double)(agg_default - agg_optimal) / agg_default) * 100.0);

    printf("\n=======================================================================\n");
    printf("IMPORTANT CAVEATS\n");
    printf("=======================================================================\n\n");

    printf("  1. This data is SYNTHETIC (bench_compression.c generators).\n");
    printf("     Real NASDAQ ITCH data may have very different distributions.\n");
    printf("     k=3 for price was tuned for real data where predictions are\n");
    printf("     accurate (small residuals). Synthetic data has poor predictions\n");
    printf("     (no per-stock price tracking in generators => large residuals).\n\n");

    printf("  2. The adaptive k mechanism (residc_adaptive_k) adjusts k at runtime.\n");
    printf("     The defaults are just the starting point. With good warmup, the\n");
    printf("     adaptive k should converge to near-optimal values automatically.\n");
    printf("     Only TIMESTAMP and SEQUENTIAL_ID currently use adaptive k.\n\n");

    printf("  3. Price and Quantity k are NOT adaptive -- they use fixed regime-based\n");
    printf("     values. Making these adaptive could be more impactful than tuning\n");
    printf("     the static defaults.\n\n");

    printf("Done.\n");

    free(quotes); free(trades); free(orders); free(books);
    free(all_ts); free(all_pr); free(all_qt); free(all_sq);
    for (int i = 0; i < 4; i++) free_field_residuals(&results[i].fr);

    return 0;
}
