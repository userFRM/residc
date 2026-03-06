/* Benchmark: compare tiered vs exp-golomb coder — compression and latency */
#include "residc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N 100000
#define N_ITERS 10

typedef struct { uint64_t ts; uint16_t inst; uint32_t price; uint32_t qty; uint8_t side; } Quote;
typedef struct { uint64_t ts; uint16_t inst; uint32_t price; uint32_t qty; uint8_t side;
                 uint64_t trade_id; uint32_t bid; uint32_t ask; } Trade;
typedef struct { uint64_t ts; uint16_t inst; uint32_t price; uint32_t qty; uint8_t side;
                 uint8_t type; uint64_t order_id; uint16_t tif; uint64_t orig_id; uint32_t ref_price; } Order;
typedef struct { uint64_t ts; uint16_t inst; uint32_t bid; uint32_t ask;
                 uint32_t bid_sz; uint32_t ask_sz; uint8_t lvl; } Book;

static uint64_t rng = 12345678901ULL;
static uint64_t xrand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

/* Field order: {type, offset, size, ref_field} */
static const residc_field_t qf[] = {
    {RESIDC_TIMESTAMP,  offsetof(Quote,ts),    8, -1},
    {RESIDC_INSTRUMENT, offsetof(Quote,inst),  2, -1},
    {RESIDC_PRICE,      offsetof(Quote,price), 4, -1},
    {RESIDC_QUANTITY,   offsetof(Quote,qty),   4, -1},
    {RESIDC_BOOL,       offsetof(Quote,side),  1, -1}};
static const residc_schema_t qs = {qf, 5, sizeof(Quote)};

static const residc_field_t tf[] = {
    {RESIDC_TIMESTAMP,     offsetof(Trade,ts),       8, -1},
    {RESIDC_INSTRUMENT,    offsetof(Trade,inst),      2, -1},
    {RESIDC_PRICE,         offsetof(Trade,price),     4, -1},
    {RESIDC_QUANTITY,      offsetof(Trade,qty),       4, -1},
    {RESIDC_BOOL,          offsetof(Trade,side),      1, -1},
    {RESIDC_SEQUENTIAL_ID, offsetof(Trade,trade_id),  8, -1},
    {RESIDC_PRICE,         offsetof(Trade,bid),       4, -1},
    {RESIDC_PRICE,         offsetof(Trade,ask),       4, -1}};
static const residc_schema_t ts_schema = {tf, 8, sizeof(Trade)};

static const residc_field_t of[] = {
    {RESIDC_TIMESTAMP,     offsetof(Order,ts),        8, -1},
    {RESIDC_INSTRUMENT,    offsetof(Order,inst),       2, -1},
    {RESIDC_PRICE,         offsetof(Order,price),      4, -1},
    {RESIDC_QUANTITY,      offsetof(Order,qty),        4, -1},
    {RESIDC_BOOL,          offsetof(Order,side),       1, -1},
    {RESIDC_ENUM,          offsetof(Order,type),       1, -1},
    {RESIDC_SEQUENTIAL_ID, offsetof(Order,order_id),   8, -1},
    {RESIDC_ENUM,          offsetof(Order,tif),        2, -1},
    {RESIDC_DELTA_ID,      offsetof(Order,orig_id),    8,  6},
    {RESIDC_DELTA_PRICE,   offsetof(Order,ref_price),  4,  2}};
static const residc_schema_t os = {of, 10, sizeof(Order)};

static const residc_field_t bf[] = {
    {RESIDC_TIMESTAMP,  offsetof(Book,ts),     8, -1},
    {RESIDC_INSTRUMENT, offsetof(Book,inst),   2, -1},
    {RESIDC_PRICE,      offsetof(Book,bid),    4, -1},
    {RESIDC_PRICE,      offsetof(Book,ask),    4, -1},
    {RESIDC_QUANTITY,   offsetof(Book,bid_sz), 4, -1},
    {RESIDC_QUANTITY,   offsetof(Book,ask_sz), 4, -1},
    {RESIDC_RAW,        offsetof(Book,lvl),    1, -1}};
static const residc_schema_t bs = {bf, 7, sizeof(Book)};

typedef struct {
    const char *name;
    int raw_size;
    long total_compressed;
    double best_encode_ns;
    double best_decode_ns;
    int errors;
} Result;

