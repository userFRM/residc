/*
 * experiment_branchless.c — Branchless tier selection experiment
 *
 * Compares the current if/else tier selection in encode_residual / decode_residual
 * against a CLZ-based branchless encoder and a 4-bit-peek LUT decoder.
 *
 * Build:
 *   cc -O2 -march=native -o experiment_branchless bench/experiment_branchless.c core/residc.c -Icore
 *
 * Run:
 *   ./experiment_branchless
 */
#define _POSIX_C_SOURCE 199309L
#include "residc.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

/* ================================================================
 * Branchless tier encoder using __builtin_clzll
 * ================================================================
 *
 * Tier boundaries (from residc.c):
 *   tier 0: zz < 2^k
 *   tier 1: zz < 2^(k+2)
 *   tier 2: zz < 2^(k+5)
 *   tier 3: zz < 2^(k+10)
 *   tier 4: everything else (raw 64 bits)
 *
 * Instead of 4 chained if/else comparisons, we compute bits_needed
 * from CLZ and map to a tier via a small lookup table.
 */

/* Prefix values:  tier0=0(1bit), tier1=10(2bit), tier2=110(3bit),
 *                 tier3=1110(4bit), tier4=1111(4bit) */
static const uint8_t tier_prefix_val[]  = {0, 2, 6, 14, 15};
static const uint8_t tier_prefix_bits[] = {1, 2, 3,  4,  4};
static const int8_t  tier_payload_add[] = {0, 2, 5, 10,  0};

/*
 * Map bits_needed (0..64) to tier for a given k.
 * Boundaries: k, k+2, k+5, k+10.
 *
 * We build a 65-entry LUT per k value. Since k is small (typically 3-10),
 * we just do the comparison chain but avoid the zigzag recomputation
 * and the multiple bw_write calls by combining prefix+payload.
 *
 * Actually, the real win is computing tier from bits_needed using
 * a branchless cmov chain. GCC/Clang will lower this to cmov
 * instructions when we write it as a series of conditional increments.
 */
static inline __attribute__((always_inline))
int bits_needed(uint64_t zz)
{
    return zz ? (64 - __builtin_clzll(zz)) : 0;
}

static inline __attribute__((always_inline))
int compute_tier(int bn, int k)
{
    /* Branchless tier selection via conditional increment.
     * The compiler will use cmov for each of these. */
    int tier = 0;
    tier += (bn > k);        /* tier >= 1 if bits_needed > k */
    tier += (bn > k + 2);    /* tier >= 2 if bits_needed > k+2 */
    tier += (bn > k + 5);    /* tier >= 3 if bits_needed > k+5 */
    tier += (bn > k + 10);   /* tier >= 4 if bits_needed > k+10 */
    return tier;
}

static inline __attribute__((always_inline))
void encode_residual_branchless(residc_bitwriter_t *bw, int64_t value, int k)
{
    uint64_t zz = residc_zigzag_enc(value);
    int bn = bits_needed(zz);
    int tier = compute_tier(bn, k);

    residc_bw_write(bw, tier_prefix_val[tier], tier_prefix_bits[tier]);

    if (__builtin_expect(tier < 4, 1)) {
        residc_bw_write(bw, zz, k + tier_payload_add[tier]);
    } else {
        residc_bw_write(bw, (uint64_t)value >> 32, 32);
        residc_bw_write(bw, (uint64_t)value & 0xFFFFFFFF, 32);
    }
}

/* ================================================================
 * Branchless tier decoder using 4-bit peek + LUT
 * ================================================================
 *
 * Instead of reading 1 bit at a time with 4 sequential branches,
 * peek 4 bits and use a 16-entry LUT to determine the tier and
 * how many prefix bits were consumed.
 *
 * Prefix patterns (MSB-first):
 *   0xxx -> tier 0 (1 prefix bit)
 *   10xx -> tier 1 (2 prefix bits)
 *   110x -> tier 2 (3 prefix bits)
 *   1110 -> tier 3 (4 prefix bits)
 *   1111 -> tier 4 (4 prefix bits)
 */

