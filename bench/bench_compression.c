/*
 * bench_compression.c — Compression ratio benchmark across message types
 *
 * Tests residc on synthetic and realistic data for common financial message types:
 *   - Quotes (5 fields): bid/ask updates
 *   - Trades (8 fields): execution reports
 *   - Orders (10 fields): new order single
 *   - Book updates (7 fields): L2 depth incremental
 *
 * Build:
 *   cc -O2 -march=native -o bench_compression bench/bench_compression.c core/residc.c -Icore
 *
 * Run:
 *   ./bench_compression
 */
#define _POSIX_C_SOURCE 199309L
#include "residc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define N_MSGS 100000
#define N_ITERS 10

/* ================================================================
 * Simple LCG for deterministic "random" data
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
 * Message: Quote (5 fields, 19 raw bytes)
 * Typical: bid/ask price updates from an exchange
 * ================================================================ */

typedef struct {
    uint64_t timestamp;        /* ns since midnight */
    uint16_t instrument_id;
    uint32_t price;            /* fixed-point, e.g. price * 10000 */
    uint32_t quantity;
    uint8_t  side;             /* 0=bid, 1=ask */
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

static void gen_quotes(Quote *msgs, int n) {
    uint64_t ts = 34200000000000ULL; /* 09:30:00.000 */
    for (int i = 0; i < n; i++) {
        ts += 500 + rng_range(0, 50000);  /* 0.5-50us gaps */
        msgs[i].timestamp = ts;
        msgs[i].instrument_id = (uint16_t)rng_range(0, 49);  /* 50 instruments */
        /* prices cluster around 150.00 with small movements */
        msgs[i].price = 1500000 + (uint32_t)(rng_range(0, 2000)) - 1000;
        msgs[i].quantity = (uint32_t)(rng_range(1, 20) * 100);  /* round lots */
        msgs[i].side = (uint8_t)(rng_next() & 1);
    }
}

/* ================================================================
 * Message: Trade (8 fields, 33 raw bytes)
 * Typical: execution report / trade confirmation
 * ================================================================ */

typedef struct {
    uint64_t timestamp;
    uint16_t instrument_id;
    uint32_t price;
    uint32_t quantity;
    uint64_t trade_id;          /* monotonically increasing */
    uint32_t buyer_id;          /* categorical: firm/account */
    uint32_t seller_id;         /* categorical: firm/account */
    uint8_t  aggressor_side;    /* 0=buy, 1=sell */
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

static void gen_trades(Trade *msgs, int n) {
    uint64_t ts = 34200000000000ULL;
    uint64_t tid = 1000000;
    /* 20 firms, Zipf-ish distribution */
    uint32_t firms[] = {
        1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010,
        2001, 2002, 2003, 2004, 2005, 3001, 3002, 3003, 4001, 5001
    };
    for (int i = 0; i < n; i++) {
        ts += 2000 + rng_range(0, 200000);  /* 2-200us gaps */
        tid += 1 + rng_range(0, 3);          /* small increments */
        msgs[i].timestamp = ts;
        msgs[i].instrument_id = (uint16_t)rng_range(0, 49);
        msgs[i].price = 1500000 + (uint32_t)(rng_range(0, 2000)) - 1000;
        msgs[i].quantity = (uint32_t)(rng_range(1, 50) * 100);
        msgs[i].trade_id = tid;
        /* top 5 firms get ~60% of trades */
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
 * Message: Order (10 fields, 39 raw bytes)
 * Typical: new order single / order entry
 * ================================================================ */

typedef struct {
    uint64_t timestamp;
    uint16_t instrument_id;
    uint32_t price;
    uint32_t quantity;
    uint64_t order_id;          /* monotonically increasing */
    uint32_t account_id;        /* categorical */
    uint8_t  side;              /* 0=buy, 1=sell */
    uint8_t  order_type;        /* 0=limit, 1=market, 2=stop, 3=stop-limit */
    uint8_t  time_in_force;     /* 0=day, 1=GTC, 2=IOC, 3=FOK */
    uint8_t  flags;             /* bit flags */
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
        /* top 3 accounts get 70% of orders */
        uint32_t r = rng_range(0, 99);
        int idx = (r < 70) ? (int)rng_range(0, 2) : (int)rng_range(3, 9);
        msgs[i].account_id = accounts[idx];
        msgs[i].side = (uint8_t)(rng_next() & 1);
        /* 80% limit, 15% market, 5% stop/stop-limit */
        r = rng_range(0, 99);
        msgs[i].order_type = (r < 80) ? 0 : (r < 95) ? 1 : (uint8_t)rng_range(2, 3);
        /* 60% day, 25% GTC, 10% IOC, 5% FOK */
        r = rng_range(0, 99);
        msgs[i].time_in_force = (r < 60) ? 0 : (r < 85) ? 1 : (r < 95) ? 2 : 3;
        msgs[i].flags = 0;
    }
}

/* ================================================================
 * Message: Book Update (7 fields, 25 raw bytes)
 * Typical: L2 order book depth update / market data incremental
 * ================================================================ */

typedef struct {
    uint64_t timestamp;
    uint16_t instrument_id;
    uint32_t price;             /* price level */
    uint32_t quantity;          /* new quantity at level */
    uint8_t  side;              /* 0=bid, 1=ask */
    uint8_t  action;            /* 0=add, 1=change, 2=delete */
    uint8_t  level;             /* depth level 0-9 */
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

static void gen_book_updates(BookUpdate *msgs, int n) {
    uint64_t ts = 34200000000000ULL;
    for (int i = 0; i < n; i++) {
        ts += 100 + rng_range(0, 10000);  /* very fast: 0.1-10us */
        msgs[i].timestamp = ts;
        msgs[i].instrument_id = (uint16_t)rng_range(0, 49);
        msgs[i].price = 1500000 + (uint32_t)(rng_range(0, 1000)) - 500;
        msgs[i].quantity = (uint32_t)(rng_range(0, 200) * 100);
        msgs[i].side = (uint8_t)(rng_next() & 1);
        /* 30% add, 50% change, 20% delete */
        uint32_t r = rng_range(0, 99);
        msgs[i].action = (r < 30) ? 0 : (r < 80) ? 1 : 2;
        msgs[i].level = (uint8_t)rng_range(0, 9);
    }
}

/* ================================================================
 * Realistic data generators
 *
 * These model real market data patterns:
 *   - Zipf instrument distribution (top 5 get ~60% of traffic)
 *   - Price random walks (small tick increments, occasional jumps)
 *   - Quantity clustering (60% round lots, 30% x10, 10% odd)
 *   - Timestamp bursts (clusters of fast messages with gaps)
 *   - Instrument locality (consecutive msgs for same stock)
 * ================================================================ */

/* Zipf-like instrument selection: top instruments get most traffic */
static uint16_t zipf_instrument(int n_instruments) {
    uint32_t r = rng_range(0, 99);
    if (r < 60) return (uint16_t)rng_range(0, 4);           /* top 5: 60% */
    if (r < 85) return (uint16_t)rng_range(5, 14);          /* next 10: 25% */
    return (uint16_t)rng_range(15, (uint32_t)(n_instruments - 1)); /* rest: 15% */
}

/* Instrument with locality: 30% same-as-last, 10% same-as-2-back */
static uint16_t instrument_with_locality(uint16_t last, uint16_t second_last,
                                         int n_instruments, int msg_count) {
    if (msg_count > 0) {
        uint32_t r = rng_range(0, 99);
        if (r < 30) return last;          /* 30% repeat */
        if (r < 40) return second_last;   /* 10% alternation */
    }
    return zipf_instrument(n_instruments);
}

/* Realistic quantity: 60% round lots, 30% x10, 10% odd */
static uint32_t realistic_quantity(void) {
    uint32_t r = rng_range(0, 99);
    if (r < 60) {
        /* Round lots: heavily clustered at 100, 200, 500, 1000 */
        uint32_t r2 = rng_range(0, 99);
        if (r2 < 40) return 100;
        if (r2 < 60) return 200;
        if (r2 < 75) return 500;
        if (r2 < 85) return 1000;
        return rng_range(1, 50) * 100;
    }
    if (r < 90) {
        /* Multiples of 10 */
        return rng_range(1, 200) * 10;
    }
    /* Odd lots */
    return rng_range(1, 500);
}

/* Bursty timestamp: 90% in-burst (50-200ns gap), 10% between bursts (1-50us) */
static uint64_t bursty_timestamp(uint64_t last_ts) {
    uint32_t r = rng_range(0, 99);
    if (r < 90) {
        return last_ts + 50 + rng_range(0, 150);     /* in-burst: 50-200ns */
    }
    return last_ts + 1000 + rng_range(0, 49000);      /* gap: 1-50us */
}

static void gen_quotes_realistic(Quote *msgs, int n) {
    uint64_t ts = 34200000000000ULL;
    /* Per-instrument price state (random walk) */
    uint32_t prices[50];
    for (int i = 0; i < 50; i++)
        prices[i] = 1000000 + rng_range(0, 2000000); /* $100-$300 */

    uint16_t last_inst = 0, second_last_inst = 0;
    for (int i = 0; i < n; i++) {
        ts = bursty_timestamp(ts);
        uint16_t inst = instrument_with_locality(last_inst, second_last_inst, 50, i);

        /* Random walk: +/- 1-3 ticks (1 tick = 100 = $0.01) */
        int32_t delta = (int32_t)rng_range(0, 6) - 3; /* -3 to +3 */
        /* Occasional large move (1% chance) */
        if (rng_range(0, 99) == 0)
            delta = (int32_t)rng_range(0, 200) - 100;  /* -100 to +100 ticks */
        prices[inst] = (uint32_t)((int64_t)prices[inst] + delta * 100);

        msgs[i].timestamp = ts;
        msgs[i].instrument_id = inst;
        msgs[i].price = prices[inst];
        msgs[i].quantity = realistic_quantity();
        msgs[i].side = (uint8_t)(rng_next() & 1);

        second_last_inst = last_inst;
        last_inst = inst;
    }
}

static void gen_trades_realistic(Trade *msgs, int n) {
    uint64_t ts = 34200000000000ULL;
    uint64_t tid = 1000000;
    uint32_t firms[] = {
        1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010,
        2001, 2002, 2003, 2004, 2005, 3001, 3002, 3003, 4001, 5001
    };
    uint32_t prices[50];
    for (int i = 0; i < 50; i++)
        prices[i] = 1000000 + rng_range(0, 2000000);

    uint16_t last_inst = 0, second_last_inst = 0;
    for (int i = 0; i < n; i++) {
        ts = bursty_timestamp(ts);
        /* Trades: small monotonic increments, 80% +1, 15% +2, 5% +3 */
        uint32_t r = rng_range(0, 99);
        tid += (r < 80) ? 1 : (r < 95) ? 2 : 3;

        uint16_t inst = instrument_with_locality(last_inst, second_last_inst, 50, i);

        int32_t delta = (int32_t)rng_range(0, 6) - 3;
        if (rng_range(0, 99) == 0)
            delta = (int32_t)rng_range(0, 200) - 100;
        prices[inst] = (uint32_t)((int64_t)prices[inst] + delta * 100);

        msgs[i].timestamp = ts;
        msgs[i].instrument_id = inst;
        msgs[i].price = prices[inst];
        msgs[i].quantity = realistic_quantity();
        msgs[i].trade_id = tid;
        /* Zipf firm distribution */
        r = rng_range(0, 99);
        int bi = (r < 60) ? (int)rng_range(0, 4) : (int)rng_range(5, 19);
        r = rng_range(0, 99);
        int si = (r < 60) ? (int)rng_range(0, 4) : (int)rng_range(5, 19);
        msgs[i].buyer_id = firms[bi];
        msgs[i].seller_id = firms[si];
        msgs[i].aggressor_side = (uint8_t)(rng_next() & 1);

        second_last_inst = last_inst;
        last_inst = inst;
    }
}

static void gen_orders_realistic(Order *msgs, int n) {
    uint64_t ts = 34200000000000ULL;
    uint64_t oid = 5000000;
    uint32_t accounts[] = { 100, 101, 102, 103, 104, 200, 201, 300, 400, 500 };
    uint32_t prices[50];
    for (int i = 0; i < 50; i++)
        prices[i] = 1000000 + rng_range(0, 2000000);

    uint16_t last_inst = 0, second_last_inst = 0;
    for (int i = 0; i < n; i++) {
        ts = bursty_timestamp(ts);
        oid += 1;
        uint16_t inst = instrument_with_locality(last_inst, second_last_inst, 50, i);

        int32_t delta = (int32_t)rng_range(0, 6) - 3;
        if (rng_range(0, 99) == 0)
            delta = (int32_t)rng_range(0, 200) - 100;
        prices[inst] = (uint32_t)((int64_t)prices[inst] + delta * 100);

        msgs[i].timestamp = ts;
        msgs[i].instrument_id = inst;
        msgs[i].price = prices[inst];
        msgs[i].quantity = realistic_quantity();
        msgs[i].order_id = oid;
        /* Top 3 accounts: 70% */
        uint32_t r = rng_range(0, 99);
        int idx = (r < 70) ? (int)rng_range(0, 2) : (int)rng_range(3, 9);
        msgs[i].account_id = accounts[idx];
        msgs[i].side = (uint8_t)(rng_next() & 1);
        r = rng_range(0, 99);
        msgs[i].order_type = (r < 80) ? 0 : (r < 95) ? 1 : (uint8_t)rng_range(2, 3);
        r = rng_range(0, 99);
        msgs[i].time_in_force = (r < 60) ? 0 : (r < 85) ? 1 : (r < 95) ? 2 : 3;
        msgs[i].flags = 0;

        second_last_inst = last_inst;
        last_inst = inst;
    }
}

static void gen_book_updates_realistic(BookUpdate *msgs, int n) {
    uint64_t ts = 34200000000000ULL;
    uint32_t prices[50];
    for (int i = 0; i < 50; i++)
        prices[i] = 1000000 + rng_range(0, 2000000);

    uint16_t last_inst = 0, second_last_inst = 0;
    for (int i = 0; i < n; i++) {
        ts = bursty_timestamp(ts);
        uint16_t inst = instrument_with_locality(last_inst, second_last_inst, 50, i);

        int32_t delta = (int32_t)rng_range(0, 4) - 2; /* tighter spread for book */
        prices[inst] = (uint32_t)((int64_t)prices[inst] + delta * 100);

        msgs[i].timestamp = ts;
        msgs[i].instrument_id = inst;
        msgs[i].price = prices[inst];
        msgs[i].quantity = realistic_quantity();
        msgs[i].side = (uint8_t)(rng_next() & 1);
        uint32_t r = rng_range(0, 99);
        msgs[i].action = (r < 30) ? 0 : (r < 80) ? 1 : 2;
        msgs[i].level = (uint8_t)rng_range(0, 9);

        second_last_inst = last_inst;
        last_inst = inst;
    }
}

/* ================================================================
 * Benchmark runner
 * ================================================================ */

typedef struct {
    const char *name;
    const char *description;
    int raw_size;
    int n_fields;
    long total_compressed;
    double best_encode_ns;
    double best_decode_ns;
    int errors;
} BenchResult;

static void bench_schema(
    const residc_schema_t *schema, const void *msgs, int n,
    const char *name, const char *desc, BenchResult *result)
{
    int raw_size = residc_raw_size(schema);
    int msg_size = schema->msg_size;
    residc_state_t enc, dec;
    uint8_t buf[128];

    /* Pre-encode for decode benchmark + measure compression */
    int *lens = malloc(n * sizeof(int));
    uint8_t (*bufs)[128] = malloc(n * 128);
    residc_init(&enc, schema);
    long total_compressed = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
        lens[i] = residc_encode(&enc, msg, bufs[i], 128);
        total_compressed += lens[i];
    }

    /* Encode benchmark */
    double best_enc = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(&enc, schema);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < n; i++) {
            const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
            residc_encode(&enc, msg, buf, 128);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        double ns_per = elapsed / n;
        if (ns_per < best_enc) best_enc = ns_per;
    }

    /* Decode benchmark */
    double best_dec = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(&dec, schema);
        uint8_t decoded_msg[256];
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < n; i++) {
            residc_decode(&dec, bufs[i], lens[i], decoded_msg);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        double ns_per = elapsed / n;
        if (ns_per < best_dec) best_dec = ns_per;
    }

    /* Verify roundtrip */
    residc_init(&enc, schema);
    residc_init(&dec, schema);
    int errors = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
        int len = residc_encode(&enc, msg, buf, 128);
        uint8_t decoded_msg[256];
        memset(decoded_msg, 0, sizeof(decoded_msg));
        residc_decode(&dec, buf, len, decoded_msg);
        if (memcmp(msg, decoded_msg, msg_size) != 0) errors++;
    }

    result->name = name;
    result->description = desc;
    result->raw_size = raw_size;
    result->n_fields = schema->num_fields;
    result->total_compressed = total_compressed;
    result->best_encode_ns = best_enc;
    result->best_decode_ns = best_dec;
    result->errors = errors;

    free(lens);
    free(bufs);
}

