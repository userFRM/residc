/*
 * experiment_qty_runs.c — Per-instrument quantity zero-run encoding experiment
 *
 * Quantities for a given instrument often repeat many times in a row.
 * Currently we always spend 1 bit for the "same as predicted" flag.
 * This experiment tracks a per-instrument qty_repeat_count. When the
 * count exceeds a threshold, we switch to "run mode" where:
 *   - Same quantity: 0 (1 bit)   -- cheaper path, just like today
 *   - Different:     1 + payload  -- 1-bit escape + full residual
 *
 * But the real savings come from an even simpler approach: after N
 * consecutive identical quantities, we SKIP the same-flag entirely
 * (0 bits for "same"). Only emit a 1-bit "changed" escape followed
 * by the new value when it actually changes. This is basically
 * run-length coding of the zero-residual case.
 *
 * Protocol (per instrument, tracked by qty_repeat_count):
 *   repeat_count < THRESHOLD:
 *     Same as current: 0 = same (1 bit), 1 + payload = different
 *   repeat_count >= THRESHOLD (run mode):
 *     Read 1 bit: 0 = still same (continue run), 1 = changed + payload
 *     This looks identical in bit format to the current encoding!
 *     The savings come from the DECODER not needing the flag when
 *     confidence is 100% — but actually, we still need the flag
 *     to tell the decoder when the run breaks.
 *
 * Better approach: ASYMMETRIC probability coding. When repeat_count
 * is high, use a Golomb-like code where "same" is very cheap:
 *   - For repeat_count >= 4: use 2-bit "run extension":
 *     00 = 3 more same (2 bits for 3 messages)
 *     01 = 2 more same, then change
 *     10 = 1 more same, then change
 *     11 = change now
 *     This amortizes the flag cost over multiple messages.
 *
 * Actually, the simplest and most robust approach: just track
 * whether we're in a "high confidence" mode per instrument, and
 * when we are, emit the same-flag only on CHANGE (saving 1 bit
 * per message for the common case of repeated quantities).
 *
 * Wait — the encoder and decoder must stay synchronized. The decoder
 * ALWAYS needs to know whether the quantity changed or not. So we
 * can't skip the flag entirely. But we CAN make the representation
 * more efficient:
 *
 * FINAL DESIGN:
 * Track qty_repeat_count per instrument. When >= THRESHOLD:
 *   Encoder reads the next N messages for this instrument and
 *   encodes a run length... but that breaks the streaming property.
 *
 * Simplest viable approach that maintains streaming:
 * Track qty_repeat_count. When >= THRESHOLD, the "same" flag bit
 * is statistically very likely to be 0. We can't remove it, but we
 * could batch: every B messages for this instrument, emit a single
 * "all same" bit. If all B were same, cost is 1 bit / B messages.
 * If not, emit individual flags. But this requires lookahead.
 *
 * PRACTICAL approach (no lookahead):
 * Use an ADAPTIVE same-flag: when qty_repeat_count >= THRESHOLD,
 * don't emit the same-flag at all for "same" — just emit nothing.
 * For "changed", emit a 1-bit escape (1) + payload.
 * The decoder mirrors: in run mode, try to read 1 bit. If 0, it's
 * still same. If 1, read the change payload.
 * Wait, this is exactly the same as the current encoding! The bit
 * cost is identical.
 *
 * THE REAL WIN: The current encoding always emits the "same" flag
 * PLUS the round-lot/odd-lot flag PLUS the residual. But when
 * repeat_count >= THRESHOLD, we can use a SHORTER escape:
 *   - In run mode: 0 = same (1 bit), 1 = exit run (1 bit)
 *   - After exiting run: encode quantity as absolute (not residual)
 *     using fewer bits since we know the range from history
 *
 * OK, let me just measure what the actual savings potential is first,
 * then implement the most practical variant.
 *
 * Build:
 *   cc -O2 -march=native -o experiment_qty_runs bench/experiment_qty_runs.c core/residc.c -Icore
 */
