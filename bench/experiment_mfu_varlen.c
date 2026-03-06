/*
 * experiment_mfu_varlen.c — Variable-length MFU index encoding experiment
 *
 * Current: instrument MFU index is always 8 bits.
 * Proposed: variable-length code where low ranks (frequent instruments) use fewer bits.
 *
 * Approach: maintain a frequency-sorted rank mapping alongside the MFU table.
 * Encode the rank (not raw index) with tiered variable-length code:
 *   Rank 0-3:    prefix 0   + 2 bits = 3 bits total
 *   Rank 4-15:   prefix 10  + 4 bits = 6 bits total
 *   Rank 16-63:  prefix 110 + 6 bits = 9 bits total
 *   Rank 64-255: prefix 111 + 8 bits = 11 bits total
 *
 * vs fixed 8 bits. Savings depend on how skewed the distribution is.
 *
 * Build:
 *   cc -O2 -march=native -o experiment_mfu_varlen bench/experiment_mfu_varlen.c core/residc.c -Icore
 */
#define _POSIX_C_SOURCE 199309L
#include "residc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N_MSGS 100000

/* ================================================================
 * Analysis: measure MFU index distribution
 *
 * First, let's just instrument the existing codec to see what
 * MFU indices are actually used and how they distribute.
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

/* ================================================================
 * RNG (same as bench_compression.c)
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
 * Data generators
 * ================================================================ */

