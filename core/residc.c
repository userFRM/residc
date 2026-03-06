/*
 * residc.c — Schema-Driven Prediction-Residual Compression for Financial Data
 *
 * Implementation of the core encoding/decoding engine.
 * See residc.h for API documentation and usage examples.
 *
 * License: MIT OR Apache-2.0
 */

#include "residc.h"

_Static_assert((-1 >> 1) == -1, "residc requires arithmetic right shift");

/* ================================================================
 * Internal: k parameter tables
 * ================================================================ */

static inline int k_timestamp(int regime)
{ return regime == RESIDC_REGIME_VOLATILE ? 8 : 10; }

static inline int k_price(int regime)
{ return regime == RESIDC_REGIME_VOLATILE ? 7 : 3; }

static inline int k_quantity(int regime)
{ return regime == RESIDC_REGIME_VOLATILE ? 7 : 4; }

static inline int k_seqid(int regime)
{ return regime == RESIDC_REGIME_VOLATILE ? 5 : 3; }

/* ================================================================
 * Bit Writer
 * ================================================================ */

void residc_bw_init(residc_bitwriter_t *bw)
{
    bw->accum = 0;
    bw->count = 0;
    bw->byte_pos = 0;
}

static void bw_flush(residc_bitwriter_t *bw)
{
    while (bw->count >= 8) {
        if (bw->byte_pos >= RESIDC_SCRATCH_BYTES) return;
        bw->count -= 8;
        bw->buf[bw->byte_pos++] = (uint8_t)(bw->accum >> bw->count);
    }
}

void residc_bw_write(residc_bitwriter_t *bw, uint64_t val, int nbits)
{
    bw->accum = (bw->accum << nbits) | (val & ((1ULL << nbits) - 1));
    bw->count += nbits;
    if (__builtin_expect(bw->count >= 32, 0))
        bw_flush(bw);
}

/* Fast inline version for internal hot path — word-at-a-time flush.
 * When count >= 32, flush all complete bytes via a single 8-byte store
 * instead of looping byte-by-byte. Wire format is identical.
 * Threshold is 32 because the largest single write is 32 bits,
 * so count + nbits <= 32 + 32 = 64 (no accumulator overflow). */
static inline __attribute__((always_inline))
void bw_write(residc_bitwriter_t *bw, uint64_t val, int nbits)
{
    bw->accum = (bw->accum << nbits) | (val & ((1ULL << nbits) - 1));
    bw->count += nbits;
    if (__builtin_expect(bw->count >= 32, 0)) {
        /* Align pending bits to the top of a 64-bit word, byte-swap to
         * big-endian, and store 8 bytes. Only byte_pos advances by the
         * number of complete bytes, so the partial-byte bits stay in accum. */
        uint64_t be = __builtin_bswap64(bw->accum << (64 - bw->count));
        memcpy(bw->buf + bw->byte_pos, &be, 8);
        int bytes_out = bw->count >> 3;
        bw->byte_pos += bytes_out;
        bw->count &= 7;  /* count %= 8 — keep only the partial byte */
    }
}

int residc_bw_finish(residc_bitwriter_t *bw)
{
    if (__builtin_expect(bw->count >= 8, 1))
        bw_flush(bw);
    if (bw->count > 0 && bw->byte_pos < RESIDC_SCRATCH_BYTES) {
        bw->buf[bw->byte_pos++] =
            (uint8_t)(bw->accum << (8 - bw->count));
    }
    return bw->byte_pos;
}

/* ================================================================
 * Bit Reader
 * ================================================================ */

void residc_br_init(residc_bitreader_t *br, const uint8_t *data, int len)
{
    br->data = data;
    br->len_bytes = len;
    br->accum = 0;
    br->count = 0;
    br->byte_pos = 0;
}

static void br_refill(residc_bitreader_t *br)
{
    while (br->count <= 56 && br->byte_pos < br->len_bytes) {
        br->accum = (br->accum << 8) | br->data[br->byte_pos++];
        br->count += 8;
    }
}

uint64_t residc_br_read(residc_bitreader_t *br, int nbits)
{
    if (__builtin_expect(br->count < nbits, 0))
        br_refill(br);
    br->count -= nbits;
    return (br->accum >> br->count) & ((1ULL << nbits) - 1);
}

int residc_br_read_bit(residc_bitreader_t *br)
{
    if (__builtin_expect(br->count < 1, 0))
        br_refill(br);
    br->count--;
    return (int)((br->accum >> br->count) & 1);
}

/* Fast inline versions for internal hot path — word-at-a-time refill.
 * When 8+ bytes are available, load 8 bytes at once and merge into
 * the accumulator instead of looping byte-by-byte. */