#define _POSIX_C_SOURCE 199309L
#include "residc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ================================================================
 * Data generation (reuse patterns from bench_compression.c)
 * ================================================================ */

static uint64_t rng_state;

static uint32_t rng_next(void) {
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(rng_state >> 32);
}

static uint32_t rng_range(uint32_t lo, uint32_t hi) {
    return lo + rng_next() % (hi - lo + 1);
}

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

#define N_MSGS     100000
#define N_INSTR    50
#define N_ITERS    20

/* ================================================================
 * Data analysis: measure quantity repeat patterns
 * ================================================================ */

typedef struct {
    uint32_t last_qty;
    uint32_t msg_count;
    uint32_t total_same;     /* number of times qty == last_qty */
    uint32_t run_length;     /* current consecutive same count */
    /* histogram: how many messages had run_length >= threshold */
    uint32_t in_run_at[9];   /* threshold 1,2,3,4,5,6,7,8,16 */
} InstrAnalysis;

static void analyze_repeats(Quote *msgs, int n)
{
    InstrAnalysis instr[N_INSTR];
    memset(instr, 0, sizeof(instr));

    int thresholds[] = {1, 2, 3, 4, 5, 6, 7, 8, 16};
    int n_thresh = 9;

    long total_qty_msgs = 0;

    for (int i = 0; i < n; i++) {
        int id = msgs[i].instrument_id;
        if (id >= N_INSTR) continue;
        InstrAnalysis *a = &instr[id];

        if (a->msg_count > 0) {
            total_qty_msgs++;
            if (msgs[i].quantity == a->last_qty) {
                a->total_same++;
                a->run_length++;
            } else {
                a->run_length = 0;
            }

            for (int t = 0; t < n_thresh; t++) {
                if ((int)a->run_length >= thresholds[t])
                    a->in_run_at[t]++;
            }
        }

        a->last_qty = msgs[i].quantity;
        a->msg_count++;
    }

    /* Aggregate */
    long total_same = 0;
    long in_run[9] = {0};
    for (int id = 0; id < N_INSTR; id++) {
        total_same += instr[id].total_same;
        for (int t = 0; t < n_thresh; t++)
            in_run[t] += instr[id].in_run_at[t];
    }

    printf("=== Quantity Repeat Analysis (%d messages, %d instruments) ===\n", n, N_INSTR);
    printf("  Total quantity comparisons: %ld\n", total_qty_msgs);
    printf("  Same as last:              %ld (%.1f%%)\n",
           total_same, 100.0 * total_same / total_qty_msgs);
    printf("\n  Messages where run_length >= threshold (potential savings):\n");
    for (int t = 0; t < n_thresh; t++) {
        printf("    threshold=%2d: %6ld msgs (%.1f%%) -- could save 1 bit each\n",
               thresholds[t], in_run[t], 100.0 * in_run[t] / total_qty_msgs);
    }

    /* Compute potential bit savings */
    printf("\n  Potential savings at threshold=4:\n");
    long saveable = in_run[3]; /* threshold=4 */
    printf("    %ld messages where run >= 4\n", saveable);
    printf("    Current cost: 1 bit each (same-flag) = %ld bits\n", saveable);
    printf("    If we could skip the flag: save %ld bits = %.1f bytes\n",
           saveable, saveable / 8.0);
    printf("    Per message avg saving: %.3f bits\n",
           (double)saveable / total_qty_msgs);
}

/* ================================================================
 * Realistic data: higher repeat rates (like real market data)
 * ================================================================
 *
 * Real market data has much higher quantity repeat rates than the
 * uniform random data in bench_compression.c. In real NASDAQ ITCH,
 * quantities often repeat 80-95% of the time for active instruments.
 * Generate a more realistic dataset.
 */

