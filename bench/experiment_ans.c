/*
 * experiment_ans.c -- rANS residual coding experiment
 *
 * Compares table-based rANS (range Asymmetric Numeral Systems) against the
 * current 5-tier prefix coding for residual values.
 *
 * rANS bucket symbols:
 *   sym 0: zz == 0             (exact prediction)
 *   sym 1: zz in [1, 3]        (2 bits offset)
 *   sym 2: zz in [4, 15]       (4 bits offset -- was 3.58)
 *   sym 3: zz in [16, 63]      (6 bits offset -- was 5.58)
 *   sym 4: zz in [64, 255]     (8 bits offset -- was 7.58)
 *   sym 5: zz in [256, 4095]   (12 bits offset)
 *   sym 6: zz in [4096, 65535] (16 bits offset)
 *   sym 7: zz >= 65536          (raw 64-bit escape)
 *
 * After encoding the symbol via rANS, the offset within the bucket is
 * appended as raw bits.
 *
 * Build:
 *   cc -O2 -march=native -o experiment_ans bench/experiment_ans.c core/residc.c -Icore
 *
 * Run:
 *   ./experiment_ans
 */

#define _POSIX_C_SOURCE 199309L
#include "residc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ================================================================
 * rANS implementation (table-based, 8-bit precision)
 * ================================================================ */

#define RANS_PRECISION_BITS  8
#define RANS_PRECISION       (1u << RANS_PRECISION_BITS)  /* 256 */
#define RANS_NUM_SYMBOLS     8

/* Bucket boundaries: bucket i covers [bucket_lo[i], bucket_lo[i+1]) */
static const uint64_t bucket_lo[RANS_NUM_SYMBOLS + 1] = {
    0, 1, 4, 16, 64, 256, 4096, 65536, UINT64_MAX
};

/* Offset bits per bucket (to encode position within bucket) */
static const int bucket_offset_bits[RANS_NUM_SYMBOLS] = {
    0,   /* sym 0: zz==0, no offset */
    2,   /* sym 1: [1,3], 2 bits (range 3 -> ceil(log2(3))=2) */
    4,   /* sym 2: [4,15], 4 bits (range 12 -> ceil(log2(12))=4) */
    6,   /* sym 3: [16,63], 6 bits (range 48 -> ceil(log2(48))=6) */
    8,   /* sym 4: [64,255], 8 bits (range 192 -> ceil(log2(192))=8) */
    12,  /* sym 5: [256,4095], 12 bits */
    16,  /* sym 6: [4096,65535], 16 bits */
    64,  /* sym 7: raw 64-bit escape */
};

/* rANS symbol table entry: cumulative freq and freq */
typedef struct {
    uint16_t start;   /* cumulative frequency (sum of freqs of symbols < this) */
    uint16_t freq;    /* frequency of this symbol (sum of all = RANS_PRECISION) */
} rans_sym_t;

/* rANS probability table */
typedef struct {
    rans_sym_t syms[RANS_NUM_SYMBOLS];
    /* Decode table: maps cumulative position -> symbol index */
    uint8_t    cum2sym[RANS_PRECISION];
} rans_table_t;

/* Build decode LUT from symbol table */
static void rans_build_lut(rans_table_t *tab)
{
    for (int s = 0; s < RANS_NUM_SYMBOLS; s++) {
        for (uint16_t j = 0; j < tab->syms[s].freq; j++) {
            tab->cum2sym[tab->syms[s].start + j] = (uint8_t)s;
        }
    }
}

/* Static probability table for financial residuals.
 * Most residuals are 0 or small. Tuned for typical price/qty/ts data. */
static void rans_init_static(rans_table_t *tab)
{
    /*
     * Distribution estimate (financial data):
     *   sym 0 (zz==0):       ~40%  -> 102/256
     *   sym 1 (1..3):        ~25%  ->  64/256
     *   sym 2 (4..15):       ~15%  ->  38/256
     *   sym 3 (16..63):      ~10%  ->  26/256
     *   sym 4 (64..255):      ~5%  ->  13/256
     *   sym 5 (256..4095):    ~3%  ->   8/256
     *   sym 6 (4096..65535):  ~1.5% ->  4/256
     *   sym 7 (>=65536):     ~0.5% ->  1/256
     *                                 256/256
     */
    static const uint16_t freqs[RANS_NUM_SYMBOLS] = {
        102, 64, 38, 26, 13, 8, 4, 1
    };
    uint16_t cum = 0;
    for (int i = 0; i < RANS_NUM_SYMBOLS; i++) {
        tab->syms[i].start = cum;
        tab->syms[i].freq  = freqs[i];
        cum += freqs[i];
    }
    rans_build_lut(tab);
}