/* Uniform distribution: 50 instruments */
static void gen_uniform(Quote *msgs, int n) {
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

/* Zipf-like distribution: top 5 get ~60%, next 15 get ~30%, rest get ~10% */
static void gen_zipf(Quote *msgs, int n) {
    uint64_t ts = 34200000000000ULL;
    for (int i = 0; i < n; i++) {
        ts += 500 + rng_range(0, 50000);
        msgs[i].timestamp = ts;
        uint32_t r = rng_range(0, 99);
        if (r < 60)
            msgs[i].instrument_id = (uint16_t)rng_range(0, 4);    /* top 5: 60% */
        else if (r < 90)
            msgs[i].instrument_id = (uint16_t)rng_range(5, 19);   /* next 15: 30% */
        else
            msgs[i].instrument_id = (uint16_t)rng_range(20, 199); /* rest: 10% */
        msgs[i].price = 1500000 + (uint32_t)(rng_range(0, 2000)) - 1000;
        msgs[i].quantity = (uint32_t)(rng_range(1, 20) * 100);
        msgs[i].side = (uint8_t)(rng_next() & 1);
    }
}

/* Heavy concentration: top 3 instruments get 80% */
static void gen_concentrated(Quote *msgs, int n) {
    uint64_t ts = 34200000000000ULL;
    for (int i = 0; i < n; i++) {
        ts += 500 + rng_range(0, 50000);
        msgs[i].timestamp = ts;
        uint32_t r = rng_range(0, 99);
        if (r < 80)
            msgs[i].instrument_id = (uint16_t)rng_range(0, 2);    /* top 3: 80% */
        else
            msgs[i].instrument_id = (uint16_t)rng_range(3, 49);   /* rest: 20% */
        msgs[i].price = 1500000 + (uint32_t)(rng_range(0, 2000)) - 1000;
        msgs[i].quantity = (uint32_t)(rng_range(1, 20) * 100);
        msgs[i].side = (uint8_t)(rng_next() & 1);
    }
}

/* ================================================================
 * MFU index distribution analysis
 *
 * Run the encoder and intercept MFU lookups to see the distribution
 * of raw indices. This tells us how much variable-length could save.
 * ================================================================ */

static void analyze_mfu_distribution(const char *label, Quote *msgs, int n)
{
    residc_state_t enc;
    residc_init(&enc, &quote_schema);

    int mfu_hit_count = 0;
    int same_count = 0;
    int raw_count = 0;
    int total = 0;

    /* Track index usage by rank bucket */
    int idx_hist[256];
    memset(idx_hist, 0, sizeof(idx_hist));

    for (int i = 0; i < n; i++) {
        uint16_t inst = msgs[i].instrument_id;

        if (inst == enc.last_instrument_id && enc.msg_count > 0) {
            same_count++;
        } else {
            int mfu_idx = residc_mfu_lookup(&enc.mfu, inst);
            if (mfu_idx >= 0) {
                mfu_hit_count++;
                idx_hist[mfu_idx]++;
            } else {
                raw_count++;
            }
        }
        total++;

        /* Commit state (encode + discard output) */
        uint8_t buf[64];
        residc_encode(&enc, &msgs[i], buf, 64);
        /* Note: encode already commits state internally, so we just
         * re-init for analysis. Actually, we need to track the state
         * that encode maintains. Let's just use the encode output. */
    }

    /* Re-run properly: just encode and track the MFU state */
    residc_init(&enc, &quote_schema);
    mfu_hit_count = 0;
    same_count = 0;
    raw_count = 0;
    total = 0;
    memset(idx_hist, 0, sizeof(idx_hist));

    for (int i = 0; i < n; i++) {
        uint16_t inst = msgs[i].instrument_id;

        if (inst == enc.last_instrument_id && enc.msg_count > 0) {
            same_count++;
        } else {
            int mfu_idx = residc_mfu_lookup(&enc.mfu, inst);
            if (mfu_idx >= 0) {
                mfu_hit_count++;
                idx_hist[mfu_idx]++;
            } else {
                raw_count++;
            }
        }
        total++;

        /* Now compute rank for this index: count entries with higher frequency */
        /* We need the full encode to advance state properly */
        uint8_t buf[64];
        residc_encode(&enc, &msgs[i], buf, 64);
    }

    /* Now compute what ranks would have been if we sorted by frequency */
    /* Sort indices by count (descending) to get rank mapping */
    typedef struct { int idx; uint16_t count; } sort_entry;
    sort_entry sorted[256];
    for (int i = 0; i < 256; i++) {
        sorted[i].idx = i;
        sorted[i].count = enc.mfu.entries[i].count;
    }
    /* Simple insertion sort (256 elements, once) */
    for (int i = 1; i < (int)enc.mfu.num_entries; i++) {
        sort_entry tmp = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j].count < tmp.count) {
            sorted[j+1] = sorted[j];
            j--;
        }
        sorted[j+1] = tmp;
    }

    /* Build idx_to_rank mapping */
    int idx_to_rank[256];
    memset(idx_to_rank, 0, sizeof(idx_to_rank));
    for (int r = 0; r < (int)enc.mfu.num_entries; r++) {
        idx_to_rank[sorted[r].idx] = r;
    }

    /* Compute rank distribution */
    int rank_hist[256];
    memset(rank_hist, 0, sizeof(rank_hist));
    for (int i = 0; i < 256; i++) {
        if (idx_hist[i] > 0) {
            int rank = idx_to_rank[i];
            rank_hist[rank] += idx_hist[i];
        }
    }

    /* Compute bit savings */
    int bits_fixed = 0;
    int bits_varlen = 0;
    int rank_0_3 = 0, rank_4_15 = 0, rank_16_63 = 0, rank_64_255 = 0;

    for (int r = 0; r < 256; r++) {
        if (rank_hist[r] == 0) continue;
        bits_fixed += rank_hist[r] * 8;
        if (r < 4) {
            bits_varlen += rank_hist[r] * 3;
            rank_0_3 += rank_hist[r];
        } else if (r < 16) {
            bits_varlen += rank_hist[r] * 6;
            rank_4_15 += rank_hist[r];
        } else if (r < 64) {
            bits_varlen += rank_hist[r] * 9;
            rank_16_63 += rank_hist[r];
        } else {
            bits_varlen += rank_hist[r] * 11;
            rank_64_255 += rank_hist[r];
        }
    }

    printf("\n  [%s] — %d messages, %d instruments in MFU\n", label, n, enc.mfu.num_entries);
    printf("  Instrument encoding breakdown:\n");
    printf("    Same-as-last:  %6d (%5.1f%%) — 1 bit\n", same_count, 100.0*same_count/total);
    printf("    MFU hit:       %6d (%5.1f%%) — currently 1+1+8 = 10 bits\n", mfu_hit_count, 100.0*mfu_hit_count/total);
    printf("    Raw:           %6d (%5.1f%%) — 1+1+14 = 16 bits\n", raw_count, 100.0*raw_count/total);
    printf("  MFU rank distribution (if frequency-sorted):\n");
    printf("    Rank 0-3:    %6d (%5.1f%%) — 3 bits (saves 5 bits each)\n", rank_0_3, 100.0*rank_0_3/total);
    printf("    Rank 4-15:   %6d (%5.1f%%) — 6 bits (saves 2 bits each)\n", rank_4_15, 100.0*rank_4_15/total);
    printf("    Rank 16-63:  %6d (%5.1f%%) — 9 bits (costs 1 bit each)\n", rank_16_63, 100.0*rank_16_63/total);
    printf("    Rank 64-255: %6d (%5.1f%%) — 11 bits (costs 3 bits each)\n", rank_64_255, 100.0*rank_64_255/total);
    printf("  Bit comparison (MFU hits only):\n");
    printf("    Fixed 8-bit: %7d bits (%.1f bits/msg avg)\n", bits_fixed, (double)bits_fixed/mfu_hit_count);
    printf("    Variable:    %7d bits (%.1f bits/msg avg)\n", bits_varlen, (double)bits_varlen/mfu_hit_count);
    printf("    Savings:     %7d bits (%.1f%%)\n", bits_fixed - bits_varlen,
           100.0*(bits_fixed - bits_varlen)/bits_fixed);

    /* Total message-level impact */
    double bytes_saved = (double)(bits_fixed - bits_varlen) / 8.0;
    printf("  Total byte savings: %.0f bytes across %d MFU-encoded messages\n", bytes_saved, mfu_hit_count);
    printf("  Per-message savings: %.2f bytes avg (across ALL messages)\n", bytes_saved / total);

    /* Compare against actual compressed sizes */
    residc_init(&enc, &quote_schema);
    long total_compressed = 0;
    for (int i = 0; i < n; i++) {
        uint8_t buf[64];
        int len = residc_encode(&enc, &msgs[i], buf, 64);
        total_compressed += len;
    }
    int raw_size = residc_raw_size(&quote_schema);
    double current_avg = (double)total_compressed / n;
    double improved_avg = current_avg - bytes_saved / n;
    printf("  Current compression: %.2f:1 (%.1f bytes avg)\n",
           (double)raw_size / current_avg, current_avg);
    printf("  Projected with varlen MFU: %.2f:1 (%.1f bytes avg)\n",
           (double)raw_size / improved_avg, improved_avg);
}