static void gen_quotes_realistic(Quote *msgs, int n)
{
    uint64_t ts = 34200000000000ULL;
    /* Each instrument has a "sticky" quantity that changes rarely */
    uint32_t sticky_qty[N_INSTR];
    for (int i = 0; i < N_INSTR; i++)
        sticky_qty[i] = (uint32_t)((1 + i % 10) * 100);

    for (int i = 0; i < n; i++) {
        ts += 500 + rng_range(0, 50000);
        int id = (int)rng_range(0, N_INSTR - 1);
        msgs[i].timestamp = ts;
        msgs[i].instrument_id = (uint16_t)id;
        msgs[i].price = 1500000 + (uint32_t)(rng_range(0, 2000)) - 1000;
        msgs[i].side = (uint8_t)(rng_next() & 1);

        /* 85% chance: keep same quantity, 15% chance: new quantity */
        if (rng_range(0, 99) < 85) {
            msgs[i].quantity = sticky_qty[id];
        } else {
            sticky_qty[id] = (uint32_t)(rng_range(1, 20) * 100);
            msgs[i].quantity = sticky_qty[id];
        }
    }
}

/* Even higher: 95% repeat (like a very liquid instrument) */
static void gen_quotes_very_sticky(Quote *msgs, int n)
{
    uint64_t ts = 34200000000000ULL;
    uint32_t sticky_qty[N_INSTR];
    for (int i = 0; i < N_INSTR; i++)
        sticky_qty[i] = (uint32_t)((1 + i % 10) * 100);

    for (int i = 0; i < n; i++) {
        ts += 500 + rng_range(0, 50000);
        int id = (int)rng_range(0, N_INSTR - 1);
        msgs[i].timestamp = ts;
        msgs[i].instrument_id = (uint16_t)id;
        msgs[i].price = 1500000 + (uint32_t)(rng_range(0, 2000)) - 1000;
        msgs[i].side = (uint8_t)(rng_next() & 1);

        if (rng_range(0, 99) < 95) {
            msgs[i].quantity = sticky_qty[id];
        } else {
            sticky_qty[id] = (uint32_t)(rng_range(1, 20) * 100);
            msgs[i].quantity = sticky_qty[id];
        }
    }
}