static void print_results(const char *label __attribute__((unused)),
                          BenchResult *results, int n_types,
                          int *total_errors)
{
    printf("%-14s  %6s  %10s  %7s  %7s  %7s  %6s\n",
           "Message", "Fields", "Raw", "Compr.", "Ratio", "Enc", "Dec");
    printf("%-14s  %6s  %10s  %7s  %7s  %7s  %6s\n",
           "--------------", "------", "----------", "-------", "-------", "-------", "------");

    for (int i = 0; i < n_types; i++) {
        BenchResult *r = &results[i];
        double avg_compressed = (double)r->total_compressed / N_MSGS;
        double ratio = (double)r->raw_size / avg_compressed;
        printf("%-14s  %6d  %7d B    %5.1f B  %5.2f:1  %4.0f ns  %4.0f ns",
               r->name, r->n_fields, r->raw_size, avg_compressed, ratio,
               r->best_encode_ns, r->best_decode_ns);
        if (r->errors > 0) printf("  [%d ERRORS]", r->errors);
        printf("\n");
        *total_errors += r->errors;
    }
}

int main(void)
{
    Quote *quotes = calloc(N_MSGS, sizeof(Quote));
    Trade *trades = calloc(N_MSGS, sizeof(Trade));
    Order *orders = calloc(N_MSGS, sizeof(Order));
    BookUpdate *books = calloc(N_MSGS, sizeof(BookUpdate));
    BenchResult results[4];
    int total_errors = 0;

    printf("residc compression benchmark (%d messages per type, best of %d iterations)\n", N_MSGS, N_ITERS);
    printf("================================================================================\n");

    /* ---- Synthetic (uniform random) data ---- */
    rng_state = 12345678901ULL;
    gen_quotes(quotes, N_MSGS);
    gen_trades(trades, N_MSGS);
    gen_orders(orders, N_MSGS);
    gen_book_updates(books, N_MSGS);

    bench_schema(&quote_schema, quotes, N_MSGS,
        "Quote", "bid/ask updates (5 fields)", &results[0]);
    bench_schema(&trade_schema, trades, N_MSGS,
        "Trade", "execution reports (8 fields)", &results[1]);
    bench_schema(&order_schema, orders, N_MSGS,
        "Order", "new order single (10 fields)", &results[2]);
    bench_schema(&book_schema, books, N_MSGS,
        "Book Update", "L2 depth incremental (7 fields)", &results[3]);

    printf("\n--- SYNTHETIC data (uniform random instruments/prices/quantities) ---\n\n");
    print_results("Synthetic", results, 4, &total_errors);

    /* ---- Realistic market data ---- */
    rng_state = 98765432109ULL;  /* different seed for realistic data */
    gen_quotes_realistic(quotes, N_MSGS);
    gen_trades_realistic(trades, N_MSGS);
    gen_orders_realistic(orders, N_MSGS);
    gen_book_updates_realistic(books, N_MSGS);

    bench_schema(&quote_schema, quotes, N_MSGS,
        "Quote", "bid/ask updates (5 fields)", &results[0]);
    bench_schema(&trade_schema, trades, N_MSGS,
        "Trade", "execution reports (8 fields)", &results[1]);
    bench_schema(&order_schema, orders, N_MSGS,
        "Order", "new order single (10 fields)", &results[2]);
    bench_schema(&book_schema, books, N_MSGS,
        "Book Update", "L2 depth incremental (7 fields)", &results[3]);

    printf("\n--- REALISTIC data (Zipf instruments, price walks, qty clustering, bursty ts) ---\n\n");
    print_results("Realistic", results, 4, &total_errors);

    printf("\nAll roundtrips: %s\n", total_errors == 0 ? "PASS (0 errors)" : "FAIL");

    free(quotes);
    free(trades);
    free(orders);
    free(books);

    return total_errors;
}