static void bench_schema(const residc_schema_t *schema, const void *msgs, int n,
                          const char *name, int coder_id, Result *result)
{
    int msg_size = schema->msg_size;
    residc_state_t *enc = malloc(sizeof(residc_state_t));
    residc_state_t *dec = malloc(sizeof(residc_state_t));
    uint8_t buf[256];

    /* Pre-encode for decode benchmark + measure compression */
    int *lens = malloc(n * sizeof(int));
    uint8_t (*bufs)[256] = malloc(n * 256);
    residc_init(enc, schema);
    residc_set_coder(enc, coder_id);
    long total_compressed = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
        lens[i] = residc_encode(enc, msg, bufs[i], 256);
        total_compressed += lens[i];
    }

    /* Encode benchmark */
    double best_enc = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(enc, schema);
        residc_set_coder(enc, coder_id);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < n; i++) {
            const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
            residc_encode(enc, msg, buf, 256);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        double per = ns / n;
        if (per < best_enc) best_enc = per;
    }

    /* Decode benchmark */
    double best_dec = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(dec, schema);
        residc_set_coder(dec, coder_id);
        uint8_t decoded[256];
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < n; i++)
            residc_decode(dec, bufs[i], lens[i], decoded);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        double per = ns / n;
        if (per < best_dec) best_dec = per;
    }

    /* Roundtrip verification */
    residc_init(enc, schema);
    residc_init(dec, schema);
    residc_set_coder(enc, coder_id);
    residc_set_coder(dec, coder_id);
    int errors = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
        int len = residc_encode(enc, msg, buf, 256);
        uint8_t decoded[256];
        memset(decoded, 0, sizeof(decoded));
        residc_decode(dec, buf, len, decoded);
        if (memcmp(msg, decoded, msg_size) != 0) errors++;
    }

    result->name = name;
    result->raw_size = residc_raw_size(schema);
    result->total_compressed = total_compressed;
    result->best_encode_ns = best_enc;
    result->best_decode_ns = best_dec;
    result->errors = errors;

    free(enc); free(dec); free(lens); free(bufs);
}

int main(void)
{
    Quote *quotes = calloc(N, sizeof(Quote));
    Trade *trades = calloc(N, sizeof(Trade));
    Order *orders = calloc(N, sizeof(Order));
    Book *books = calloc(N, sizeof(Book));

    uint64_t ts = 34200000000000ULL;
    for (int i = 0; i < N; i++) {
        ts += 1000 + (xrand() % 5000);
        uint16_t inst = xrand() % 500;
        quotes[i] = (Quote){ts, inst, 100000 + (xrand()%50000)*100, (1+(xrand()%20))*100, xrand()&1};
        trades[i] = (Trade){ts, inst, 100000+(xrand()%50000)*100, (1+(xrand()%20))*100,
                            xrand()&1, 1000000+i, 100000+(xrand()%50000)*100, 100000+(xrand()%50000)*100};
        orders[i] = (Order){ts, inst, 100000+(xrand()%50000)*100, (1+(xrand()%20))*100,
                            xrand()&1, xrand()%4, 2000000+i, xrand()%6, 2000000+i-(xrand()%10),
                            100000+(xrand()%1000)*100};
        books[i] = (Book){ts, inst, 100000+(xrand()%50000)*100, 100000+(xrand()%50000)*100,
                          (1+(xrand()%50))*100, (1+(xrand()%50))*100, xrand()%10};
    }

    printf("Tiered vs Exp-Golomb comparison (%d msgs/type, best of %d iterations)\n", N, N_ITERS);
    printf("======================================================================\n\n");

    struct { const residc_schema_t *s; const void *d; const char *n; } types[] = {
        {&qs, quotes, "Quote"}, {&ts_schema, trades, "Trade"},
        {&os, orders, "Order"}, {&bs, books, "Book Update"}};

    printf("%-14s  %6s  %7s  %7s  %7s  %6s  |  %7s  %7s  %7s  %6s\n",
           "", "", "TIERED", "", "", "", "EXPGOLOMB", "", "", "");
    printf("%-14s  %6s  %7s  %7s  %7s  %6s  |  %7s  %7s  %7s  %6s\n",
           "Message", "Raw", "Compr.", "Ratio", "Enc", "Dec", "Compr.", "Ratio", "Enc", "Dec");
    printf("%-14s  %6s  %7s  %7s  %7s  %6s  |  %7s  %7s  %7s  %6s\n",
           "--------------", "------", "-------", "-------", "-------", "------",
           "-------", "-------", "-------", "------");

    int total_errors = 0;
    for (int t = 0; t < 4; t++) {
        Result tiered, expg;
        bench_schema(types[t].s, types[t].d, N, types[t].n, RESIDC_CODER_TIERED, &tiered);
        bench_schema(types[t].s, types[t].d, N, types[t].n, RESIDC_CODER_EXPGOLOMB, &expg);

        double t_avg = (double)tiered.total_compressed / N;
        double e_avg = (double)expg.total_compressed / N;
        double t_ratio = (double)tiered.raw_size / t_avg;
        double e_ratio = (double)expg.raw_size / e_avg;

        printf("%-14s  %4d B  %5.1f B  %5.2f:1  %4.0f ns  %4.0f ns  |  %5.1f B  %5.2f:1  %4.0f ns  %4.0f ns",
               tiered.name, tiered.raw_size,
               t_avg, t_ratio, tiered.best_encode_ns, tiered.best_decode_ns,
               e_avg, e_ratio, expg.best_encode_ns, expg.best_decode_ns);
        if (tiered.errors + expg.errors > 0)
            printf("  [%d+%d ERRORS]", tiered.errors, expg.errors);
        printf("\n");
        total_errors += tiered.errors + expg.errors;
    }

    printf("\nAll roundtrips: %s\n", total_errors == 0 ? "PASS (0 errors)" : "FAIL");

    free(quotes); free(trades); free(orders); free(books);
    return total_errors;
}