/* ================================================================
 * Experimental codec: quantity run-length encoding
 *
 * Strategy: add qty_repeat_count to per-instrument state. When
 * >= THRESHOLD, encode quantity differently:
 *
 * Normal mode (repeat_count < THRESHOLD):
 *   0 = same (1 bit)
 *   1 + round_flag + residual = different
 *
 * Run mode (repeat_count >= THRESHOLD):
 *   0 = same (1 bit) -- same as normal mode
 *   1 = changed (1 bit) + round_flag + residual
 *   Identical wire format! But the key difference: in run mode,
 *   the branch predictor can predict "0" with very high accuracy,
 *   which speeds up the DECODER. And for compression, we can
 *   optionally use a STRONGER prediction.
 *
 * Actually, the REAL optimization opportunity for compression:
 * Use a 2-bit GROUP code when in run mode. Instead of encoding
 * the same-flag independently for each message:
 *   GROUP code: encode how many more "same" before a change:
 *     0    = 1+ more same (1 bit, amortized over multiple messages)
 *     10   = change after 0 more same (2 bits)
 *     110  = reserved
 *     111  = reserved
 *   But this requires batching, which breaks streaming.
 *
 * SIMPLEST VIABLE WIN: just skip the same-flag entirely in run mode.
 * The decoder, when in run mode, assumes "same" without reading a bit.
 * When the encoder sees a change, it backtracks and writes:
 *   - A 1-bit escape for each "assumed same" message: impossible,
 *     those messages are already committed.
 *
 * OK, the streaming constraint is real. We cannot skip the flag.
 * But we CAN make the flag cheaper by using it as a CONTEXT for
 * an arithmetic coder... but we don't have an arithmetic coder.
 *
 * FINAL PRACTICAL APPROACH: The savings come not from the wire
 * format but from the CODE PATH. In run mode:
 * - Encoder: if same, write 0. If different, write 1 + full qty
 *   (raw, not as residual from predicted). This avoids the
 *   residual computation and round-lot check in the common case.
 * - Decoder: read 1 bit. If 0, done. If 1, read raw qty.
 *
 * For COMPRESSION improvement, the only option without arithmetic
 * coding is to change the wire format. Here's the idea:
 *
 * When repeat_count >= THRESHOLD, switch to "run-length" framing:
 *   Emit a run-length (Golomb-coded) of how many more "same" follow,
 *   then the new value. But this requires lookahead.
 *
 * ALTERNATIVE: Instead of per-message flag, use a per-INSTRUMENT
 * batch when in run mode. Every time we see this instrument:
 *   If repeat_count >= THRESHOLD:
 *     Don't emit the same-flag (save 1 bit)
 *     The decoder assumes same
 *     On change: emit a CORRECTION that includes:
 *       (a) how many messages ago the change happened (log2 encoded)
 *       (b) the new value
 *     Problem: requires rewriting history.
 *
 * MOST PRACTICAL: Just measure the bit savings and speed impact of
 * the existing approach with different data distributions. The
 * current 1-bit same-flag is already very efficient. Let's see if
 * any improvement is meaningful.
 *
 * WAIT — I just realized there IS a simple streaming improvement:
 *
 * CONFIDENCE-BASED FLAG ELISION:
 * When qty_repeat_count >= THRESHOLD for this instrument:
 *   Encoder: skip the same-flag, just encode nothing for "same"
 *   Decoder: in run mode, DON'T read a flag — assume same
 *   When the quantity CHANGES:
 *     Encoder: emits a "break" marker (the first "1" bit of the NEXT
 *     field's encoding could serve as context, but that's fragile)
 *     PROBLEM: the decoder doesn't know whether to read a flag or not
 *
 * Actually, this DOES work if we frame it correctly:
 *   In run mode: always read 1 bit.
 *     0 = same (continue run)
 *     1 = changed + payload
 *   This is IDENTICAL to the current encoding. Zero savings.
 *
 * The only way to save bits is if the probability is asymmetric
 * and we use entropy coding. With fixed-width codes, 1 bit is the
 * minimum for a binary decision.
 *
 * CONCLUSION: The savings from "quantity run encoding" with the
 * current tiered-residual framework are essentially ZERO for
 * compression ratio. The 1-bit same-flag is already optimal for
 * a non-entropy-coded binary decision.
 *
 * However, there MAY be SPEED improvements from:
 * 1. Better branch prediction (run mode flag is highly predictable)
 * 2. Skipping the residual computation and round-lot check
 *
 * Let's benchmark both the compression and speed impact.
 * ================================================================ */

#define QTY_RUN_THRESHOLD 4

/* Custom encode/decode that's identical in wire format but tracks
 * per-instrument repeat count for analysis purposes. */

typedef struct {
    uint32_t last_qty;
    uint32_t msg_count;
    uint16_t qty_repeat_count;
} QtyInstrState;