/* ================================================================
 * Benchmark: implement variable-length MFU in a modified codec
 *
 * We can't easily swap out just the MFU encoding in the existing
 * codec without modifying residc.c. Instead, we'll:
 * 1. Measure the theoretical savings (above)
 * 2. Implement a standalone encode/decode with varlen MFU
 *    to verify correctness and measure overhead
 * ================================================================ */

/* Variable-length MFU rank encoding */
static inline void encode_mfu_rank(residc_bitwriter_t *bw, int rank)
{
    if (rank < 4) {
        residc_bw_write(bw, 0, 1);           /* prefix 0 */
        residc_bw_write(bw, (uint64_t)rank, 2);  /* 2-bit index */
    } else if (rank < 16) {
        residc_bw_write(bw, 2, 2);           /* prefix 10 */
        residc_bw_write(bw, (uint64_t)(rank - 4), 4);
    } else if (rank < 64) {
        residc_bw_write(bw, 6, 3);           /* prefix 110 */
        residc_bw_write(bw, (uint64_t)(rank - 16), 6);
    } else {
        residc_bw_write(bw, 7, 3);           /* prefix 111 */
        residc_bw_write(bw, (uint64_t)(rank - 64), 8);
    }
}

static inline int decode_mfu_rank(residc_bitreader_t *br)
{
    if (residc_br_read_bit(br) == 0) {
        return (int)residc_br_read(br, 2);           /* rank 0-3 */
    }
    if (residc_br_read_bit(br) == 0) {
        return 4 + (int)residc_br_read(br, 4);       /* rank 4-15 */
    }
    if (residc_br_read_bit(br) == 0) {
        return 16 + (int)residc_br_read(br, 6);      /* rank 16-63 */
    }
    return 64 + (int)residc_br_read(br, 8);          /* rank 64-255 */
}

/* Verify the variable-length encode/decode roundtrip */
static int verify_varlen_roundtrip(void)
{
    int errors = 0;
    for (int rank = 0; rank < 256; rank++) {
        residc_bitwriter_t bw;
        residc_bw_init(&bw);
        encode_mfu_rank(&bw, rank);
        int len = residc_bw_finish(&bw);

        residc_bitreader_t br;
        residc_br_init(&br, bw.buf, len);
        int decoded = decode_mfu_rank(&br);

        if (decoded != rank) {
            printf("  MISMATCH: rank %d decoded as %d\n", rank, decoded);
            errors++;
        }
    }
    return errors;
}

/* Verify bit counts are as expected */
static void verify_bit_counts(void)
{
    printf("  Bit count verification:\n");
    int expected[] = { 3, 3, 3, 3 };  /* rank 0-3: 3 bits */
    int ranks[] = { 0, 1, 2, 3 };

    for (int tier = 0; tier < 4; tier++) {
        int test_ranks[4][2] = { {0,3}, {4,15}, {16,63}, {64,255} };
        int expected_bits[] = { 3, 6, 9, 11 };
        (void)expected;
        (void)ranks;

        residc_bitwriter_t bw;
        residc_bw_init(&bw);
        encode_mfu_rank(&bw, test_ranks[tier][0]);
        int len0 = residc_bw_finish(&bw);
        /* Bit count is approximately len * 8, but trailing bits pad */
        /* Just check total bytes are reasonable */
        int min_bytes = (expected_bits[tier] + 7) / 8;
        printf("    Tier %d (rank %d-%d): %d bits expected, %d bytes encoded (%s)\n",
               tier, test_ranks[tier][0], test_ranks[tier][1],
               expected_bits[tier], len0,
               len0 <= min_bytes ? "OK" : "UNEXPECTED");
    }
}