/* LUT indexed by 4-bit peek value */
static const struct {
    uint8_t tier;
    uint8_t prefix_bits;
} decode_lut[16] = {
    /* 0000 */ {0, 1}, /* 0001 */ {0, 1}, /* 0010 */ {0, 1}, /* 0011 */ {0, 1},
    /* 0100 */ {0, 1}, /* 0101 */ {0, 1}, /* 0110 */ {0, 1}, /* 0111 */ {0, 1},
    /* 1000 */ {1, 2}, /* 1001 */ {1, 2}, /* 1010 */ {1, 2}, /* 1011 */ {1, 2},
    /* 1100 */ {2, 3}, /* 1101 */ {2, 3},
    /* 1110 */ {3, 4},
    /* 1111 */ {4, 4},
};

static inline __attribute__((always_inline))
int64_t decode_residual_branchless(residc_bitreader_t *br, int k)
{
    /* Peek 4 bits without consuming them */
    if (__builtin_expect(br->count < 4, 0)) {
        /* Refill */
        while (br->count <= 56 && br->byte_pos < br->len_bytes) {
            br->accum = (br->accum << 8) | br->data[br->byte_pos++];
            br->count += 8;
        }
    }

    int peek = (int)((br->accum >> (br->count - 4)) & 0xF);
    int tier = decode_lut[peek].tier;
    int prefix_consumed = decode_lut[peek].prefix_bits;

    /* Consume the prefix bits */
    br->count -= prefix_consumed;

    if (__builtin_expect(tier < 4, 1)) {
        int payload_bits = k + tier_payload_add[tier];
        uint64_t zz = residc_br_read(br, payload_bits);
        return residc_zigzag_dec(zz);
    } else {
        uint64_t hi = residc_br_read(br, 32);
        uint64_t lo = residc_br_read(br, 32);
        return (int64_t)((hi << 32) | lo);
    }
}

/* ================================================================
 * Benchmark infrastructure
 * ================================================================ */

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

#define N_MSGS  100000
#define N_ITERS 20

/* Generate synthetic messages */
static void gen_messages(Quote *msgs, int n)
{
    uint64_t ts = 34200000000000ULL;
    for (int i = 0; i < n; i++) {
        ts += 1000 + ((uint64_t)i * 37 % 50000);
        msgs[i].timestamp = ts;
        msgs[i].instrument_id = (uint16_t)(i % 50);
        msgs[i].price = 1500000 + (uint32_t)((i * 7) % 2000);
        msgs[i].quantity = (uint32_t)((1 + i % 20) * 100);
        msgs[i].side = (uint8_t)(i % 2);
    }
}

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

/* ================================================================
 * Isolated residual encode/decode benchmarks
 *
 * These benchmark ONLY the residual coding, not the full message
 * encode/decode path, to isolate the tier selection improvement.
 * ================================================================ */

/* Generate realistic residual values: mostly small, some medium, rare large */
static void gen_residuals(int64_t *residuals, int n)
{
    uint32_t seed = 12345;
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245 + 12345;
        uint32_t r = seed >> 16;
        if (r < 40000) {
            /* ~61%: tier 0 range for k=3: |val| < 4 */
            residuals[i] = (int64_t)(r % 7) - 3;
        } else if (r < 55000) {
            /* ~23%: tier 1 range: |val| < 16 */
            residuals[i] = (int64_t)(r % 31) - 15;
        } else if (r < 62000) {
            /* ~11%: tier 2 range: |val| < 128 */
            residuals[i] = (int64_t)(r % 255) - 127;
        } else if (r < 65000) {
            /* ~5%: tier 3 range: |val| < 4096 */
            residuals[i] = (int64_t)(r % 8191) - 4095;
        } else {
            /* ~0.5%: tier 4 range */
            residuals[i] = (int64_t)seed * 123456789LL;
        }
    }
}