/* Build table from training data (frequency counts per bucket) */
static void rans_init_trained(rans_table_t *tab, const uint64_t counts[RANS_NUM_SYMBOLS])
{
    uint64_t total = 0;
    for (int i = 0; i < RANS_NUM_SYMBOLS; i++)
        total += counts[i];
    if (total == 0) { rans_init_static(tab); return; }

    /* Scale counts to sum to RANS_PRECISION, ensuring each symbol has freq >= 1 */
    uint16_t freqs[RANS_NUM_SYMBOLS];
    int assigned = 0;
    for (int i = 0; i < RANS_NUM_SYMBOLS; i++) {
        if (counts[i] > 0) {
            freqs[i] = (uint16_t)((counts[i] * RANS_PRECISION + total / 2) / total);
            if (freqs[i] < 1) freqs[i] = 1;
        } else {
            freqs[i] = 1;  /* minimum 1 to avoid division by zero */
        }
        assigned += freqs[i];
    }

    /* Correct to exactly RANS_PRECISION by adjusting the largest bucket */
    int max_idx = 0;
    for (int i = 1; i < RANS_NUM_SYMBOLS; i++)
        if (freqs[i] > freqs[max_idx]) max_idx = i;

    int diff = assigned - (int)RANS_PRECISION;
    freqs[max_idx] = (uint16_t)((int)freqs[max_idx] - diff);
    if (freqs[max_idx] < 1) freqs[max_idx] = 1;

    uint16_t cum = 0;
    for (int i = 0; i < RANS_NUM_SYMBOLS; i++) {
        tab->syms[i].start = cum;
        tab->syms[i].freq  = freqs[i];
        cum += freqs[i];
    }
    rans_build_lut(tab);
}

/* ================================================================
 * rANS encoder (writes to a byte buffer in reverse)
 *
 * rANS encodes in reverse: last symbol encoded is first decoded.
 * We accumulate into a 32-bit state and renormalize by pushing
 * bytes to the output.
 * ================================================================ */

#define RANS_L  (1u << 23)  /* lower bound of rANS state */

typedef struct {
    uint32_t state;
    uint8_t  buf[512];  /* output buffer (filled from end) */
    int      pos;       /* current write position (starts at end, goes down) */
    /* Extra raw bits buffer (written forward, appended after rANS bytes) */
    uint8_t  raw_buf[512];
    int      raw_pos;
    uint64_t raw_accum;
    int      raw_bits;
} rans_encoder_t;

static void rans_enc_init(rans_encoder_t *enc)
{
    enc->state = RANS_L;
    enc->pos = (int)sizeof(enc->buf);
    enc->raw_pos = 0;
    enc->raw_accum = 0;
    enc->raw_bits = 0;
}

/* Push raw bits (for offsets within buckets) */
static void rans_enc_raw_bits(rans_encoder_t *enc, uint64_t val, int nbits)
{
    if (nbits == 0) return;
    enc->raw_accum = (enc->raw_accum << nbits) | (val & ((1ULL << nbits) - 1));
    enc->raw_bits += nbits;
    while (enc->raw_bits >= 8) {
        enc->raw_bits -= 8;
        if (enc->raw_pos < (int)sizeof(enc->raw_buf))
            enc->raw_buf[enc->raw_pos++] = (uint8_t)(enc->raw_accum >> enc->raw_bits);
    }
}

/* Flush remaining raw bits */
static void rans_enc_raw_flush(rans_encoder_t *enc)
{
    if (enc->raw_bits > 0 && enc->raw_pos < (int)sizeof(enc->raw_buf)) {
        enc->raw_buf[enc->raw_pos++] =
            (uint8_t)(enc->raw_accum << (8 - enc->raw_bits));
    }
}

/* Encode a single symbol */
static void rans_enc_put(rans_encoder_t *enc, const rans_table_t *tab, int sym)
{
    uint32_t x = enc->state;
    uint32_t freq = tab->syms[sym].freq;
    uint32_t start = tab->syms[sym].start;

    /* Renormalize: push bytes until state is in the correct range */
    uint32_t x_max = ((RANS_L >> RANS_PRECISION_BITS) << 8) * freq;
    while (x >= x_max) {
        if (enc->pos > 0)
            enc->buf[--enc->pos] = (uint8_t)(x & 0xFF);
        x >>= 8;
    }

    /* rANS encode step: x' = (x / freq) * M + start + (x % freq) */
    enc->state = ((x / freq) << RANS_PRECISION_BITS) + start + (x % freq);
}