/* ================================================================
 * Microbenchmark: encoding/decoding overhead of varlen vs fixed
 * ================================================================ */

static double bench_fixed_encode(int n)
{
    residc_bitwriter_t bw;
    double best = 1e18;

    for (int iter = 0; iter < 10; iter++) {
        residc_bw_init(&bw);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < n; i++) {
            residc_bw_write(&bw, (uint64_t)(i % 256), 8);
            if (bw.byte_pos > RESIDC_SCRATCH_BYTES - 16)
                residc_bw_init(&bw);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        if (ns < best) best = ns;
    }
    return best;
}

static double bench_varlen_encode(int n)
{
    residc_bitwriter_t bw;
    double best = 1e18;

    /* Simulate realistic rank distribution: 40% rank 0-3, 30% rank 4-15, etc. */
    for (int iter = 0; iter < 10; iter++) {
        residc_bw_init(&bw);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < n; i++) {
            int rank;
            int r = i % 100;
            if (r < 40) rank = r % 4;
            else if (r < 70) rank = 4 + (r % 12);
            else if (r < 90) rank = 16 + (r % 48);
            else rank = 64 + (r % 192);
            encode_mfu_rank(&bw, rank);
            if (bw.byte_pos > RESIDC_SCRATCH_BYTES - 16)
                residc_bw_init(&bw);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        if (ns < best) best = ns;
    }
    return best;
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
    printf("residc MFU variable-length index experiment\n");
    printf("============================================\n");

    /* 1. Correctness */
    printf("\n1. Correctness verification\n");
    int errors = verify_varlen_roundtrip();
    printf("   Varlen roundtrip (all 256 ranks): %s (%d errors)\n",
           errors == 0 ? "PASS" : "FAIL", errors);
    if (errors) return 1;

    verify_bit_counts();

    /* 2. Distribution analysis */
    printf("\n2. MFU index distribution analysis\n");

    static Quote msgs[N_MSGS];

    rng_state = 12345678901ULL;
    gen_uniform(msgs, N_MSGS);
    analyze_mfu_distribution("Uniform (50 instruments)", msgs, N_MSGS);

    rng_state = 12345678901ULL;
    gen_zipf(msgs, N_MSGS);
    analyze_mfu_distribution("Zipf-like (top 5 = 60%)", msgs, N_MSGS);

    rng_state = 12345678901ULL;
    gen_concentrated(msgs, N_MSGS);
    analyze_mfu_distribution("Concentrated (top 3 = 80%)", msgs, N_MSGS);

    /* 3. Encoding overhead microbenchmark */
    printf("\n3. Encoding overhead microbenchmark (1M ops)\n");
    int n_ops = 1000000;
    double fixed_ns = bench_fixed_encode(n_ops);
    double varlen_ns = bench_varlen_encode(n_ops);
    printf("   Fixed 8-bit:  %.1f ns total (%.2f ns/op)\n", fixed_ns, fixed_ns / n_ops);
    printf("   Variable-len: %.1f ns total (%.2f ns/op)\n", varlen_ns, varlen_ns / n_ops);
    printf("   Overhead:     %.2fx (varlen is %s)\n",
           varlen_ns / fixed_ns,
           varlen_ns > fixed_ns ? "slower" : "faster");

    /* 4. Summary */
    printf("\n4. Summary\n");
    printf("   Variable-length MFU encoding trades ~1-2 extra branch instructions\n");
    printf("   for bit savings on skewed distributions. The savings are:\n");
    printf("   - Uniform distribution: minimal (all ranks roughly equal)\n");
    printf("   - Zipf/concentrated: significant (top instruments use 3 bits vs 8)\n");
    printf("   - Net impact depends on what fraction of messages hit the MFU path\n");
    printf("     vs same-as-last (which is already 1 bit and unaffected)\n");
    printf("\n   NOTE: Implementing this in residc.c requires adding rank tracking\n");
    printf("   to the MFU table (idx_to_rank/rank_to_idx arrays, rebuilt at decay).\n");
    printf("   This adds ~512 bytes of state and a sort every 10K messages.\n");

    return 0;
}
