/*
 * bench_quote.c — Latency benchmark for residc encode/decode
 *
 * Build:
 *   cc -O2 -march=native -o bench_quote bench_quote.c ../core/residc.c -I../core
 *
 * Run:
 *   ./bench_quote
 */
#define _POSIX_C_SOURCE 199309L
#include "residc.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

typedef struct {
    uint64_t timestamp;
    uint16_t instrument_id;
    uint32_t price;
    uint32_t quantity;
    uint8_t  side;
} Quote;

static const residc_field_t fields[] = {
    { RESIDC_TIMESTAMP,  offsetof(Quote, timestamp),     8, -1 },
    { RESIDC_INSTRUMENT, offsetof(Quote, instrument_id), 2, -1 },
    { RESIDC_PRICE,      offsetof(Quote, price),         4, -1 },
    { RESIDC_QUANTITY,   offsetof(Quote, quantity),       4, -1 },
    { RESIDC_BOOL,       offsetof(Quote, side),           1, -1 },
};

static const residc_schema_t schema = {
    .fields = fields, .num_fields = 5, .msg_size = sizeof(Quote),
};

#define N_MSGS 100000
#define N_ITERS 10

int main(void)
{
    residc_state_t enc, dec;
    uint8_t buf[64];
    int encoded_lens[N_MSGS];
    uint8_t encoded_bufs[N_MSGS][64];
    Quote msgs[N_MSGS];
    int raw_size = residc_raw_size(&schema);

    /* Generate messages */
    uint64_t ts = 34200000000000ULL;
    for (int i = 0; i < N_MSGS; i++) {
        ts += 1000 + ((uint64_t)i * 37 % 50000);
        msgs[i].timestamp = ts;
        msgs[i].instrument_id = (uint16_t)(i % 50);
        msgs[i].price = 1500000 + (uint32_t)((i * 7) % 2000);
        msgs[i].quantity = (uint32_t)((1 + i % 20) * 100);
        msgs[i].side = (uint8_t)(i % 2);
    }

    /* Warm up + pre-encode for decode benchmark */
    residc_init(&enc, &schema);
    long total_compressed = 0;
    for (int i = 0; i < N_MSGS; i++) {
        encoded_lens[i] = residc_encode(&enc, &msgs[i], encoded_bufs[i], 64);
        total_compressed += encoded_lens[i];
    }

    /* Encode benchmark */
    double best_enc = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(&enc, &schema);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < N_MSGS; i++) {
            residc_encode(&enc, &msgs[i], buf, 64);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        double ns_per_msg = elapsed / N_MSGS;
        if (ns_per_msg < best_enc) best_enc = ns_per_msg;
    }

    /* Decode benchmark */
    double best_dec = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(&dec, &schema);
        Quote decoded;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < N_MSGS; i++) {
            residc_decode(&dec, encoded_bufs[i], encoded_lens[i], &decoded);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        double ns_per_msg = elapsed / N_MSGS;
        if (ns_per_msg < best_dec) best_dec = ns_per_msg;
    }

    /* Verify roundtrip */
    residc_init(&enc, &schema);
    residc_init(&dec, &schema);
    int errors = 0;
    for (int i = 0; i < N_MSGS; i++) {
        int len = residc_encode(&enc, &msgs[i], buf, 64);
        Quote decoded;
        residc_decode(&dec, buf, len, &decoded);
        if (memcmp(&msgs[i], &decoded, sizeof(Quote)) != 0) errors++;
    }

    printf("residc C benchmark (5-field quote, %d messages, best of %d iterations)\n", N_MSGS, N_ITERS);
    printf("=================================================================\n");
    printf("  Encode:      %.0f ns/msg\n", best_enc);
    printf("  Decode:      %.0f ns/msg\n", best_dec);
    printf("  Ratio:       %.2f:1 (%d -> %.1f bytes avg)\n",
           (double)(N_MSGS * raw_size) / total_compressed,
           raw_size, (double)total_compressed / N_MSGS);
    printf("  Errors:      %d\n", errors);

    return errors;
}