/* Finalize: flush the rANS state (4 bytes, big-endian) and return total size */
static int rans_enc_finish(rans_encoder_t *enc, uint8_t *out, int capacity)
{
    /* Flush remaining raw bits */
    rans_enc_raw_flush(enc);

    /* Push final rANS state (4 bytes) */
    uint32_t x = enc->state;
    for (int i = 0; i < 4; i++) {
        if (enc->pos > 0)
            enc->buf[--enc->pos] = (uint8_t)(x & 0xFF);
        x >>= 8;
    }

    int rans_bytes = (int)sizeof(enc->buf) - enc->pos;
    int total = 1 + rans_bytes + enc->raw_pos;  /* 1 byte for rans_bytes count */
    if (total > capacity) return -1;

    /* Output format: [rans_byte_count] [rans_bytes...] [raw_bytes...] */
    out[0] = (uint8_t)rans_bytes;
    memcpy(out + 1, enc->buf + enc->pos, rans_bytes);
    memcpy(out + 1 + rans_bytes, enc->raw_buf, enc->raw_pos);
    return total;
}

/* ================================================================
 * rANS decoder
 * ================================================================ */

typedef struct {
    uint32_t state;
    const uint8_t *buf;
    int pos;
    int len;
    /* Raw bits reader */
    const uint8_t *raw_buf;
    int raw_pos;
    int raw_len;
    uint64_t raw_accum;
    int raw_bits;
} rans_decoder_t;

static void rans_dec_init(rans_decoder_t *dec, const uint8_t *data, int data_len)
{
    memset(dec, 0, sizeof(*dec));
    if (data_len < 1) return;

    int rans_bytes = data[0];
    if (1 + rans_bytes > data_len) return;

    /* Read initial rANS state (4 bytes big-endian from start of rans_bytes) */
    dec->buf = data + 1;
    dec->pos = 0;
    dec->len = rans_bytes;

    dec->state = 0;
    for (int i = 0; i < 4 && dec->pos < dec->len; i++) {
        dec->state = (dec->state << 8) | dec->buf[dec->pos++];
    }

    /* Raw bits start after rANS bytes */
    dec->raw_buf = data + 1 + rans_bytes;
    dec->raw_len = data_len - 1 - rans_bytes;
    dec->raw_pos = 0;
    dec->raw_accum = 0;
    dec->raw_bits = 0;
}

/* Read raw bits (for offsets within buckets) */
static uint64_t rans_dec_raw_bits(rans_decoder_t *dec, int nbits)
{
    if (nbits == 0) return 0;
    while (dec->raw_bits < nbits && dec->raw_pos < dec->raw_len) {
        dec->raw_accum = (dec->raw_accum << 8) | dec->raw_buf[dec->raw_pos++];
        dec->raw_bits += 8;
    }
    dec->raw_bits -= nbits;
    return (dec->raw_accum >> dec->raw_bits) & ((1ULL << nbits) - 1);
}

/* Decode a single symbol */
static int rans_dec_get(rans_decoder_t *dec, const rans_table_t *tab)
{
    uint32_t x = dec->state;

    /* Extract the symbol from cumulative frequency */
    uint32_t cum = x & (RANS_PRECISION - 1);
    int sym = tab->cum2sym[cum];

    uint32_t freq = tab->syms[sym].freq;
    uint32_t start = tab->syms[sym].start;

    /* rANS decode step: x' = freq * (x >> M_bits) + (x & M_mask) - start */
    x = freq * (x >> RANS_PRECISION_BITS) + cum - start;

    /* Renormalize: pull bytes */
    while (x < RANS_L && dec->pos < dec->len) {
        x = (x << 8) | dec->buf[dec->pos++];
    }

    dec->state = x;
    return sym;
}

/* ================================================================
 * rANS residual coding (encode/decode a signed residual)
 * ================================================================ */

/* Classify zigzag value into bucket symbol */
static inline int zz_to_sym(uint64_t zz)
{
    if (zz == 0)      return 0;
    if (zz <= 3)      return 1;
    if (zz <= 15)     return 2;
    if (zz <= 63)     return 3;
    if (zz <= 255)    return 4;
    if (zz <= 4095)   return 5;
    if (zz <= 65535)  return 6;
    return 7;
}

/*
 * Since rANS encodes in reverse but we need to encode multiple residuals
 * in order, we collect all symbols+offsets first, then encode them in
 * reverse order.
 */

#define MAX_RESIDUALS_PER_MSG 16

typedef struct {
    int sym;
    uint64_t offset;      /* offset within bucket */
    int offset_bits;      /* number of bits for offset */
    int64_t raw_value;    /* for sym 7: the original signed value */
} residual_entry_t;

typedef struct {
    residual_entry_t entries[MAX_RESIDUALS_PER_MSG];
    int count;
} residual_list_t;