/* Measure bits used by quantity encoding across different datasets */
static void measure_qty_bits(Quote *msgs, int n, const char *label)
{
    residc_state_t enc;
    residc_init(&enc, &quote_schema);

    uint8_t buf[64];
    long total_compressed = 0;
    int raw_size = residc_raw_size(&quote_schema);

    /* Also track per-instrument qty repeat stats */
    QtyInstrState qis[N_INSTR];
    memset(qis, 0, sizeof(qis));

    long total_same_flag_bits = 0;   /* bits spent on same-flag */
    long total_qty_msgs = 0;
    long total_same = 0;
    long total_in_run = 0;           /* messages where repeat >= threshold */

    for (int i = 0; i < n; i++) {
        int id = msgs[i].instrument_id;
        if (id < N_INSTR && qis[id].msg_count > 0) {
            total_qty_msgs++;
            total_same_flag_bits++;  /* always 1 bit for the flag */

            if (msgs[i].quantity == qis[id].last_qty) {
                total_same++;
                qis[id].qty_repeat_count++;
                if (qis[id].qty_repeat_count >= QTY_RUN_THRESHOLD)
                    total_in_run++;
            } else {
                qis[id].qty_repeat_count = 0;
            }
        }
        if (id < N_INSTR) {
            qis[id].last_qty = msgs[i].quantity;
            qis[id].msg_count++;
        }

        int len = residc_encode(&enc, &msgs[i], buf, 64);
        total_compressed += len;
    }

    double ratio = (double)(n * raw_size) / total_compressed;
    double avg_compressed = (double)total_compressed / n;

    printf("\n=== %s (%d messages) ===\n", label, n);
    printf("  Compression:        %.2f:1 (%d -> %.1f bytes avg)\n",
           ratio, raw_size, avg_compressed);
    printf("  Qty same-flag:      %ld bits (%.1f%% of total compressed)\n",
           total_same_flag_bits,
           100.0 * total_same_flag_bits / (total_compressed * 8.0));
    printf("  Qty same rate:      %ld / %ld (%.1f%%)\n",
           total_same, total_qty_msgs,
           total_qty_msgs > 0 ? 100.0 * total_same / total_qty_msgs : 0.0);
    printf("  In run (>=%d reps):  %ld / %ld (%.1f%%)\n",
           QTY_RUN_THRESHOLD, total_in_run, total_qty_msgs,
           total_qty_msgs > 0 ? 100.0 * total_in_run / total_qty_msgs : 0.0);
    printf("  Potential savings:  If we could elide the same-flag in run mode,\n");
    printf("    save %ld bits = %.0f bytes = %.3f bytes/msg\n",
           total_in_run, total_in_run / 8.0,
           total_qty_msgs > 0 ? total_in_run / 8.0 / n : 0.0);
    if (total_in_run > 0) {
        double new_total = total_compressed - total_in_run / 8.0;
        double new_ratio = (double)(n * raw_size) / new_total;
        printf("    Would improve ratio: %.2f:1 -> %.2f:1 (%.1f%% improvement)\n",
               ratio, new_ratio, 100.0 * (new_ratio - ratio) / ratio);
    }
}

/* ================================================================
 * Speed benchmark: current vs run-aware decode
 * ================================================================ */

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

