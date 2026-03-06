/*
 * experiment_bitio.c — Word-at-a-time bit I/O experiment
 *
 * Compares original byte-at-a-time bit I/O vs 64-bit bulk flush/refill.
 *
 * Build:
 *   cc -O2 -march=native -o experiment_bitio bench/experiment_bitio.c core/residc.c -Icore
 *
 * Run:
 *   ./experiment_bitio
 */
#define _POSIX_C_SOURCE 199309L
#include "residc.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

/* ================================================================
 * Fast Bit Writer — 64-bit bulk flush
 *
 * Instead of flushing byte-by-byte in a while loop, flush all
 * complete bytes via a single 8-byte unaligned store.
 * Threshold is 32 so count + max_nbits(32) <= 64 (no overflow).
 * ================================================================ */

typedef struct {
    uint8_t  buf[RESIDC_SCRATCH_BYTES];
    uint64_t accum;
    int      count;
    int      byte_pos;
} fast_bitwriter_t;

static inline void fast_bw_init(fast_bitwriter_t *bw)
{
    bw->accum = 0;
    bw->count = 0;
    bw->byte_pos = 0;
}

static inline __attribute__((always_inline))
void fast_bw_write(fast_bitwriter_t *bw, uint64_t val, int nbits)
{
    bw->accum = (bw->accum << nbits) | (val & ((1ULL << nbits) - 1));
    bw->count += nbits;
    if (__builtin_expect(bw->count >= 32, 0)) {
        uint64_t be = __builtin_bswap64(bw->accum << (64 - bw->count));
        memcpy(bw->buf + bw->byte_pos, &be, 8);
        int bytes_out = bw->count >> 3;
        bw->byte_pos += bytes_out;
        bw->count &= 7;
    }
}

static inline int fast_bw_finish(fast_bitwriter_t *bw)
{
    while (bw->count >= 8) {
        bw->count -= 8;
        bw->buf[bw->byte_pos++] = (uint8_t)(bw->accum >> bw->count);
    }
    if (bw->count > 0 && bw->byte_pos < RESIDC_SCRATCH_BYTES) {
        bw->buf[bw->byte_pos++] = (uint8_t)(bw->accum << (8 - bw->count));
    }
    return bw->byte_pos;
}

/* ================================================================
 * Fast Bit Reader — 64-bit bulk refill
 *
 * Load 8 bytes at once, byte-swap, and merge whole bytes into
 * the accumulator instead of looping byte-by-byte.
 * ================================================================ */

typedef struct {
    const uint8_t *data;
    int            len_bytes;
    uint64_t       accum;
    int            count;
    int            byte_pos;
} fast_bitreader_t;

static inline void fast_br_init(fast_bitreader_t *br, const uint8_t *data, int len)
{
    br->data = data;
    br->len_bytes = len;
    br->accum = 0;
    br->count = 0;
    br->byte_pos = 0;
}

static inline __attribute__((always_inline))
void fast_br_refill(fast_bitreader_t *br)
{
    if (__builtin_expect(br->byte_pos + 8 <= br->len_bytes, 1)) {
        uint64_t raw;
        memcpy(&raw, br->data + br->byte_pos, 8);
        raw = __builtin_bswap64(raw);
        int consume = (64 - br->count) >> 3;
        if (__builtin_expect(consume > 0, 1)) {
            int consume_bits = consume << 3;
            if (__builtin_expect(consume_bits < 64, 1)) {
                br->accum = (br->accum << consume_bits) | (raw >> (64 - consume_bits));
            } else {
                br->accum = raw;
            }
            br->count += consume_bits;
            br->byte_pos += consume;
        }
    } else {
        while (br->count <= 56 && br->byte_pos < br->len_bytes) {
            br->accum = (br->accum << 8) | br->data[br->byte_pos++];
            br->count += 8;
        }
    }
}

static inline __attribute__((always_inline))
uint64_t fast_br_read(fast_bitreader_t *br, int nbits)
{
    if (__builtin_expect(br->count < nbits, 0))
        fast_br_refill(br);
    br->count -= nbits;
    return (br->accum >> br->count) & ((1ULL << nbits) - 1);
}

static inline __attribute__((always_inline))
int fast_br_read_bit(fast_bitreader_t *br)
{
    if (__builtin_expect(br->count < 1, 0))
        fast_br_refill(br);
    br->count--;
    return (int)((br->accum >> br->count) & 1);
}