static void bench_residual_encode(int64_t *residuals, int n, int k)
{
    residc_bitwriter_t bw;
    double best_orig = 1e18, best_new = 1e18;

    printf("\n--- Isolated residual encode benchmark (k=%d, N=%d) ---\n", k, n);

    /* Original */
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_bw_init(&bw);
        double t0 = now_ns();
        for (int i = 0; i < n; i++) {
            residc_encode_residual(&bw, residuals[i], k);
            if (__builtin_expect(bw.byte_pos > RESIDC_SCRATCH_BYTES - 32, 0))
                residc_bw_init(&bw);  /* reset to avoid overflow */
        }
        double t1 = now_ns();
        double ns = (t1 - t0) / n;
        if (ns < best_orig) best_orig = ns;
    }

    /* Branchless */
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_bw_init(&bw);
        double t0 = now_ns();
        for (int i = 0; i < n; i++) {
            encode_residual_branchless(&bw, residuals[i], k);
            if (__builtin_expect(bw.byte_pos > RESIDC_SCRATCH_BYTES - 32, 0))
                residc_bw_init(&bw);
        }
        double t1 = now_ns();
        double ns = (t1 - t0) / n;
        if (ns < best_new) best_new = ns;
    }

    double speedup = best_orig / best_new;
    printf("  Original encode:   %.1f ns/residual\n", best_orig);
    printf("  Branchless encode: %.1f ns/residual\n", best_new);
    printf("  Speedup:           %.2fx %s\n", speedup,
           speedup > 1.0 ? "(branchless FASTER)" : "(original faster)");
}

static void bench_residual_decode(int64_t *residuals, int n, int k)
{
    residc_bitwriter_t bw;
    residc_bitreader_t br;
    double best_orig = 1e18, best_new = 1e18;

    printf("\n--- Isolated residual decode benchmark (k=%d, N=%d) ---\n", k, n);

    /* Pre-encode all residuals with the original encoder */
    /* We need to encode in chunks since buffer is limited */
    #define CHUNK 24  /* conservative: worst case tier4 = 68 bits ~= 9 bytes */
    uint8_t encoded_chunks[N_MSGS][16];
    int encoded_lens[N_MSGS];
    int actual_n = n;

    for (int i = 0; i < actual_n; i++) {
        residc_bw_init(&bw);
        residc_encode_residual(&bw, residuals[i], k);
        encoded_lens[i] = residc_bw_finish(&bw);
        memcpy(encoded_chunks[i], bw.buf, encoded_lens[i]);
    }

    /* Original decode */
    for (int iter = 0; iter < N_ITERS; iter++) {
        double t0 = now_ns();
        volatile int64_t sink = 0;
        for (int i = 0; i < actual_n; i++) {
            residc_br_init(&br, encoded_chunks[i], encoded_lens[i]);
            sink += residc_decode_residual(&br, k);
        }
        double t1 = now_ns();
        double ns = (t1 - t0) / actual_n;
        if (ns < best_orig) best_orig = ns;
    }

    /* Branchless decode */
    for (int iter = 0; iter < N_ITERS; iter++) {
        double t0 = now_ns();
        volatile int64_t sink = 0;
        for (int i = 0; i < actual_n; i++) {
            residc_br_init(&br, encoded_chunks[i], encoded_lens[i]);
            sink += decode_residual_branchless(&br, k);
        }
        double t1 = now_ns();
        double ns = (t1 - t0) / actual_n;
        if (ns < best_new) best_new = ns;
    }

    double speedup = best_orig / best_new;
    printf("  Original decode:   %.1f ns/residual\n", best_orig);
    printf("  Branchless decode: %.1f ns/residual\n", best_new);
    printf("  Speedup:           %.2fx %s\n", speedup,
           speedup > 1.0 ? "(branchless FASTER)" : "(original faster)");
}

/* ================================================================
 * Full message encode/decode benchmark
 * ================================================================ */