static void rlist_init(residual_list_t *rl) { rl->count = 0; }

static void rlist_add(residual_list_t *rl, int64_t value)
{
    if (rl->count >= MAX_RESIDUALS_PER_MSG) return;
    residual_entry_t *e = &rl->entries[rl->count++];

    uint64_t zz = residc_zigzag_enc(value);
    e->sym = zz_to_sym(zz);
    e->offset_bits = bucket_offset_bits[e->sym];

    if (e->sym == 7) {
        /* Raw escape: store full 64-bit signed value */
        e->offset = 0;
        e->raw_value = value;
    } else if (e->sym == 0) {
        e->offset = 0;
        e->raw_value = 0;
    } else {
        e->offset = zz - bucket_lo[e->sym];
        e->raw_value = 0;
    }
}

/* Encode all residuals via rANS.
 * Returns encoded size, or -1 on error. */
static int rans_encode_residuals(const rans_table_t *tab,
                                 const residual_list_t *rl,
                                 uint8_t *out, int capacity)
{
    rans_encoder_t enc;
    rans_enc_init(&enc);

    /* Write raw bits FORWARD (offsets in order) */
    for (int i = 0; i < rl->count; i++) {
        const residual_entry_t *e = &rl->entries[i];
        if (e->sym == 7) {
            /* 64-bit raw: split into hi/lo for safety */
            uint64_t v = (uint64_t)e->raw_value;
            rans_enc_raw_bits(&enc, v >> 32, 32);
            rans_enc_raw_bits(&enc, v & 0xFFFFFFFF, 32);
        } else {
            rans_enc_raw_bits(&enc, e->offset, e->offset_bits);
        }
    }

    /* Encode symbols in REVERSE order (rANS requirement) */
    for (int i = rl->count - 1; i >= 0; i--) {
        rans_enc_put(&enc, tab, rl->entries[i].sym);
    }

    return rans_enc_finish(&enc, out, capacity);
}

/* Decode all residuals from rANS stream */
static void rans_decode_residuals(const rans_table_t *tab,
                                  const uint8_t *data, int data_len,
                                  int64_t *residuals, int n_residuals)
{
    rans_decoder_t dec;
    rans_dec_init(&dec, data, data_len);

    for (int i = 0; i < n_residuals; i++) {
        int sym = rans_dec_get(&dec, tab);

        if (sym == 0) {
            residuals[i] = 0;
        } else if (sym == 7) {
            uint64_t hi = rans_dec_raw_bits(&dec, 32);
            uint64_t lo = rans_dec_raw_bits(&dec, 32);
            residuals[i] = (int64_t)((hi << 32) | lo);
        } else {
            uint64_t offset = rans_dec_raw_bits(&dec, bucket_offset_bits[sym]);
            uint64_t zz = bucket_lo[sym] + offset;
            residuals[i] = residc_zigzag_dec(zz);
        }
    }
}

/* ================================================================
 * Deterministic RNG (same as bench_compression.c)
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
 * Message types (same as bench_compression.c)
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
 * Extract residuals from encoded messages
 *
 * We intercept the prediction phase to collect the actual residuals
 * that residc would compute, then code them with both tiered and rANS.
 * ================================================================ */

typedef struct {
    int64_t  *residuals;      /* array of residuals per message */
    int      *n_residuals;    /* count of residuals per message */
    int       max_per_msg;
    int       n_msgs;
} residual_trace_t;

/*
 * Extract residuals by encoding each message and measuring the
 * residual values that encode_residual would see. We do this by
 * running the full encoder and then analyzing the state deltas.
 *
 * For a cleaner approach: we replicate the prediction logic and compute
 * residuals directly from the message fields and codec state.
 */