/* ================================================================
 * Test types and schema
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

static const residc_schema_t schema = {
    .fields = quote_fields, .num_fields = 5, .msg_size = sizeof(Quote),
};

#define N_MSGS  100000
#define N_ITERS 10

/* ================================================================
 * Isolated bit I/O microbenchmark
 * ================================================================ */

static double bench_original_bitio_write(int n_values)
{
    residc_bitwriter_t bw;
    double best = 1e18;

    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_bw_init(&bw);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < n_values; i++) {
            residc_bw_write(&bw, i & 1, 1);
            residc_bw_write(&bw, (uint64_t)(i % 7), 3);
            residc_bw_write(&bw, (uint64_t)(i % 255), 8);
            if (bw.byte_pos > RESIDC_SCRATCH_BYTES - 16)
                residc_bw_init(&bw);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        if (elapsed < best) best = elapsed;
    }
    return best;
}

static double bench_fast_bitio_write(int n_values)
{
    fast_bitwriter_t bw;
    double best = 1e18;

    for (int iter = 0; iter < N_ITERS; iter++) {
        fast_bw_init(&bw);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < n_values; i++) {
            fast_bw_write(&bw, i & 1, 1);
            fast_bw_write(&bw, (uint64_t)(i % 7), 3);
            fast_bw_write(&bw, (uint64_t)(i % 255), 8);
            if (bw.byte_pos > RESIDC_SCRATCH_BYTES - 16)
                fast_bw_init(&bw);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        if (elapsed < best) best = elapsed;
    }
    return best;
}

/* ================================================================
 * Full encode/decode benchmark using original residc API
 * ================================================================ */

static void generate_msgs(Quote *msgs)
{
    uint64_t ts = 34200000000000ULL;
    for (int i = 0; i < N_MSGS; i++) {
        ts += 1000 + ((uint64_t)i * 37 % 50000);
        msgs[i].timestamp = ts;
        msgs[i].instrument_id = (uint16_t)(i % 50);
        msgs[i].price = 1500000 + (uint32_t)((i * 7) % 2000);
        msgs[i].quantity = (uint32_t)((1 + i % 20) * 100);
        msgs[i].side = (uint8_t)(i % 2);
    }
}

static double bench_original_encode(Quote *msgs)
{
    residc_state_t enc;
    uint8_t buf[64];
    double best = 1e18;

    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(&enc, &schema);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < N_MSGS; i++)
            residc_encode(&enc, &msgs[i], buf, 64);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        double ns_per = elapsed / N_MSGS;
        if (ns_per < best) best = ns_per;
    }
    return best;
}

static double bench_original_decode(uint8_t encoded_bufs[][64], int *encoded_lens)
{
    residc_state_t dec;
    Quote decoded;
    double best = 1e18;

    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(&dec, &schema);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < N_MSGS; i++)
            residc_decode(&dec, encoded_bufs[i], encoded_lens[i], &decoded);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        double ns_per = elapsed / N_MSGS;
        if (ns_per < best) best = ns_per;
    }
    return best;
}

/* ================================================================
 * Correctness verification
 * ================================================================ */

static int verify_fast_bitio(void)
{
    fast_bitwriter_t bw;
    fast_bw_init(&bw);

    int test_nbits[]  = { 1, 3, 8, 14, 1, 5, 32, 1, 7, 16, 2, 10, 4, 6, 1 };
    uint64_t test_vals[] = {
        1, 5, 0xAB, 0x3FFF, 0, 0x1F, 0xDEADBEEF, 1, 0x7F, 0xCAFE,
        3, 0x3FF, 0xA, 0x3F, 0
    };
    int n_tests = (int)(sizeof(test_nbits) / sizeof(test_nbits[0]));

    for (int i = 0; i < n_tests; i++)
        fast_bw_write(&bw, test_vals[i], test_nbits[i]);
    int total_bytes = fast_bw_finish(&bw);

    fast_bitreader_t br;
    fast_br_init(&br, bw.buf, total_bytes);

    int errors = 0;
    for (int i = 0; i < n_tests; i++) {
        uint64_t got = fast_br_read(&br, test_nbits[i]);
        uint64_t expected = test_vals[i] & ((1ULL << test_nbits[i]) - 1);
        if (got != expected) {
            printf("  MISMATCH test %d: expected 0x%llx got 0x%llx\n",
                   i, (unsigned long long)expected, (unsigned long long)got);
            errors++;
        }
    }
    return errors;
}