static void bench_full_message(Quote *msgs, int n)
{
    residc_state_t enc, dec;
    uint8_t buf[64];
    uint8_t encoded_bufs[N_MSGS][64];
    int encoded_lens[N_MSGS];
    int raw_size = residc_raw_size(&schema);

    printf("\n--- Full message benchmark (%d messages, best of %d) ---\n", n, N_ITERS);

    /* Pre-encode for decode benchmark */
    residc_init(&enc, &schema);
    long total_compressed = 0;
    for (int i = 0; i < n; i++) {
        encoded_lens[i] = residc_encode(&enc, &msgs[i], encoded_bufs[i], 64);
        total_compressed += encoded_lens[i];
    }

    /* Encode benchmark (baseline — uses original code path) */
    double best_enc = 1e18;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(&enc, &schema);
        double t0 = now_ns();
        for (int i = 0; i < n; i++)
            residc_encode(&enc, &msgs[i], buf, 64);
        double t1 = now_ns();
        double ns = (t1 - t0) / n;
        if (ns < best_enc) best_enc = ns;
    }

    /* Decode benchmark (baseline) */
    double best_dec = 1e18;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(&dec, &schema);
        Quote decoded;
        double t0 = now_ns();
        for (int i = 0; i < n; i++)
            residc_decode(&dec, encoded_bufs[i], encoded_lens[i], &decoded);
        double t1 = now_ns();
        double ns = (t1 - t0) / n;
        if (ns < best_dec) best_dec = ns;
    }

    printf("  Encode:  %.0f ns/msg\n", best_enc);
    printf("  Decode:  %.0f ns/msg\n", best_dec);
    printf("  Ratio:   %.2f:1 (%d -> %.1f bytes avg)\n",
           (double)(n * raw_size) / total_compressed,
           raw_size, (double)total_compressed / n);
}

/* ================================================================
 * Correctness verification
 * ================================================================ */

static int verify_roundtrip(int64_t *residuals, int n, int k)
{
    residc_bitwriter_t bw;
    residc_bitreader_t br;
    int errors = 0;

    printf("\n--- Roundtrip verification (k=%d, N=%d) ---\n", k, n);

    for (int i = 0; i < n; i++) {
        int64_t val = residuals[i];

        /* Encode with branchless */
        residc_bw_init(&bw);
        encode_residual_branchless(&bw, val, k);
        int len_new = residc_bw_finish(&bw);
        uint8_t buf_new[32];
        memcpy(buf_new, bw.buf, len_new);

        /* Encode with original */
        residc_bw_init(&bw);
        residc_encode_residual(&bw, val, k);
        int len_orig = residc_bw_finish(&bw);
        uint8_t buf_orig[32];
        memcpy(buf_orig, bw.buf, len_orig);

        /* Verify identical bitstream */
        if (len_new != len_orig || memcmp(buf_new, buf_orig, len_orig) != 0) {
            if (errors < 5)
                printf("  ENCODE MISMATCH at i=%d val=%ld: orig_len=%d new_len=%d\n",
                       i, (long)val, len_orig, len_new);
            errors++;
            continue;
        }

        /* Decode with branchless from original-encoded data */
        residc_br_init(&br, buf_orig, len_orig);
        int64_t dec_new = decode_residual_branchless(&br, k);

        /* Decode with original */
        residc_br_init(&br, buf_orig, len_orig);
        int64_t dec_orig = residc_decode_residual(&br, k);

        if (dec_new != val || dec_orig != val) {
            if (errors < 5)
                printf("  DECODE MISMATCH at i=%d: val=%ld orig=%ld new=%ld\n",
                       i, (long)val, (long)dec_orig, (long)dec_new);
            errors++;
        }
    }

    /* Also test cross-compatibility: branchless encode -> original decode */
    for (int i = 0; i < n; i++) {
        int64_t val = residuals[i];
        residc_bw_init(&bw);
        encode_residual_branchless(&bw, val, k);
        int len = residc_bw_finish(&bw);

        residc_br_init(&br, bw.buf, len);
        int64_t decoded = residc_decode_residual(&br, k);
        if (decoded != val) {
            if (errors < 5)
                printf("  CROSS MISMATCH at i=%d: val=%ld decoded=%ld\n",
                       i, (long)val, (long)decoded);
            errors++;
        }
    }

    /* And: original encode -> branchless decode */
    for (int i = 0; i < n; i++) {
        int64_t val = residuals[i];
        residc_bw_init(&bw);
        residc_encode_residual(&bw, val, k);
        int len = residc_bw_finish(&bw);

        residc_br_init(&br, bw.buf, len);
        int64_t decoded = decode_residual_branchless(&br, k);
        if (decoded != val) {
            if (errors < 5)
                printf("  CROSS MISMATCH at i=%d: val=%ld decoded=%ld\n",
                       i, (long)val, (long)decoded);
            errors++;
        }
    }

    printf("  Errors: %d / %d (x3 cross-checks)\n", errors, n * 3);
    return errors;
}