static void extract_residuals_generic(
    const residc_schema_t *schema, const void *msgs, int n,
    residual_trace_t *trace)
{
    int msg_size = schema->msg_size;
    /* We need codec state to compute predictions */
    residc_state_t *state = calloc(1, sizeof(residc_state_t));
    residc_init(state, schema);

    for (int i = 0; i < n; i++) {
        const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
        int nr = 0;
        int64_t *res = &trace->residuals[i * trace->max_per_msg];

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
                uint64_t ts = val;
                int64_t gap = (int64_t)(ts - state->last_timestamp);
                int64_t predicted_gap = state->timestamp_gap_ema >> 16;
                if (predicted_gap < 0) predicted_gap = 0;
                res[nr++] = gap - predicted_gap;
                break;
            }
            case RESIDC_INSTRUMENT:
                instrument_id = (uint16_t)val;
                is = (instrument_id < RESIDC_MAX_INSTRUMENTS)
                     ? &state->instruments[instrument_id] : NULL;
                break;
            case RESIDC_PRICE: {
                uint32_t price = (uint32_t)val;
                uint32_t predicted = (is && is->msg_count > 0) ? is->last_price : 0;
                int64_t residual = (int64_t)price - (int64_t)predicted;
                if (price % 100 == 0 && predicted % 100 == 0 &&
                    (price > 0 || predicted > 0))
                    res[nr++] = residual / 100;
                else
                    res[nr++] = residual;
                break;
            }
            case RESIDC_QUANTITY: {
                uint32_t qty = (uint32_t)val;
                uint32_t predicted = (is && is->msg_count > 0) ? is->last_qty : 100;
                int64_t residual = (int64_t)qty - (int64_t)predicted;
                if (residual != 0) {
                    if (qty % 100 == 0 && predicted % 100 == 0)
                        res[nr++] = residual / 100;
                    else
                        res[nr++] = residual;
                }
                break;
            }
            case RESIDC_SEQUENTIAL_ID: {
                uint64_t id = val;
                residc_field_state_t *fs = &state->field_state[fi];
                uint64_t predicted = (is && is->last_seq_id > 0)
                                   ? is->last_seq_id : fs->last_value;
                res[nr++] = (int64_t)(id - predicted);
                break;
            }
            case RESIDC_DELTA_ID: {
                /* Simplified: just record the raw delta */
                res[nr++] = (int64_t)val;
                break;
            }
            case RESIDC_DELTA_PRICE: {
                res[nr++] = (int64_t)val;
                break;
            }
            default:
                break;
            }
        }

        trace->n_residuals[i] = nr;

        /* Advance codec state using the real encoder */
        uint8_t buf[128];
        residc_encode(state, msg, buf, 128);
    }

    free(state);
}

/* ================================================================
 * Measure bits used by tiered coding for a set of residuals
 * ================================================================ */

static int tiered_bits_for_residual(int64_t value, int k)
{
    uint64_t zz = residc_zigzag_enc(value);
    if (zz < (1ULL << k))        return 1 + k;
    if (zz < (1ULL << (k + 2)))  return 2 + (k + 2);
    if (zz < (1ULL << (k + 5)))  return 3 + (k + 5);
    if (zz < (1ULL << (k + 10))) return 4 + (k + 10);
    return 4 + 64;  /* tier 4: 4 prefix + 64 raw */
}

/* ================================================================
 * Main benchmark
 * ================================================================ */

#define N_MSGS    100000
#define N_TRAIN   1000
#define N_ITERS   10
#define MAX_RESID 12  /* max residuals per message */

typedef struct {
    const char *name;
    int raw_bytes;
    double tiered_total_bits;
    double rans_static_total_bytes;
    double rans_trained_total_bytes;
    double tiered_bits_per_msg;
    double rans_static_bits_per_msg;
    double rans_trained_bits_per_msg;
    /* Speed */
    double enc_tiered_ns;
    double dec_tiered_ns;
    double enc_rans_ns;
    double dec_rans_ns;
    int roundtrip_errors;
} experiment_result_t;

static struct timespec ts_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}