static void bench_speed(Quote *msgs, int n, const char *label)
{
    residc_state_t enc, dec;
    uint8_t buf[64];
    int raw_size = residc_raw_size(&quote_schema);

    /* Pre-encode */
    uint8_t (*bufs)[64] = malloc(n * 64);
    int *lens = malloc(n * sizeof(int));
    residc_init(&enc, &quote_schema);
    long total_compressed = 0;
    for (int i = 0; i < n; i++) {
        lens[i] = residc_encode(&enc, &msgs[i], bufs[i], 64);
        total_compressed += lens[i];
    }

    /* Encode benchmark */
    double best_enc = 1e18;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(&enc, &quote_schema);
        double t0 = now_ns();
        for (int i = 0; i < n; i++)
            residc_encode(&enc, &msgs[i], buf, 64);
        double t1 = now_ns();
        double ns = (t1 - t0) / n;
        if (ns < best_enc) best_enc = ns;
    }

    /* Decode benchmark */
    double best_dec = 1e18;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_init(&dec, &quote_schema);
        Quote decoded;
        double t0 = now_ns();
        for (int i = 0; i < n; i++)
            residc_decode(&dec, bufs[i], lens[i], &decoded);
        double t1 = now_ns();
        double ns = (t1 - t0) / n;
        if (ns < best_dec) best_dec = ns;
    }

    /* Verify */
    residc_init(&enc, &quote_schema);
    residc_init(&dec, &quote_schema);
    int errors = 0;
    for (int i = 0; i < n; i++) {
        int len = residc_encode(&enc, &msgs[i], buf, 64);
        Quote decoded;
        residc_decode(&dec, buf, len, &decoded);
        if (memcmp(&msgs[i], &decoded, sizeof(Quote)) != 0) errors++;
    }

    printf("\n  %s performance:\n", label);
    printf("    Encode:  %.0f ns/msg\n", best_enc);
    printf("    Decode:  %.0f ns/msg\n", best_dec);
    printf("    Ratio:   %.2f:1 (%d -> %.1f bytes avg)\n",
           (double)(n * raw_size) / total_compressed,
           raw_size, (double)total_compressed / n);
    printf("    Errors:  %d\n", errors);

    free(bufs);
    free(lens);
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void)
{
    printf("============================================================\n");
    printf("Experiment: Per-Instrument Quantity Zero-Run Encoding\n");
    printf("============================================================\n");

    Quote *quotes_random = malloc(N_MSGS * sizeof(Quote));
    Quote *quotes_realistic = malloc(N_MSGS * sizeof(Quote));
    Quote *quotes_sticky = malloc(N_MSGS * sizeof(Quote));

    /* Generate three datasets with different repeat characteristics */
    rng_state = 12345678901ULL;
    {
        /* Random (from bench_compression.c) — low repeat rate */
        uint64_t ts = 34200000000000ULL;
        for (int i = 0; i < N_MSGS; i++) {
            ts += 500 + rng_range(0, 50000);
            quotes_random[i].timestamp = ts;
            quotes_random[i].instrument_id = (uint16_t)rng_range(0, 49);
            quotes_random[i].price = 1500000 + (uint32_t)(rng_range(0, 2000)) - 1000;
            quotes_random[i].quantity = (uint32_t)(rng_range(1, 20) * 100);
            quotes_random[i].side = (uint8_t)(rng_next() & 1);
        }
    }

    rng_state = 12345678901ULL;
    gen_quotes_realistic(quotes_realistic, N_MSGS);

    rng_state = 12345678901ULL;
    gen_quotes_very_sticky(quotes_sticky, N_MSGS);

    /* Phase 1: Analyze repeat patterns */
    printf("\n");
    rng_state = 12345678901ULL;
    analyze_repeats(quotes_random, N_MSGS);

    rng_state = 12345678901ULL;
    analyze_repeats(quotes_realistic, N_MSGS);

    rng_state = 12345678901ULL;
    analyze_repeats(quotes_sticky, N_MSGS);

    /* Phase 2: Measure compression impact */
    measure_qty_bits(quotes_random, N_MSGS, "Random quantities (bench_compression.c style)");
    measure_qty_bits(quotes_realistic, N_MSGS, "Realistic quantities (85% repeat)");
    measure_qty_bits(quotes_sticky, N_MSGS, "Very sticky quantities (95% repeat)");

    /* Phase 3: Speed benchmark */
    printf("\n=== Speed Benchmarks ===\n");
    bench_speed(quotes_random, N_MSGS, "Random");
    bench_speed(quotes_realistic, N_MSGS, "Realistic (85%% repeat)");
    bench_speed(quotes_sticky, N_MSGS, "Very sticky (95%% repeat)");

    /* Phase 4: Conclusions */
    printf("\n============================================================\n");
    printf("Conclusions\n");
    printf("============================================================\n");
    printf("With the current tiered-residual framework (no arithmetic coding),\n");
    printf("a binary same/different flag costs exactly 1 bit, which is the\n");
    printf("Shannon-optimal minimum for any non-trivial binary decision.\n");
    printf("\n");
    printf("Quantity run-length encoding CANNOT improve compression ratio\n");
    printf("without switching to entropy coding (arithmetic/ANS) for the\n");
    printf("same-flag, which would allow encoding a 95%% probable event\n");
    printf("in ~0.07 bits instead of 1 bit (14x improvement on that flag).\n");
    printf("\n");
    printf("The only benefit of tracking qty_repeat_count is potential\n");
    printf("SPEED improvement from better branch prediction hints and\n");
    printf("skipping unnecessary residual computation on the same path.\n");

    free(quotes_random);
    free(quotes_realistic);
    free(quotes_sticky);

    return 0;
}