/* Test edge cases */
static int verify_edge_cases(int k)
{
    residc_bitwriter_t bw;
    residc_bitreader_t br;
    int errors = 0;

    int64_t edge_values[] = {
        0, 1, -1, 2, -2,
        (1LL << k) - 1,        /* max tier 0 */
        (1LL << k),            /* min tier 1 (after zigzag) */
        (1LL << (k+2)) - 1,   /* max tier 1 */
        (1LL << (k+2)),        /* min tier 2 */
        (1LL << (k+5)) - 1,   /* max tier 2 */
        (1LL << (k+5)),        /* min tier 3 */
        (1LL << (k+10)) - 1,  /* max tier 3 */
        (1LL << (k+10)),       /* min tier 4 */
        INT64_MAX, INT64_MIN, INT64_MIN + 1,
        1234567890LL, -1234567890LL,
    };
    int n_edges = sizeof(edge_values) / sizeof(edge_values[0]);

    printf("\n--- Edge case verification (k=%d, %d values) ---\n", k, n_edges);

    for (int i = 0; i < n_edges; i++) {
        int64_t val = edge_values[i];

        /* Branchless encode -> branchless decode */
        residc_bw_init(&bw);
        encode_residual_branchless(&bw, val, k);
        int len = residc_bw_finish(&bw);
        residc_br_init(&br, bw.buf, len);
        int64_t dec1 = decode_residual_branchless(&br, k);

        /* Original encode -> original decode */
        residc_bw_init(&bw);
        residc_encode_residual(&bw, val, k);
        len = residc_bw_finish(&bw);
        residc_br_init(&br, bw.buf, len);
        int64_t dec2 = residc_decode_residual(&br, k);

        if (dec1 != val || dec2 != val) {
            printf("  EDGE FAIL: val=%ld branchless=%ld original=%ld\n",
                   (long)val, (long)dec1, (long)dec2);
            errors++;
        }
    }

    printf("  Errors: %d / %d\n", errors, n_edges);
    return errors;
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void)
{
    printf("============================================================\n");
    printf("Branchless Tier Selection Experiment\n");
    printf("CLZ-based encoder + 4-bit-peek LUT decoder\n");
    printf("============================================================\n");

    /* Allocate on heap to avoid stack overflow */
    int64_t *residuals = malloc(N_MSGS * sizeof(int64_t));
    Quote *msgs = malloc(N_MSGS * sizeof(Quote));
    if (!residuals || !msgs) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    gen_residuals(residuals, N_MSGS);
    gen_messages(msgs, N_MSGS);

    int total_errors = 0;

    /* Correctness first */
    total_errors += verify_edge_cases(3);
    total_errors += verify_edge_cases(7);
    total_errors += verify_edge_cases(10);
    total_errors += verify_roundtrip(residuals, N_MSGS, 3);
    total_errors += verify_roundtrip(residuals, N_MSGS, 7);

    if (total_errors > 0) {
        printf("\n*** CORRECTNESS FAILURES: %d errors ***\n", total_errors);
        printf("*** Skipping benchmarks until correctness is fixed ***\n");
        free(residuals);
        free(msgs);
        return 1;
    }

    printf("\nAll correctness checks PASSED.\n");

    /* Isolated residual benchmarks */
    bench_residual_encode(residuals, N_MSGS, 3);
    bench_residual_encode(residuals, N_MSGS, 7);
    bench_residual_decode(residuals, N_MSGS, 3);
    bench_residual_decode(residuals, N_MSGS, 7);

    /* Full message benchmark (baseline) */
    bench_full_message(msgs, N_MSGS);

    printf("\n============================================================\n");
    printf("Summary\n");
    printf("============================================================\n");
    printf("The branchless encoder uses __builtin_clzll + cmov chain to\n");
    printf("select the tier without branch mispredictions.\n");
    printf("The branchless decoder peeks 4 bits and uses a 16-entry LUT\n");
    printf("to determine tier and prefix length in one step.\n");
    printf("Both produce bit-identical output to the original.\n");

    free(residuals);
    free(msgs);
    return 0;
}