static double ts_diff_ns(struct timespec t0, struct timespec t1) {
    return (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
}

static void run_experiment(
    const residc_schema_t *schema, const void *msgs, int n,
    const char *name, experiment_result_t *result)
{
    int msg_size = schema->msg_size;
    int raw_size = residc_raw_size(schema);

    /* 1. Extract residuals */
    residual_trace_t trace;
    trace.max_per_msg = MAX_RESID;
    trace.n_msgs = n;
    trace.residuals = calloc((size_t)n * MAX_RESID, sizeof(int64_t));
    trace.n_residuals = calloc(n, sizeof(int));
    extract_residuals_generic(schema, msgs, n, &trace);

    /* 2. Compute tiered coding cost (bits) */
    int k = 3;  /* typical k for CALM regime */
    double tiered_total = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < trace.n_residuals[i]; j++) {
            tiered_total += tiered_bits_for_residual(
                trace.residuals[i * MAX_RESID + j], k);
        }
    }

    /* 3. Train rANS table on first N_TRAIN messages */
    uint64_t train_counts[RANS_NUM_SYMBOLS] = {0};
    for (int i = 0; i < N_TRAIN && i < n; i++) {
        for (int j = 0; j < trace.n_residuals[i]; j++) {
            uint64_t zz = residc_zigzag_enc(trace.residuals[i * MAX_RESID + j]);
            train_counts[zz_to_sym(zz)]++;
        }
    }

    rans_table_t tab_static, tab_trained;
    rans_init_static(&tab_static);
    rans_init_trained(&tab_trained, train_counts);

    /* 4. Encode all residuals with rANS (static + trained), measure size */
    double rans_static_total = 0;
    double rans_trained_total = 0;
    int roundtrip_errors = 0;

    for (int i = 0; i < n; i++) {
        int nr = trace.n_residuals[i];
        if (nr == 0) continue;

        int64_t *res = &trace.residuals[i * MAX_RESID];

        /* Build residual list */
        residual_list_t rl;
        rlist_init(&rl);
        for (int j = 0; j < nr; j++)
            rlist_add(&rl, res[j]);

        /* Static table */
        uint8_t buf[512];
        int slen = rans_encode_residuals(&tab_static, &rl, buf, sizeof(buf));
        if (slen > 0) {
            rans_static_total += slen;
            /* Verify roundtrip */
            int64_t decoded[MAX_RESID];
            rans_decode_residuals(&tab_static, buf, slen, decoded, nr);
            for (int j = 0; j < nr; j++) {
                if (decoded[j] != res[j]) roundtrip_errors++;
            }
        }

        /* Trained table */
        int tlen = rans_encode_residuals(&tab_trained, &rl, buf, sizeof(buf));
        if (tlen > 0) rans_trained_total += tlen;
    }

    /* 5. Speed benchmark: tiered encode/decode */
    double best_enc_tiered = 1e9, best_dec_tiered = 1e9;
    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_state_t enc_state;
        residc_init(&enc_state, schema);
        struct timespec t0 = ts_now();
        for (int i = 0; i < n; i++) {
            const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
            uint8_t buf[128];
            residc_encode(&enc_state, msg, buf, 128);
        }
        struct timespec t1 = ts_now();
        double ns = ts_diff_ns(t0, t1) / n;
        if (ns < best_enc_tiered) best_enc_tiered = ns;
    }

    /* Pre-encode for decode speed test */
    int *lens = malloc(n * sizeof(int));
    uint8_t (*bufs)[128] = malloc((size_t)n * 128);
    {
        residc_state_t enc_state;
        residc_init(&enc_state, schema);
        for (int i = 0; i < n; i++) {
            const uint8_t *msg = (const uint8_t *)msgs + (size_t)i * msg_size;
            lens[i] = residc_encode(&enc_state, msg, bufs[i], 128);
        }
    }

    for (int iter = 0; iter < N_ITERS; iter++) {
        residc_state_t dec_state;
        residc_init(&dec_state, schema);
        uint8_t decoded[256];
        struct timespec t0 = ts_now();
        for (int i = 0; i < n; i++) {
            residc_decode(&dec_state, bufs[i], lens[i], decoded);
        }
        struct timespec t1 = ts_now();
        double ns = ts_diff_ns(t0, t1) / n;
        if (ns < best_dec_tiered) best_dec_tiered = ns;
    }

    /* 6. Speed benchmark: rANS encode/decode */
    double best_enc_rans = 1e9, best_dec_rans = 1e9;

    for (int iter = 0; iter < N_ITERS; iter++) {
        struct timespec t0 = ts_now();
        for (int i = 0; i < n; i++) {
            int nr = trace.n_residuals[i];
            if (nr == 0) continue;
            int64_t *res = &trace.residuals[i * MAX_RESID];
            residual_list_t rl;
            rlist_init(&rl);
            for (int j = 0; j < nr; j++)
                rlist_add(&rl, res[j]);
            uint8_t buf[512];
            rans_encode_residuals(&tab_trained, &rl, buf, sizeof(buf));
        }
        struct timespec t1 = ts_now();
        double ns = ts_diff_ns(t0, t1) / n;
        if (ns < best_enc_rans) best_enc_rans = ns;
    }

    /* Pre-encode rANS for decode benchmark */
    uint8_t (*rans_bufs)[512] = malloc((size_t)n * 512);
    int *rans_lens = malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) {
        int nr = trace.n_residuals[i];
        if (nr == 0) { rans_lens[i] = 0; continue; }
        int64_t *res = &trace.residuals[i * MAX_RESID];
        residual_list_t rl;
        rlist_init(&rl);
        for (int j = 0; j < nr; j++)
            rlist_add(&rl, res[j]);
        rans_lens[i] = rans_encode_residuals(&tab_trained, &rl, rans_bufs[i], 512);
    }

    for (int iter = 0; iter < N_ITERS; iter++) {
        struct timespec t0 = ts_now();
        for (int i = 0; i < n; i++) {
            int nr = trace.n_residuals[i];
            if (nr == 0 || rans_lens[i] <= 0) continue;
            int64_t decoded[MAX_RESID];
            rans_decode_residuals(&tab_trained, rans_bufs[i], rans_lens[i], decoded, nr);
        }
        struct timespec t1 = ts_now();
        double ns = ts_diff_ns(t0, t1) / n;
        if (ns < best_dec_rans) best_dec_rans = ns;
    }

    /* Fill result */
    result->name = name;
    result->raw_bytes = raw_size;
    result->tiered_total_bits = tiered_total;
    result->rans_static_total_bytes = rans_static_total;
    result->rans_trained_total_bytes = rans_trained_total;
    result->tiered_bits_per_msg = tiered_total / n;
    result->rans_static_bits_per_msg = (rans_static_total * 8.0) / n;
    result->rans_trained_bits_per_msg = (rans_trained_total * 8.0) / n;
    result->enc_tiered_ns = best_enc_tiered;
    result->dec_tiered_ns = best_dec_tiered;
    result->enc_rans_ns = best_enc_rans;
    result->dec_rans_ns = best_dec_rans;
    result->roundtrip_errors = roundtrip_errors;

    free(trace.residuals);
    free(trace.n_residuals);
    free(lens);
    free(bufs);
    free(rans_bufs);
    free(rans_lens);
}