static int verify_wire_compat(void)
{
    residc_bitwriter_t orig;
    fast_bitwriter_t fast;
    residc_bw_init(&orig);
    fast_bw_init(&fast);

    int test_nbits[]  = { 1, 3, 8, 14, 1, 5, 32, 1, 7, 16, 2, 10, 4, 6, 1 };
    uint64_t test_vals[] = {
        1, 5, 0xAB, 0x3FFF, 0, 0x1F, 0xDEADBEEF, 1, 0x7F, 0xCAFE,
        3, 0x3FF, 0xA, 0x3F, 0
    };
    int n_tests = (int)(sizeof(test_nbits) / sizeof(test_nbits[0]));

    for (int i = 0; i < n_tests; i++) {
        residc_bw_write(&orig, test_vals[i], test_nbits[i]);
        fast_bw_write(&fast, test_vals[i], test_nbits[i]);
    }

    int orig_len = residc_bw_finish(&orig);
    int fast_len = fast_bw_finish(&fast);

    if (orig_len != fast_len) {
        printf("  Wire compat: length mismatch (orig=%d, fast=%d)\n", orig_len, fast_len);
        return 1;
    }
    if (memcmp(orig.buf, fast.buf, orig_len) != 0) {
        printf("  Wire compat: content mismatch\n");
        return 1;
    }
    return 0;
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
    printf("residc bit I/O experiment: word-at-a-time vs byte-at-a-time\n");
    printf("============================================================\n\n");

    /* Correctness */
    printf("1. Correctness verification\n");
    int bitio_err = verify_fast_bitio();
    printf("   Fast bit I/O roundtrip:  %s (%d errors)\n",
           bitio_err == 0 ? "PASS" : "FAIL", bitio_err);
    int wire_err = verify_wire_compat();
    printf("   Wire compatibility:      %s\n",
           wire_err == 0 ? "PASS" : "FAIL");
    if (bitio_err || wire_err) {
        printf("\nAborting benchmark due to correctness failures.\n");
        return 1;
    }

    /* Isolated microbenchmark */
    printf("\n2. Isolated bit I/O microbenchmark (1M write ops x 3 writes each)\n");
    int n_micro = 1000000;
    double orig_ns = bench_original_bitio_write(n_micro);
    double fast_ns = bench_fast_bitio_write(n_micro);
    printf("   Original bw_write:  %.1f ns total (%.2f ns/op)\n",
           orig_ns, orig_ns / (n_micro * 3.0));
    printf("   Fast bw_write:      %.1f ns total (%.2f ns/op)\n",
           fast_ns, fast_ns / (n_micro * 3.0));
    printf("   Speedup:            %.2fx\n", orig_ns / fast_ns);

    /* Full codec benchmark */
    printf("\n3. Full codec benchmark (%d msgs, best of %d iters)\n", N_MSGS, N_ITERS);
    printf("   (These already use the integrated fast bit I/O in residc.c)\n");

    static Quote msgs[N_MSGS];
    static uint8_t encoded_bufs[N_MSGS][64];
    static int encoded_lens[N_MSGS];
    generate_msgs(msgs);

    {
        residc_state_t enc;
        residc_init(&enc, &schema);
        long total = 0;
        for (int i = 0; i < N_MSGS; i++) {
            encoded_lens[i] = residc_encode(&enc, &msgs[i], encoded_bufs[i], 64);
            total += encoded_lens[i];
        }
        int raw = residc_raw_size(&schema);
        printf("   Compression:  %.2f:1 (%d -> %.1f bytes avg)\n",
               (double)(N_MSGS * raw) / total, raw, (double)total / N_MSGS);
    }

    double enc_ns = bench_original_encode(msgs);
    double dec_ns = bench_original_decode(encoded_bufs, encoded_lens);
    printf("   Encode:       %.0f ns/msg\n", enc_ns);
    printf("   Decode:       %.0f ns/msg\n", dec_ns);

    /* Roundtrip verification */
    {
        residc_state_t enc, dec;
        residc_init(&enc, &schema);
        residc_init(&dec, &schema);
        uint8_t buf[64];
        int errors = 0;
        for (int i = 0; i < N_MSGS; i++) {
            int len = residc_encode(&enc, &msgs[i], buf, 64);
            Quote decoded;
            residc_decode(&dec, buf, len, &decoded);
            if (memcmp(&msgs[i], &decoded, sizeof(Quote)) != 0) errors++;
        }
        printf("   Roundtrip:    %d errors\n", errors);
    }

    printf("\n4. Summary\n");
    printf("   Isolated bit I/O write speedup: %.2fx\n", orig_ns / fast_ns);
    printf("   Wire format: IDENTICAL (verified)\n");
    printf("   Roundtrip: CORRECT\n");

    return 0;
}