static inline __attribute__((always_inline))
void br_refill_inline(residc_bitreader_t *br)
{
    if (__builtin_expect(br->byte_pos + 8 <= br->len_bytes, 1)) {
        /* Load 8 bytes big-endian as a 64-bit word */
        uint64_t raw;
        memcpy(&raw, br->data + br->byte_pos, 8);
        raw = __builtin_bswap64(raw);
        /* Consume whole bytes that fit in the 64-bit accumulator */
        int consume = (64 - br->count) >> 3;  /* 0..8 */
        if (__builtin_expect(consume > 0, 1)) {
            int consume_bits = consume << 3;   /* 8..64 */
            /* Shift existing bits up and merge new bytes from the top of raw.
             * For consume_bits == 64: accum has 0 valid bits, so we just
             * replace it entirely with raw. Handle specially to avoid UB. */
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
uint64_t br_read(residc_bitreader_t *br, int nbits)
{
    if (__builtin_expect(br->count < nbits, 0))
        br_refill_inline(br);
    br->count -= nbits;
    return (br->accum >> br->count) & ((1ULL << nbits) - 1);
}

static inline __attribute__((always_inline))
int br_read_bit(residc_bitreader_t *br)
{
    if (__builtin_expect(br->count < 1, 0))
        br_refill_inline(br);
    br->count--;
    return (int)((br->accum >> br->count) & 1);
}

/* ================================================================
 * Zigzag + Tiered Residual Coding
 * ================================================================ */

void residc_encode_residual(residc_bitwriter_t *bw, int64_t value, int k)
{
    uint64_t zz = residc_zigzag_enc(value);

    if (zz < (1ULL << k)) {
        /* tier 0: 0-prefix implicit (zz < 2^k), single write of 1+k bits */
        residc_bw_write(bw, zz, 1 + k);
    } else if (zz < (1ULL << (k + 2))) {
        /* tier 1: prefix 10 + k+2 payload bits, fused into one write */
        residc_bw_write(bw, (0x2ULL << (k + 2)) | zz, 2 + k + 2);
    } else if (zz < (1ULL << (k + 5))) {
        /* tier 2: prefix 110 + k+5 payload bits */
        residc_bw_write(bw, (0x6ULL << (k + 5)) | zz, 3 + k + 5);
    } else if (zz < (1ULL << (k + 10))) {
        /* tier 3: prefix 1110 + k+10 payload bits */
        residc_bw_write(bw, (0xEULL << (k + 10)) | zz, 4 + k + 10);
    } else {
        /* tier 4: prefix 1111 + 64 raw bits */
        residc_bw_write(bw, 0xF, 4);
        residc_bw_write(bw, (uint64_t)value >> 32, 32);
        residc_bw_write(bw, (uint64_t)value & 0xFFFFFFFF, 32);
    }
}

/* Decode tier LUT: peek 4 bits -> {tier, prefix_bits_consumed}
 * 0xxx=tier0(1bit), 10xx=tier1(2bit), 110x=tier2(3bit),
 * 1110=tier3(4bit), 1111=tier4(4bit) */
static const struct { uint8_t tier; uint8_t prefix_bits; } decode_tier_lut[16] = {
    {0,1},{0,1},{0,1},{0,1},{0,1},{0,1},{0,1},{0,1}, /* 0000..0111 */
    {1,2},{1,2},{1,2},{1,2},                          /* 1000..1011 */
    {2,3},{2,3},                                      /* 1100..1101 */
    {3,4},                                            /* 1110 */
    {4,4},                                            /* 1111 */
};
static const int8_t decode_payload_add[] = {0, 2, 5, 10, 0};

int64_t residc_decode_residual(residc_bitreader_t *br, int k)
{
    /* 4-bit peek LUT: determine tier without sequential bit reads */
    if (__builtin_expect(br->count < 4, 0))
        br_refill(br);
    int peek = (int)((br->accum >> (br->count - 4)) & 0xF);
    int tier = decode_tier_lut[peek].tier;
    br->count -= decode_tier_lut[peek].prefix_bits;

    if (__builtin_expect(tier < 4, 1)) {
        uint64_t zz = residc_br_read(br, k + decode_payload_add[tier]);
        return residc_zigzag_dec(zz);
    }
    /* tier 4: raw 64-bit */
    uint64_t hi = residc_br_read(br, 32);
    uint64_t lo = residc_br_read(br, 32);
    return (int64_t)((hi << 32) | lo);
}

/* Fast inline residual encode/decode for internal hot path */
static inline __attribute__((always_inline))
void encode_residual(residc_bitwriter_t *bw, int64_t value, int k)
{
    uint64_t zz = residc_zigzag_enc(value);
    if (zz < (1ULL << k)) {
        bw_write(bw, zz, 1 + k);
    } else if (zz < (1ULL << (k + 2))) {
        bw_write(bw, (0x2ULL << (k + 2)) | zz, 2 + k + 2);
    } else if (zz < (1ULL << (k + 5))) {
        bw_write(bw, (0x6ULL << (k + 5)) | zz, 3 + k + 5);
    } else if (zz < (1ULL << (k + 10))) {
        bw_write(bw, (0xEULL << (k + 10)) | zz, 4 + k + 10);
    } else {
        bw_write(bw, 0xF, 4);
        bw_write(bw, (uint64_t)value >> 32, 32);
        bw_write(bw, (uint64_t)value & 0xFFFFFFFF, 32);
    }
}

static inline __attribute__((always_inline))
int64_t decode_residual(residc_bitreader_t *br, int k)
{
    /* Ensure at least 4 bits available for peek */
    if (__builtin_expect(br->count < 4, 0))
        br_refill_inline(br);
    int peek = (int)((br->accum >> (br->count - 4)) & 0xF);
    int tier = decode_tier_lut[peek].tier;
    br->count -= decode_tier_lut[peek].prefix_bits;

    if (__builtin_expect(tier < 4, 1)) {
        return residc_zigzag_dec(br_read(br, k + decode_payload_add[tier]));
    }
    uint64_t hi = br_read(br, 32);
    uint64_t lo = br_read(br, 32);
    return (int64_t)((hi << 32) | lo);
}

/* ================================================================
 * Exp-Golomb Residual Coding
 * ================================================================ */

void residc_encode_residual_expg(residc_bitwriter_t *bw, int64_t value, int k)
{
    uint64_t zz = residc_zigzag_enc(value);
    if (__builtin_expect(zz > UINT64_MAX - (1ULL << k), 0)) {
        residc_bw_write(bw, 0xFFFFFFFF, 32);
        residc_bw_write(bw, (uint64_t)value >> 32, 32);
        residc_bw_write(bw, (uint64_t)value & 0xFFFFFFFF, 32);
        return;
    }
    uint64_t m = zz + (1ULL << k);
    int bits = 64 - __builtin_clzll(m);
    int q = bits - 1 - k;

    if (__builtin_expect(q > 31, 0)) {
        residc_bw_write(bw, 0xFFFFFFFF, 32);
        residc_bw_write(bw, (uint64_t)value >> 32, 32);
        residc_bw_write(bw, (uint64_t)value & 0xFFFFFFFF, 32);
        return;
    }

    if (q > 0)
        residc_bw_write(bw, (1ULL << q) - 1, q);
    residc_bw_write(bw, 0, 1);
    int rem_bits = q + k;
    if (rem_bits > 32) {
        int hi_bits = rem_bits - 32;
        uint64_t rem_val = m & ((1ULL << rem_bits) - 1);
        residc_bw_write(bw, rem_val >> 32, hi_bits);
        residc_bw_write(bw, rem_val & 0xFFFFFFFF, 32);
    } else if (rem_bits > 0) {
        residc_bw_write(bw, m & ((1ULL << rem_bits) - 1), rem_bits);
    }
}

int64_t residc_decode_residual_expg(residc_bitreader_t *br, int k)
{
    int q = 0;
    while (q < 32 && residc_br_read_bit(br) == 1)
        q++;

    if (__builtin_expect(q >= 32, 0)) {
        uint64_t hi = residc_br_read(br, 32);
        uint64_t lo = residc_br_read(br, 32);
        return (int64_t)((hi << 32) | lo);
    }

    int rem_bits = q + k;
    uint64_t rem;
    if (rem_bits > 32) {
        int hi_bits = rem_bits - 32;
        uint64_t hi = residc_br_read(br, hi_bits);
        uint64_t lo = residc_br_read(br, 32);
        rem = (hi << 32) | lo;
    } else if (rem_bits > 0) {
        rem = residc_br_read(br, rem_bits);
    } else {
        rem = 0;
    }
    uint64_t m = ((uint64_t)1 << (q + k)) | rem;
    uint64_t zz = m - (1ULL << k);
    return residc_zigzag_dec(zz);
}

/* Fast inline Exp-Golomb for internal hot path */
static inline __attribute__((always_inline))
void encode_residual_expg(residc_bitwriter_t *bw, int64_t value, int k)
{
    uint64_t zz = residc_zigzag_enc(value);
    if (__builtin_expect(zz > UINT64_MAX - (1ULL << k), 0)) {
        bw_write(bw, 0xFFFFFFFF, 32);
        bw_write(bw, (uint64_t)value >> 32, 32);
        bw_write(bw, (uint64_t)value & 0xFFFFFFFF, 32);
        return;
    }
    uint64_t m = zz + (1ULL << k);
    int bits = 64 - __builtin_clzll(m);
    int q = bits - 1 - k;

    if (__builtin_expect(q > 31, 0)) {
        bw_write(bw, 0xFFFFFFFF, 32);
        bw_write(bw, (uint64_t)value >> 32, 32);
        bw_write(bw, (uint64_t)value & 0xFFFFFFFF, 32);
        return;
    }

    if (q > 0)
        bw_write(bw, (1ULL << q) - 1, q);
    bw_write(bw, 0, 1);
    int rem_bits = q + k;
    if (rem_bits > 32) {
        int hi_bits = rem_bits - 32;
        uint64_t rem_val = m & ((1ULL << rem_bits) - 1);
        bw_write(bw, rem_val >> 32, hi_bits);
        bw_write(bw, rem_val & 0xFFFFFFFF, 32);
    } else if (rem_bits > 0) {
        bw_write(bw, m & ((1ULL << rem_bits) - 1), rem_bits);
    }
}

static inline __attribute__((always_inline))
int64_t decode_residual_expg(residc_bitreader_t *br, int k)
{
    int q = 0;
    while (q < 32 && br_read_bit(br) == 1)
        q++;

    if (__builtin_expect(q >= 32, 0)) {
        uint64_t hi = br_read(br, 32);
        uint64_t lo = br_read(br, 32);
        return (int64_t)((hi << 32) | lo);
    }

    int rem_bits = q + k;
    uint64_t rem;
    if (rem_bits > 32) {
        int hi_bits = rem_bits - 32;
        uint64_t hi = br_read(br, hi_bits);
        uint64_t lo = br_read(br, 32);
        rem = (hi << 32) | lo;
    } else if (rem_bits > 0) {
        rem = br_read(br, rem_bits);
    } else {
        rem = 0;
    }
    uint64_t m = ((uint64_t)1 << (q + k)) | rem;
    uint64_t zz = m - (1ULL << k);
    return residc_zigzag_dec(zz);
}

void residc_set_coder(residc_state_t *state, int coder)
{
    state->residual_coder = (uint8_t)coder;
}

/* Dispatching encode/decode: selects tiered or exp-golomb based on state */
static inline __attribute__((always_inline))
void encode_residual_dispatch(residc_bitwriter_t *bw, int64_t value, int k,
                              uint8_t coder)
{
    if (__builtin_expect(coder == RESIDC_CODER_EXPGOLOMB, 0))
        encode_residual_expg(bw, value, k);
    else
        encode_residual(bw, value, k);
}

static inline __attribute__((always_inline))
int64_t decode_residual_dispatch(residc_bitreader_t *br, int k,
                                 uint8_t coder)
{
    if (__builtin_expect(coder == RESIDC_CODER_EXPGOLOMB, 0))
        return decode_residual_expg(br, k);
    else
        return decode_residual(br, k);
}

/* ================================================================
 * Adaptive k
 * ================================================================ */

int residc_adaptive_k(uint64_t sum, uint32_t count, int min_k, int max_k)
{
    if (count == 0) return min_k;
    /* Compute floor(log2(sum/count)) without division.
     * log2(sum/count) = log2(sum) - log2(count).
     * count is bounded by RESIDC_ADAPT_WINDOW (8). */
    if (sum == 0) return min_k;
    int log2_sum = 63 - __builtin_clzll(sum);  /* floor(log2(sum)) */
    /* floor(log2(count)) for count 1..8: 0,1,1,2,2,2,2,3 */
    int log2_count = (count <= 1) ? 0 : (31 - __builtin_clz(count));
    int k = log2_sum - log2_count;
    if (k < min_k) return min_k;
    if (k > max_k) return max_k;
    return k;
}

void residc_adaptive_update(uint64_t *sum, uint32_t *count, uint64_t val)
{
    *sum += val;
    (*count)++;
    if (*count >= RESIDC_ADAPT_WINDOW) {
        *sum >>= 1;
        *count >>= 1;
    }
}

/* ================================================================
 * MFU Table
 * ================================================================ */

void residc_mfu_init(residc_mfu_table_t *mfu)
{
    memset(mfu, 0, sizeof(*mfu));
    for (int i = 0; i < 256; i++)
        mfu->hash[i] = 0xFFFF;
    for (int i = 0; i < RESIDC_MFU_SIZE; i++) {
        mfu->chain[i] = 0xFFFF;
        mfu->rank_to_idx[i] = (uint8_t)i;
        mfu->idx_to_rank[i] = (uint8_t)i;
    }
}

int residc_mfu_lookup(const residc_mfu_table_t *mfu, uint16_t id)
{
    uint8_t h = (uint8_t)(id * 157);  /* simple hash */
    uint16_t idx = mfu->hash[h];
    while (idx != 0xFFFF) {
        if (mfu->entries[idx].instrument_id == id)
            return (int)idx;
        idx = mfu->chain[idx];
    }
    return -1;
}

void residc_mfu_update(residc_mfu_table_t *mfu, uint16_t id)
{
    int idx = residc_mfu_lookup(mfu, id);
    if (idx >= 0) {
        mfu->entries[idx].count++;
        return;
    }

    /* Not found — add or replace minimum */
    if (mfu->num_entries < RESIDC_MFU_SIZE) {
        idx = mfu->num_entries++;
    } else {
        /* Find min-count entry and replace */
        int min_idx = 0;
        uint16_t min_count = mfu->entries[0].count;
        for (int i = 1; i < RESIDC_MFU_SIZE; i++) {
            if (mfu->entries[i].count < min_count) {
                min_count = mfu->entries[i].count;
                min_idx = i;
            }
        }
        /* Remove old entry from hash chain */
        uint8_t old_h = (uint8_t)(mfu->entries[min_idx].instrument_id * 157);
        uint16_t *pp = &mfu->hash[old_h];
        while (*pp != 0xFFFF) {
            if (*pp == (uint16_t)min_idx) {
                *pp = mfu->chain[min_idx];
                break;
            }
            pp = &mfu->chain[*pp];
        }
        idx = min_idx;
    }

    mfu->entries[idx].instrument_id = id;
    mfu->entries[idx].count = 1;

    /* Insert into hash chain */
    uint8_t h = (uint8_t)(id * 157);
    mfu->chain[idx] = mfu->hash[h];
    mfu->hash[h] = (uint16_t)idx;
}

/* Rebuild frequency-rank mappings (insertion sort by descending count).
 * Called after init, seed, and every decay cycle so encoder/decoder stay in sync. */
static void mfu_rebuild_ranks(residc_mfu_table_t *mfu)
{
    int n = (int)mfu->num_entries;
    /* Initialize rank_to_idx as identity */
    for (int i = 0; i < n; i++)
        mfu->rank_to_idx[i] = (uint8_t)i;
    for (int i = n; i < RESIDC_MFU_SIZE; i++)
        mfu->rank_to_idx[i] = (uint8_t)i;

    /* Insertion sort rank_to_idx by descending count */
    for (int i = 1; i < n; i++) {
        uint8_t tmp = mfu->rank_to_idx[i];
        uint16_t tmp_count = mfu->entries[tmp].count;
        int j = i - 1;
        while (j >= 0 && mfu->entries[mfu->rank_to_idx[j]].count < tmp_count) {
            mfu->rank_to_idx[j + 1] = mfu->rank_to_idx[j];
            j--;
        }
        mfu->rank_to_idx[j + 1] = tmp;
    }

    /* Build inverse: idx_to_rank */
    for (int r = 0; r < RESIDC_MFU_SIZE; r++)
        mfu->idx_to_rank[mfu->rank_to_idx[r]] = (uint8_t)r;
}

/* Periodic decay to adapt to changing instrument distributions */
static void mfu_decay(residc_mfu_table_t *mfu)
{
    for (int i = 0; i < mfu->num_entries; i++)
        mfu->entries[i].count >>= 1;
    mfu_rebuild_ranks(mfu);
}

/* ================================================================
 * Field read/write helpers
 * ================================================================ */

static inline uint64_t read_field(const void *msg, uint16_t offset, uint8_t size)
{
    const uint8_t *p = (const uint8_t *)msg + offset;
    switch (size) {
    case 1: return *(const uint8_t *)p;
    case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
    case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
    case 8: { uint64_t v; memcpy(&v, p, 8); return v; }
    default: return 0;
    }
}

static inline void write_field(void *msg, uint16_t offset, uint8_t size, uint64_t val)
{
    uint8_t *p = (uint8_t *)msg + offset;
    switch (size) {
    case 1: *(uint8_t *)p = (uint8_t)val; break;
    case 2: { uint16_t v = (uint16_t)val; memcpy(p, &v, 2); break; }
    case 4: { uint32_t v = (uint32_t)val; memcpy(p, &v, 4); break; }
    case 8: { uint64_t v = val;           memcpy(p, &v, 8); break; }
    }
}

/* ================================================================
 * Schema-driven Encoder
 * ================================================================ */

void residc_init(residc_state_t *state, const residc_schema_t *schema)
{
    memset(state, 0, sizeof(*state));
    state->schema = schema;
    state->multi_schema = NULL;
    residc_mfu_init(&state->mfu);
    for (int i = 0; i < RESIDC_MAX_FIELDS; i++)
        state->field_state[i].last_value = 0;
}

void residc_init_multi(residc_state_t *state, const residc_multi_schema_t *multi)
{
    memset(state, 0, sizeof(*state));
    state->schema = NULL;
    state->multi_schema = multi;
    residc_mfu_init(&state->mfu);
    for (int i = 0; i < RESIDC_MAX_FIELDS; i++)
        state->field_state[i].last_value = 0;
}

void residc_reset(residc_state_t *state)
{
    const residc_schema_t *s = state->schema;
    const residc_multi_schema_t *m = state->multi_schema;
    memset(state, 0, sizeof(*state));
    state->schema = s;
    state->multi_schema = m;
    residc_mfu_init(&state->mfu);
}

/* ================================================================
 * State Checkpoint / Restore
 * ================================================================ */

void residc_snapshot(const residc_state_t *state, residc_state_t *snap)
{
    memcpy(snap, state, sizeof(*state));
}

void residc_restore(residc_state_t *state, const residc_state_t *snap)
{
    const residc_schema_t *s = state->schema;
    const residc_multi_schema_t *m = state->multi_schema;
    memcpy(state, snap, sizeof(*state));
    state->schema = s;
    state->multi_schema = m;
}

/* ================================================================
 * MFU Pre-Seeding
 * ================================================================ */

void residc_mfu_seed(residc_mfu_table_t *mfu, const uint16_t *ids,
                     const uint16_t *counts, int n)
{
    residc_mfu_init(mfu);
    int to_add = n < RESIDC_MFU_SIZE ? n : RESIDC_MFU_SIZE;
    for (int i = 0; i < to_add; i++) {
        mfu->entries[i].instrument_id = ids[i];
        mfu->entries[i].count = counts[i];
        uint8_t h = (uint8_t)(ids[i] * 157);
        mfu->chain[i] = mfu->hash[h];
        mfu->hash[h] = (uint16_t)i;
    }
    mfu->num_entries = (uint16_t)to_add;
    mfu_rebuild_ranks(mfu);
}

int residc_raw_size(const residc_schema_t *schema)
{
    int size = 0;
    for (int i = 0; i < schema->num_fields; i++) {
        if (schema->fields[i].type != RESIDC_COMPUTED)
            size += schema->fields[i].size;
    }
    return size;
}

/* Variable-length MFU rank encoding/decoding.
 * Rank 0-3: prefix 0 + 2 bits = 3 bits total
 * Rank 4-15: prefix 10 + 4 bits = 6 bits total
 * Rank 16-63: prefix 110 + 6 bits = 9 bits total
 * Rank 64-255: prefix 111 + 8 bits = 11 bits total */
static inline __attribute__((always_inline))
void encode_mfu_rank(residc_bitwriter_t *bw, int rank)
{
    if (rank < 4) {
        bw_write(bw, 0, 1);
        bw_write(bw, (uint64_t)rank, 2);
    } else if (rank < 16) {
        bw_write(bw, 2, 2);
        bw_write(bw, (uint64_t)(rank - 4), 4);
    } else if (rank < 64) {
        bw_write(bw, 6, 3);
        bw_write(bw, (uint64_t)(rank - 16), 6);
    } else {
        bw_write(bw, 7, 3);
        bw_write(bw, (uint64_t)(rank - 64), 8);
    }
}

static inline __attribute__((always_inline))
int decode_mfu_rank(residc_bitreader_t *br)
{
    if (br_read_bit(br) == 0)
        return (int)br_read(br, 2);               /* rank 0-3 */
    if (br_read_bit(br) == 0)
        return 4 + (int)br_read(br, 4);           /* rank 4-15 */
    if (br_read_bit(br) == 0)
        return 16 + (int)br_read(br, 6);          /* rank 16-63 */
    return 64 + (int)br_read(br, 8);              /* rank 64-255 */
}

static int encode_fields(residc_state_t *state, const residc_schema_t *schema,
                         const void *msg, residc_bitwriter_t *bw)
{
    uint16_t instrument_id = state->last_instrument_id;
    residc_instrument_state_t *is = NULL;
    uint8_t coder = state->residual_coder;

    for (int fi = 0; fi < schema->num_fields; fi++) {
        const residc_field_t *f = &schema->fields[fi];
        uint64_t val = read_field(msg, f->offset, f->size);

        switch (f->type) {

        case RESIDC_TIMESTAMP: {
            uint64_t ts = val;
            int64_t gap = (int64_t)(ts - state->last_timestamp);
            int64_t predicted_gap = state->timestamp_gap_ema >> 16;
            if (predicted_gap < 0) predicted_gap = 0;
            int64_t residual = gap - predicted_gap;

            int k = residc_adaptive_k(state->ts_adapt_sum,
                                       state->ts_adapt_count,
                                       k_timestamp(state->regime),
                                       k_timestamp(state->regime) + 10);
            encode_residual_dispatch(bw, residual, k, coder);

            /* fused state: TIMESTAMP */
            {
                uint64_t zz = residc_zigzag_enc(residual);
                residc_adaptive_update(&state->ts_adapt_sum,
                                       &state->ts_adapt_count, zz);
                int64_t gap_q16 = (int64_t)((uint64_t)gap << 16);
                state->timestamp_gap_ema +=
                    (gap_q16 - state->timestamp_gap_ema) >> 2;
                state->last_timestamp = ts;
                state->last_timestamp_gap = (uint64_t)gap;
            }
            break;
        }

        case RESIDC_INSTRUMENT: {
            instrument_id = (uint16_t)val;
            is = (instrument_id < RESIDC_MAX_INSTRUMENTS)
                 ? &state->instruments[instrument_id] : NULL;

            if (instrument_id == state->last_instrument_id &&
                state->msg_count > 0) {
                bw_write(bw, 0, 1);  /* same as last */
            } else {
                bw_write(bw, 1, 1);  /* different */
                int mfu_idx = residc_mfu_lookup(&state->mfu, instrument_id);
                if (mfu_idx >= 0) {
                    bw_write(bw, 0, 1);  /* in MFU */
                    int rank = (int)state->mfu.idx_to_rank[mfu_idx];
                    encode_mfu_rank(bw, rank);
                } else {
                    bw_write(bw, 1, 1);  /* raw */
                    bw_write(bw, instrument_id, 14);
                }
            }

            /* fused state: INSTRUMENT */
            residc_mfu_update(&state->mfu, instrument_id);
            state->mfu_decay_counter++;
            if (state->mfu_decay_counter >= 10000) {
                mfu_decay(&state->mfu);
                state->mfu_decay_counter = 0;
            }
            state->last_instrument_id = instrument_id;
            break;
        }

        case RESIDC_PRICE: {
            uint32_t price = (uint32_t)val;
            uint32_t predicted = (is && is->msg_count > 0)
                               ? is->last_price : 0;
            int64_t residual = (int64_t)price - (int64_t)predicted;

            int k = residc_adaptive_k(state->price_adapt_sum,
                                       state->price_adapt_count,
                                       k_price(state->regime),
                                       k_price(state->regime) + 10);

            /* Penny normalization */
            int64_t coded_residual;
            if (price % 100 == 0 && predicted % 100 == 0 &&
                (price > 0 || predicted > 0)) {
                bw_write(bw, 0, 1);  /* penny mode */
                coded_residual = residual / 100;
                encode_residual_dispatch(bw, coded_residual, k, coder);
            } else {
                bw_write(bw, 1, 1);  /* sub-penny mode */
                coded_residual = residual;
                encode_residual_dispatch(bw, coded_residual, k, coder);
            }

            /* fused state: PRICE */
            {
                uint64_t zz = residc_zigzag_enc(coded_residual);
                residc_adaptive_update(&state->price_adapt_sum,
                                       &state->price_adapt_count, zz);
                uint64_t abs_res = (uint64_t)(residual < 0
                    ? -(uint64_t)residual : (uint64_t)residual);
                state->recent_abs_price_sum += (uint32_t)abs_res;
                state->regime_counter++;
                if (state->regime_counter >= RESIDC_REGIME_WINDOW) {
                    uint32_t avg = state->recent_abs_price_sum /
                                   RESIDC_REGIME_WINDOW;
                    state->regime = (avg > 30) ? RESIDC_REGIME_VOLATILE
                                               : RESIDC_REGIME_CALM;
                    state->recent_abs_price_sum = 0;
                    state->regime_counter = 0;
                }
                if (is) is->last_price = price;
            }
            break;
        }

        case RESIDC_QUANTITY: {
            uint32_t qty = (uint32_t)val;
            uint32_t predicted = (is && is->msg_count > 0)
                               ? is->last_qty : 100;
            int64_t residual = (int64_t)qty - (int64_t)predicted;

            int k = residc_adaptive_k(state->qty_adapt_sum,
                                       state->qty_adapt_count,
                                       k_quantity(state->regime),
                                       k_quantity(state->regime) + 10);

            if (residual == 0) {
                bw_write(bw, 0, 1);  /* same as predicted */
            } else {
                int64_t coded_residual;
                bw_write(bw, 1, 1);  /* different */
                if (qty % 100 == 0 && predicted % 100 == 0) {
                    bw_write(bw, 0, 1);  /* round lot */
                    coded_residual = residual / 100;
                    encode_residual_dispatch(bw, coded_residual, k, coder);
                } else {
                    bw_write(bw, 1, 1);  /* odd lot */
                    coded_residual = residual;
                    encode_residual_dispatch(bw, coded_residual, k, coder);
                }
                {
                    uint64_t zz = residc_zigzag_enc(coded_residual);
                    residc_adaptive_update(&state->qty_adapt_sum,
                                           &state->qty_adapt_count, zz);
                }
            }

            /* fused state: QUANTITY */
            if (is) is->last_qty = qty;
            break;
        }

        case RESIDC_SEQUENTIAL_ID: {
            uint64_t id = val;
            residc_field_state_t *fs = &state->field_state[fi];

            /* Use per-instrument prediction if available */
            uint64_t predicted = (is && is->last_seq_id > 0)
                               ? is->last_seq_id : fs->last_value;
            int64_t delta = (int64_t)(id - predicted);
            int k = residc_adaptive_k(fs->adapt_sum, fs->adapt_count,
                                       k_seqid(state->regime),
                                       k_seqid(state->regime) + 10);
            encode_residual_dispatch(bw, delta, k, coder);

            /* fused state: SEQUENTIAL_ID */
            {
                uint64_t zz = residc_zigzag_enc(delta);
                residc_adaptive_update(&fs->adapt_sum, &fs->adapt_count, zz);
                fs->last_value = id;
                if (is) is->last_seq_id = id;
            }
            break;
        }

        case RESIDC_ENUM: {
            residc_field_state_t *fs = &state->field_state[fi];
            if (val == fs->last_value && state->msg_count > 0) {
                bw_write(bw, 0, 1);  /* same */
            } else {
                bw_write(bw, 1, 1);  /* different */
                bw_write(bw, val, f->size * 8);
            }
            /* fused state: ENUM */
            fs->last_value = val;
            break;
        }

        case RESIDC_BOOL:
            bw_write(bw, val & 1, 1);
            break;

        case RESIDC_CATEGORICAL: {
            residc_field_state_t *fs = &state->field_state[fi];
            if (val == fs->last_value && state->msg_count > 0) {
                bw_write(bw, 0, 1);  /* same */
            } else {
                bw_write(bw, 1, 1);  /* different */
                bw_write(bw, val >> 16, 16);
                bw_write(bw, val & 0xFFFF, 16);
            }
            /* fused state: CATEGORICAL */
            fs->last_value = val;
            break;
        }

        case RESIDC_RAW:
            if (f->size <= 8) {
                bw_write(bw, val, f->size * 8);
            } else {
                /* Byte-by-byte for larger fields */
                const uint8_t *p = (const uint8_t *)msg + f->offset;
                for (int i = 0; i < f->size; i++)
                    bw_write(bw, p[i], 8);
            }
            break;

        case RESIDC_DELTA_ID: {
            uint64_t ref_val = (f->ref_field >= 0)
                ? read_field(msg, schema->fields[f->ref_field].offset,
                             schema->fields[f->ref_field].size)
                : state->field_state[fi].last_value;
            int64_t delta = (int64_t)(val - ref_val);
            int k = k_seqid(state->regime);
            encode_residual_dispatch(bw, delta, k, coder);
            /* fused state: DELTA_ID */
            state->field_state[fi].last_value = val;
            break;
        }

        case RESIDC_DELTA_PRICE: {
            uint64_t ref_val = (f->ref_field >= 0)
                ? read_field(msg, schema->fields[f->ref_field].offset,
                             schema->fields[f->ref_field].size)
                : (is ? is->last_price : 0);
            int64_t delta = (int64_t)val - (int64_t)ref_val;
            int k = residc_adaptive_k(state->price_adapt_sum,
                                       state->price_adapt_count,
                                       k_price(state->regime),
                                       k_price(state->regime) + 10);
            encode_residual_dispatch(bw, delta, k, coder);
            break;
        }

        case RESIDC_COMPUTED:
            /* Not encoded — 0 bits */
            break;
        }
    }

    /* fused: increment per-instrument and global msg_count */
    if (is) is->msg_count++;
    state->msg_count++;

    return 0;
}

static void commit_state(residc_state_t *state, const residc_schema_t *schema,
                         const void *msg)
{
    uint16_t instrument_id = state->last_instrument_id;
    residc_instrument_state_t *is = NULL;

    for (int fi = 0; fi < schema->num_fields; fi++) {
        const residc_field_t *f = &schema->fields[fi];
        uint64_t val = read_field(msg, f->offset, f->size);

        switch (f->type) {
        case RESIDC_TIMESTAMP: {
            uint64_t ts = val;
            int64_t gap = (int64_t)(ts - state->last_timestamp);
            int64_t predicted_gap = state->timestamp_gap_ema >> 16;
            if (predicted_gap < 0) predicted_gap = 0;
            int64_t residual = gap - predicted_gap;
            uint64_t zz = residc_zigzag_enc(residual);
            residc_adaptive_update(&state->ts_adapt_sum,
                                   &state->ts_adapt_count, zz);
            int64_t gap_q16 = (int64_t)((uint64_t)gap << 16);
            state->timestamp_gap_ema +=
                (gap_q16 - state->timestamp_gap_ema) >> 2;
            state->last_timestamp = ts;
            state->last_timestamp_gap = (uint64_t)gap;
            break;
        }
        case RESIDC_INSTRUMENT:
            instrument_id = (uint16_t)val;
            is = (instrument_id < RESIDC_MAX_INSTRUMENTS)
                 ? &state->instruments[instrument_id] : NULL;
            residc_mfu_update(&state->mfu, instrument_id);
            state->mfu_decay_counter++;
            if (state->mfu_decay_counter >= 10000) {
                mfu_decay(&state->mfu);
                state->mfu_decay_counter = 0;
            }
            state->last_instrument_id = instrument_id;
            break;
        case RESIDC_PRICE: {
            uint32_t price = (uint32_t)val;
            uint32_t predicted = (is && is->msg_count > 0)
                               ? is->last_price : 0;
            int64_t residual = (int64_t)price - (int64_t)predicted;
            /* Adaptive k: track penny-normalized residual when applicable */
            {
                int64_t coded_res = residual;
                if (price % 100 == 0 && predicted % 100 == 0 &&
                    (price > 0 || predicted > 0))
                    coded_res = residual / 100;
                uint64_t zz = residc_zigzag_enc(coded_res);
                residc_adaptive_update(&state->price_adapt_sum,
                                       &state->price_adapt_count, zz);
            }
            uint64_t abs_res = (uint64_t)(residual < 0
                ? -(uint64_t)residual : (uint64_t)residual);
            state->recent_abs_price_sum += (uint32_t)abs_res;
            state->regime_counter++;
            if (state->regime_counter >= RESIDC_REGIME_WINDOW) {
                uint32_t avg = state->recent_abs_price_sum /
                               RESIDC_REGIME_WINDOW;
                state->regime = (avg > 30) ? RESIDC_REGIME_VOLATILE
                                           : RESIDC_REGIME_CALM;
                state->recent_abs_price_sum = 0;
                state->regime_counter = 0;
            }
            if (is) is->last_price = price;
            break;
        }
        case RESIDC_QUANTITY: {
            uint32_t qty = (uint32_t)val;
            uint32_t predicted = (is && is->msg_count > 0)
                               ? is->last_qty : 100;
            int64_t qty_residual = (int64_t)qty - (int64_t)predicted;
            if (qty_residual != 0) {
                int64_t coded_res = qty_residual;
                if (qty % 100 == 0 && predicted % 100 == 0)
                    coded_res = qty_residual / 100;
                uint64_t zz = residc_zigzag_enc(coded_res);
                residc_adaptive_update(&state->qty_adapt_sum,
                                       &state->qty_adapt_count, zz);
            }
            if (is) is->last_qty = qty;
            break;
        }
        case RESIDC_SEQUENTIAL_ID: {
            uint64_t id = val;
            residc_field_state_t *fs = &state->field_state[fi];
            uint64_t predicted = (is && is->last_seq_id > 0)
                               ? is->last_seq_id : fs->last_value;
            int64_t delta = (int64_t)(id - predicted);
            uint64_t zz = residc_zigzag_enc(delta);
            residc_adaptive_update(&fs->adapt_sum, &fs->adapt_count, zz);
            fs->last_value = id;
            if (is) is->last_seq_id = id;
            break;
        }
        case RESIDC_ENUM:
        case RESIDC_CATEGORICAL:
            state->field_state[fi].last_value = val;
            break;
        case RESIDC_DELTA_ID:
            state->field_state[fi].last_value = val;
            break;
        default:
            break;
        }
    }

    if (is) is->msg_count++;
    state->msg_count++;
}

int residc_encode(residc_state_t *state, const void *msg,
                  uint8_t *out, int capacity)
{
    const residc_schema_t *schema = state->schema;
    if (!schema || capacity < 2) return -1;

    residc_bitwriter_t bw;
    residc_bw_init(&bw);

    encode_fields(state, schema, msg, &bw);

    int payload_len = residc_bw_finish(&bw);

    /* Check if compressed is smaller than raw */
    int raw_size = residc_raw_size(schema);
    if (payload_len >= raw_size || payload_len >= 254) {
        /* Literal fallback */
        if (capacity < 1 + raw_size) return -1;
        out[0] = RESIDC_FRAME_LITERAL;
        /* Serialize raw fields */
        int pos = 1;
        for (int fi = 0; fi < schema->num_fields; fi++) {
            const residc_field_t *f = &schema->fields[fi];
            if (f->type == RESIDC_COMPUTED) continue;
            uint64_t val = read_field(msg, f->offset, f->size);
            for (int b = f->size - 1; b >= 0; b--)
                out[pos++] = (uint8_t)(val >> (b * 8));
        }
        /* state already updated by encode_fields */
        return pos;
    }

    /* Compressed frame */
    if (capacity < 1 + payload_len) return -1;
    out[0] = (uint8_t)payload_len;
    memcpy(out + 1, bw.buf, payload_len);

    /* state already updated by encode_fields */
    return 1 + payload_len;
}

/* ================================================================
 * Schema-driven Decoder
 * ================================================================ */

static int decode_fields(residc_state_t *state, const residc_schema_t *schema,
                         residc_bitreader_t *br, void *msg)
{
    uint16_t instrument_id = state->last_instrument_id;
    residc_instrument_state_t *is = NULL;
    uint8_t coder = state->residual_coder;

    for (int fi = 0; fi < schema->num_fields; fi++) {
        const residc_field_t *f = &schema->fields[fi];
        uint64_t val = 0;

        switch (f->type) {

        case RESIDC_TIMESTAMP: {
            int64_t predicted_gap = state->timestamp_gap_ema >> 16;
            if (predicted_gap < 0) predicted_gap = 0;

            int k = residc_adaptive_k(state->ts_adapt_sum,
                                       state->ts_adapt_count,
                                       k_timestamp(state->regime),
                                       k_timestamp(state->regime) + 10);
            int64_t residual = decode_residual_dispatch(br, k, coder);
            int64_t gap = residual + predicted_gap;
            val = state->last_timestamp + (uint64_t)gap;

            /* fused state: TIMESTAMP */
            {
                uint64_t zz = residc_zigzag_enc(residual);
                residc_adaptive_update(&state->ts_adapt_sum,
                                       &state->ts_adapt_count, zz);
                int64_t gap_q16 = (int64_t)((uint64_t)gap << 16);
                state->timestamp_gap_ema +=
                    (gap_q16 - state->timestamp_gap_ema) >> 2;
                state->last_timestamp = val;
                state->last_timestamp_gap = (uint64_t)gap;
            }
            break;
        }

        case RESIDC_INSTRUMENT: {
            if (br_read_bit(br) == 0) {
                instrument_id = state->last_instrument_id;
            } else {
                if (br_read_bit(br) == 0) {
                    int rank = decode_mfu_rank(br);
                    int idx = (int)state->mfu.rank_to_idx[rank];
                    instrument_id = state->mfu.entries[idx].instrument_id;
                } else {
                    instrument_id = (uint16_t)br_read(br, 14);
                }
            }
            is = (instrument_id < RESIDC_MAX_INSTRUMENTS)
                 ? &state->instruments[instrument_id] : NULL;
            val = instrument_id;

            /* fused state: INSTRUMENT */
            residc_mfu_update(&state->mfu, instrument_id);
            state->mfu_decay_counter++;
            if (state->mfu_decay_counter >= 10000) {
                mfu_decay(&state->mfu);
                state->mfu_decay_counter = 0;
            }
            state->last_instrument_id = instrument_id;
            break;
        }

        case RESIDC_PRICE: {
            uint32_t predicted = (is && is->msg_count > 0)
                               ? is->last_price : 0;
            int k = residc_adaptive_k(state->price_adapt_sum,
                                       state->price_adapt_count,
                                       k_price(state->regime),
                                       k_price(state->regime) + 10);

            int mode = br_read_bit(br);
            int64_t residual = decode_residual_dispatch(br, k, coder);

            uint32_t price;
            if (mode == 0) {
                price = (uint32_t)((int64_t)(predicted / 100) + residual) * 100;
            } else {
                price = (uint32_t)((int64_t)predicted + residual);
            }
            val = price;

            /* fused state: PRICE */
            {
                /* Track the coded residual for adaptive k */
                uint64_t zz = residc_zigzag_enc(residual);
                residc_adaptive_update(&state->price_adapt_sum,
                                       &state->price_adapt_count, zz);
                int64_t price_residual = (int64_t)price - (int64_t)predicted;
                uint64_t abs_res = (uint64_t)(price_residual < 0
                    ? -(uint64_t)price_residual : (uint64_t)price_residual);
                state->recent_abs_price_sum += (uint32_t)abs_res;
                state->regime_counter++;
                if (state->regime_counter >= RESIDC_REGIME_WINDOW) {
                    uint32_t avg = state->recent_abs_price_sum /
                                   RESIDC_REGIME_WINDOW;
                    state->regime = (avg > 30) ? RESIDC_REGIME_VOLATILE
                                               : RESIDC_REGIME_CALM;
                    state->recent_abs_price_sum = 0;
                    state->regime_counter = 0;
                }
                if (is) is->last_price = price;
            }
            break;
        }

        case RESIDC_QUANTITY: {
            uint32_t predicted = (is && is->msg_count > 0)
                               ? is->last_qty : 100;
            int k = residc_adaptive_k(state->qty_adapt_sum,
                                       state->qty_adapt_count,
                                       k_quantity(state->regime),
                                       k_quantity(state->regime) + 10);

            if (br_read_bit(br) == 0) {
                val = predicted;
            } else {
                int mode = br_read_bit(br);
                int64_t residual = decode_residual_dispatch(br, k, coder);
                if (mode == 0) {
                    val = (uint32_t)((int64_t)(predicted / 100) +
                                     residual) * 100;
                } else {
                    val = (uint32_t)((int64_t)predicted + residual);
                }
                {
                    uint64_t zz = residc_zigzag_enc(residual);
                    residc_adaptive_update(&state->qty_adapt_sum,
                                           &state->qty_adapt_count, zz);
                }
            }

            /* fused state: QUANTITY */
            if (is) is->last_qty = (uint32_t)val;
            break;
        }

        case RESIDC_SEQUENTIAL_ID: {
            residc_field_state_t *fs = &state->field_state[fi];
            uint64_t predicted = (is && is->last_seq_id > 0)
                               ? is->last_seq_id : fs->last_value;
            int k = residc_adaptive_k(fs->adapt_sum, fs->adapt_count,
                                       k_seqid(state->regime),
                                       k_seqid(state->regime) + 10);
            int64_t delta = decode_residual_dispatch(br, k, coder);
            val = predicted + (uint64_t)delta;

            /* fused state: SEQUENTIAL_ID */
            {
                uint64_t zz = residc_zigzag_enc(delta);
                residc_adaptive_update(&fs->adapt_sum, &fs->adapt_count, zz);
                fs->last_value = val;
                if (is) is->last_seq_id = val;
            }
            break;
        }

        case RESIDC_ENUM: {
            residc_field_state_t *fs = &state->field_state[fi];
            if (br_read_bit(br) == 0) {
                val = fs->last_value;
            } else {
                val = br_read(br, f->size * 8);
            }
            /* fused state: ENUM */
            fs->last_value = val;
            break;
        }

        case RESIDC_BOOL:
            val = (uint64_t)br_read_bit(br);
            break;

        case RESIDC_CATEGORICAL: {
            residc_field_state_t *fs = &state->field_state[fi];
            if (br_read_bit(br) == 0) {
                val = fs->last_value;
            } else {
                uint64_t hi = br_read(br, 16);
                uint64_t lo = br_read(br, 16);
                val = (hi << 16) | lo;
            }
            /* fused state: CATEGORICAL */
            fs->last_value = val;
            break;
        }

        case RESIDC_RAW:
            if (f->size <= 8) {
                val = br_read(br, f->size * 8);
            } else {
                uint8_t *p = (uint8_t *)msg + f->offset;
                for (int i = 0; i < f->size; i++)
                    p[i] = (uint8_t)br_read(br, 8);
                continue;  /* skip write_field */
            }
            break;

        case RESIDC_DELTA_ID: {
            uint64_t ref_val = (f->ref_field >= 0)
                ? read_field(msg, schema->fields[f->ref_field].offset,
                             schema->fields[f->ref_field].size)
                : state->field_state[fi].last_value;
            int k = k_seqid(state->regime);
            int64_t delta = decode_residual_dispatch(br, k, coder);
            val = ref_val + (uint64_t)delta;
            /* fused state: DELTA_ID */
            state->field_state[fi].last_value = val;
            break;
        }

        case RESIDC_DELTA_PRICE: {
            uint64_t ref_val = (f->ref_field >= 0)
                ? read_field(msg, schema->fields[f->ref_field].offset,
                             schema->fields[f->ref_field].size)
                : (is ? is->last_price : 0);
            int k = residc_adaptive_k(state->price_adapt_sum,
                                       state->price_adapt_count,
                                       k_price(state->regime),
                                       k_price(state->regime) + 10);
            int64_t delta = decode_residual_dispatch(br, k, coder);
            val = (uint64_t)((int64_t)ref_val + delta);
            break;
        }

        case RESIDC_COMPUTED:
            continue;  /* not decoded */
        }

        write_field(msg, f->offset, f->size, val);
    }

    /* fused: increment per-instrument and global msg_count */
    if (is) is->msg_count++;
    state->msg_count++;

    return 0;
}

int residc_decode(residc_state_t *state, const uint8_t *in, int in_len,
                  void *msg)
{
    const residc_schema_t *schema = state->schema;
    if (!schema || in_len < 1) return -1;

    memset(msg, 0, schema->msg_size);

    uint8_t frame = in[0];

    if (frame == RESIDC_FRAME_LITERAL) {
        /* Literal: deserialize raw fields */
        int pos = 1;
        for (int fi = 0; fi < schema->num_fields; fi++) {
            const residc_field_t *f = &schema->fields[fi];
            if (f->type == RESIDC_COMPUTED) continue;
            uint64_t val = 0;
            for (int b = f->size - 1; b >= 0; b--) {
                if (pos >= in_len) return -1;
                val |= (uint64_t)in[pos++] << (b * 8);
            }
            write_field(msg, f->offset, f->size, val);
        }
        commit_state(state, schema, msg);
        return pos;
    }

    /* Compressed */
    int payload_len = (int)frame;
    if (1 + payload_len > in_len) return -1;

    residc_bitreader_t br;
    residc_br_init(&br, in + 1, payload_len);

    decode_fields(state, schema, &br, msg);
    /* state already updated by decode_fields */

    return 1 + payload_len;
}

/* ================================================================
 * Multi-type Encoder/Decoder
 * ================================================================ */

/* Huffman-like prefix: index 0 = "0", 1 = "10", 2 = "110", etc.
 * Last type gets same length as second-to-last. */
static void encode_msg_type(residc_bitwriter_t *bw, int index, int num_types,
                            int last_index)
{
    (void)last_index;
    /* Simple variable-length: index 0 = 1 bit, index N = N+1 bits */
    if (index < num_types - 1) {
        for (int i = 0; i < index; i++)
            residc_bw_write(bw, 1, 1);
        residc_bw_write(bw, 0, 1);
    } else {
        for (int i = 0; i < num_types - 1; i++)
            residc_bw_write(bw, 1, 1);
    }
}

static int decode_msg_type(residc_bitreader_t *br, int num_types)
{
    for (int i = 0; i < num_types - 1; i++) {
        if (residc_br_read_bit(br) == 0)
            return i;
    }
    return num_types - 1;
}

int residc_encode_multi(residc_state_t *state, const void *msg,
                        uint8_t *out, int capacity)
{
    const residc_multi_schema_t *multi = state->multi_schema;
    if (!multi || capacity < 2) return -1;

    uint8_t type_val = *(const uint8_t *)((const uint8_t *)msg +
                                           multi->type_offset);
    int type_idx = multi->type_to_index(type_val);
    if (type_idx < 0 || type_idx >= multi->num_types) return -1;

    const residc_schema_t *schema = &multi->schemas[type_idx];

    residc_bitwriter_t bw;
    residc_bw_init(&bw);

    /* Encode message type with same-as-last flag */
    if (type_idx == state->last_msg_type_index && state->msg_count > 0) {
        residc_bw_write(&bw, 0, 1);
    } else {
        residc_bw_write(&bw, 1, 1);
        encode_msg_type(&bw, type_idx, multi->num_types,
                        state->last_msg_type_index);
    }

    encode_fields(state, schema, msg, &bw);

    int payload_len = residc_bw_finish(&bw);
    int raw_size = residc_raw_size(schema) + 1;

    if (payload_len >= raw_size || payload_len >= 254) {
        /* Literal fallback */
        if (capacity < 1 + raw_size) return -1;
        out[0] = RESIDC_FRAME_LITERAL;
        out[1] = type_val;
        int pos = 2;
        for (int fi = 0; fi < schema->num_fields; fi++) {
            const residc_field_t *f = &schema->fields[fi];
            if (f->type == RESIDC_COMPUTED) continue;
            uint64_t val = read_field(msg, f->offset, f->size);
            for (int b = f->size - 1; b >= 0; b--)
                out[pos++] = (uint8_t)(val >> (b * 8));
        }
        /* state already updated by encode_fields */
        state->last_msg_type_index = (uint8_t)type_idx;
        return pos;
    }

    out[0] = (uint8_t)payload_len;
    memcpy(out + 1, bw.buf, payload_len);

    /* state already updated by encode_fields */
    state->last_msg_type_index = (uint8_t)type_idx;
    return 1 + payload_len;
}

int residc_decode_multi(residc_state_t *state, const uint8_t *in, int in_len,
                        void *msg)
{
    const residc_multi_schema_t *multi = state->multi_schema;
    if (!multi || in_len < 1) return -1;

    uint8_t frame = in[0];

    if (frame == RESIDC_FRAME_LITERAL) {
        if (in_len < 2) return -1;
        uint8_t type_val = in[1];
        int type_idx = multi->type_to_index(type_val);
        if (type_idx < 0) return -1;
        const residc_schema_t *schema = &multi->schemas[type_idx];

        memset(msg, 0, schema->msg_size);
        *(uint8_t *)((uint8_t *)msg + multi->type_offset) = type_val;

        int pos = 2;
        for (int fi = 0; fi < schema->num_fields; fi++) {
            const residc_field_t *f = &schema->fields[fi];
            if (f->type == RESIDC_COMPUTED) continue;
            uint64_t val = 0;
            for (int b = f->size - 1; b >= 0; b--) {
                if (pos >= in_len) return -1;
                val |= (uint64_t)in[pos++] << (b * 8);
            }
            write_field(msg, f->offset, f->size, val);
        }
        commit_state(state, schema, msg);
        state->last_msg_type_index = (uint8_t)type_idx;
        return pos;
    }

    int payload_len = (int)frame;
    if (1 + payload_len > in_len) return -1;

    residc_bitreader_t br;
    residc_br_init(&br, in + 1, payload_len);

    /* Decode message type */
    int type_idx;
    if (residc_br_read_bit(&br) == 0) {
        type_idx = state->last_msg_type_index;
    } else {
        type_idx = decode_msg_type(&br, multi->num_types);
    }

    const residc_schema_t *schema = &multi->schemas[type_idx];
    uint8_t type_val = multi->index_to_type(type_idx);

    memset(msg, 0, schema->msg_size);
    *(uint8_t *)((uint8_t *)msg + multi->type_offset) = type_val;

    decode_fields(state, schema, &br, msg);
    /* state already updated by decode_fields */
    state->last_msg_type_index = (uint8_t)type_idx;

    return 1 + payload_len;
}