/* Print rANS probability table */
static void print_table(const char *label, const rans_table_t *tab)
{
    printf("  %s:\n", label);
    for (int i = 0; i < RANS_NUM_SYMBOLS; i++) {
        double pct = 100.0 * tab->syms[i].freq / RANS_PRECISION;
        printf("    sym %d: freq=%3u  start=%3u  (%5.1f%%)\n",
               i, tab->syms[i].freq, tab->syms[i].start, pct);
    }
}

/* Print training distribution */
static void print_training_dist(const char *name, const uint64_t counts[RANS_NUM_SYMBOLS])
{
    uint64_t total = 0;
    for (int i = 0; i < RANS_NUM_SYMBOLS; i++) total += counts[i];
    if (total == 0) return;
    printf("  %s training distribution (%llu total residuals):\n",
           name, (unsigned long long)total);
    const char *labels[] = {
        "zz==0", "[1,3]", "[4,15]", "[16,63]",
        "[64,255]", "[256,4095]", "[4096,65535]", ">=65536"
    };
    for (int i = 0; i < RANS_NUM_SYMBOLS; i++) {
        printf("    sym %d %-12s: %7llu (%5.1f%%)\n",
               i, labels[i], (unsigned long long)counts[i],
               100.0 * counts[i] / total);
    }
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
    printf("rANS vs Tiered Residual Coding Experiment\n");
    printf("=======================================================================\n");
    printf("%d messages per type, best of %d iterations\n\n", N_MSGS, N_ITERS);

    /* Show static rANS table */
    rans_table_t tab_static;
    rans_init_static(&tab_static);
    printf("Static rANS probability table:\n");
    print_table("Static", &tab_static);
    printf("\n");

    /* Show training distributions */
    {
        residual_trace_t trace;
        trace.max_per_msg = MAX_RESID;
        trace.n_msgs = N_TRAIN;
        trace.residuals = calloc((size_t)N_TRAIN * MAX_RESID, sizeof(int64_t));
        trace.n_residuals = calloc(N_TRAIN, sizeof(int));

        printf("Training distributions (first %d messages):\n", N_TRAIN);

        rng_state = 12345678901ULL;
        Quote *q = malloc(N_TRAIN * sizeof(Quote));
        gen_quotes(q, N_TRAIN);
        extract_residuals_generic(&quote_schema, q, N_TRAIN, &trace);
        uint64_t counts[RANS_NUM_SYMBOLS] = {0};
        for (int i = 0; i < N_TRAIN; i++)
            for (int j = 0; j < trace.n_residuals[i]; j++) {
                uint64_t zz = residc_zigzag_enc(trace.residuals[i * MAX_RESID + j]);
                counts[zz_to_sym(zz)]++;
            }
        print_training_dist("Quote", counts);

        rans_table_t tab_q;
        rans_init_trained(&tab_q, counts);
        print_table("Trained Quote table", &tab_q);

        free(q);
        free(trace.residuals);
        free(trace.n_residuals);
        printf("\n");
    }

    /* Run experiments */
    experiment_result_t results[4];

    rng_state = 12345678901ULL;
    gen_quotes(quotes, N_MSGS);
    gen_trades(trades, N_MSGS);
    gen_orders(orders, N_MSGS);
    gen_book_updates(books, N_MSGS);

    run_experiment(&quote_schema, quotes, N_MSGS, "Quote", &results[0]);
    run_experiment(&trade_schema, trades, N_MSGS, "Trade", &results[1]);
    run_experiment(&order_schema, orders, N_MSGS, "Order", &results[2]);
    run_experiment(&book_schema, books, N_MSGS, "Book Update", &results[3]);

    /* Print results */
    printf("=======================================================================\n");
    printf("COMPRESSION COMPARISON (residual coding only, bits per message)\n");
    printf("=======================================================================\n\n");

    printf("%-14s  %8s  %8s  %8s  %8s  %8s\n",
           "Message", "Tiered", "rANS-S", "rANS-T", "S-delta", "T-delta");
    printf("%-14s  %8s  %8s  %8s  %8s  %8s\n",
           "--------------", "--------", "--------", "--------", "--------", "--------");

    for (int i = 0; i < 4; i++) {
        experiment_result_t *r = &results[i];
        double t_bpm = r->tiered_bits_per_msg;
        double s_bpm = r->rans_static_bits_per_msg;
        double tr_bpm = r->rans_trained_bits_per_msg;
        double s_delta = ((s_bpm - t_bpm) / t_bpm) * 100.0;
        double tr_delta = ((tr_bpm - t_bpm) / t_bpm) * 100.0;
        printf("%-14s  %6.1f b  %6.1f b  %6.1f b  %+5.1f%%   %+5.1f%%\n",
               r->name, t_bpm, s_bpm, tr_bpm, s_delta, tr_delta);
    }

    printf("\n  Tiered = current 5-tier prefix coding (bits)\n");
    printf("  rANS-S = rANS with static financial distribution (bits)\n");
    printf("  rANS-T = rANS with trained distribution from first %d msgs (bits)\n", N_TRAIN);
    printf("  delta  = %% change vs tiered (negative = rANS is better)\n");

    printf("\n=======================================================================\n");
    printf("SPEED COMPARISON (ns per message)\n");
    printf("=======================================================================\n\n");

    printf("%-14s  %8s  %8s  %8s  %8s  %8s  %8s\n",
           "Message", "T-Enc", "T-Dec", "R-Enc", "R-Dec", "E-slow", "D-slow");
    printf("%-14s  %8s  %8s  %8s  %8s  %8s  %8s\n",
           "--------------", "--------", "--------", "--------", "--------", "--------", "--------");

    for (int i = 0; i < 4; i++) {
        experiment_result_t *r = &results[i];
        double enc_slow = ((r->enc_rans_ns - r->enc_tiered_ns) / r->enc_tiered_ns) * 100.0;
        double dec_slow = ((r->dec_rans_ns - r->dec_tiered_ns) / r->dec_tiered_ns) * 100.0;
        printf("%-14s  %5.0f ns  %5.0f ns  %5.0f ns  %5.0f ns  %+5.0f%%   %+5.0f%%\n",
               r->name, r->enc_tiered_ns, r->dec_tiered_ns,
               r->enc_rans_ns, r->dec_rans_ns, enc_slow, dec_slow);
    }

    printf("\n  T = Tiered (full residc encode/decode pipeline)\n");
    printf("  R = rANS (residuals only, no prediction/state overhead)\n");
    printf("  slow = %% slower than tiered (for residual coding only)\n");

    printf("\n=======================================================================\n");
    printf("ROUNDTRIP VERIFICATION\n");
    printf("=======================================================================\n\n");

    int total_errors = 0;
    for (int i = 0; i < 4; i++) {
        printf("  %-14s: %s (%d errors)\n", results[i].name,
               results[i].roundtrip_errors == 0 ? "PASS" : "FAIL",
               results[i].roundtrip_errors);
        total_errors += results[i].roundtrip_errors;
    }

    printf("\n=======================================================================\n");
    printf("ANALYSIS\n");
    printf("=======================================================================\n\n");

    printf("  Per-message rANS overhead:\n");
    printf("    - 4 bytes for rANS state flush (mandatory per-message)\n");
    printf("    - 1 byte for rans_bytes length prefix\n");
    printf("    - Total: 5 bytes (40 bits) fixed overhead per message\n\n");

    printf("  The rANS overhead comes from flushing the ANS state at each\n");
    printf("  message boundary. With only 2-4 residual symbols per message,\n");
    printf("  the 40-bit state flush cost is amortized over very few symbols.\n\n");

    printf("  For the residual distribution seen in financial data (dominated\n");
    printf("  by small values), the tiered prefix code is already near-optimal\n");
    printf("  since its tier boundaries align well with the data distribution.\n\n");

    printf("  rANS would excel if:\n");
    printf("    - Messages contained 10+ residual symbols (better amortization)\n");
    printf("    - The distribution was highly skewed in ways that don't align\n");
    printf("      with power-of-2 tier boundaries\n");
    printf("    - Streaming (non-per-message) encoding was acceptable\n");

    printf("\nDone.\n");

    free(quotes);
    free(trades);
    free(orders);
    free(books);

    return total_errors;
}
