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

/* Fast inline version for internal hot path */
static inline __attribute__((always_inline))
void bw_write(residc_bitwriter_t *bw, uint64_t val, int nbits)
{
    bw->accum = (bw->accum << nbits) | (val & ((1ULL << nbits) - 1));
    bw->count += nbits;
    while (__builtin_expect(bw->count >= 8, 0)) {
        if (bw->byte_pos >= RESIDC_SCRATCH_BYTES) return;
        bw->count -= 8;
        bw->buf[bw->byte_pos++] = (uint8_t)(bw->accum >> bw->count);
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

/* Fast inline versions for internal hot path */
static inline __attribute__((always_inline))
void br_refill_inline(residc_bitreader_t *br)
{
    while (br->count <= 56 && br->byte_pos < br->len_bytes) {
        br->accum = (br->accum << 8) | br->data[br->byte_pos++];
        br->count += 8;
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
        residc_bw_write(bw, 0, 1);           /* tier 0: 0 + k bits */
        residc_bw_write(bw, zz, k);
    } else if (zz < (1ULL << (k + 6))) {
        residc_bw_write(bw, 0x2, 2);         /* tier 1: 10 + (k+6) bits */
        residc_bw_write(bw, zz, k + 6);
    } else if (zz < (1ULL << (k + 12))) {
        residc_bw_write(bw, 0x6, 3);         /* tier 2: 110 + (k+12) bits */
        residc_bw_write(bw, zz, k + 12);
    } else if (zz < (1ULL << (k + 20))) {
        residc_bw_write(bw, 0xE, 4);         /* tier 3: 1110 + (k+20) bits */
        residc_bw_write(bw, zz, k + 20);
    } else {
        residc_bw_write(bw, 0xF, 4);         /* tier 4: 1111 + 64 raw bits */
        residc_bw_write(bw, (uint64_t)value >> 32, 32);
        residc_bw_write(bw, (uint64_t)value & 0xFFFFFFFF, 32);
    }
}

int64_t residc_decode_residual(residc_bitreader_t *br, int k)
{
    if (residc_br_read_bit(br) == 0) {
        /* tier 0 */
        uint64_t zz = residc_br_read(br, k);
        return residc_zigzag_dec(zz);
    }
    if (residc_br_read_bit(br) == 0) {
        /* tier 1 */
        uint64_t zz = residc_br_read(br, k + 6);
        return residc_zigzag_dec(zz);
    }
    if (residc_br_read_bit(br) == 0) {
        /* tier 2 */
        uint64_t zz = residc_br_read(br, k + 12);
        return residc_zigzag_dec(zz);
    }
    if (residc_br_read_bit(br) == 0) {
        /* tier 3 */
        uint64_t zz = residc_br_read(br, k + 20);
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
        bw_write(bw, 0, 1);
        bw_write(bw, zz, k);
    } else if (zz < (1ULL << (k + 6))) {
        bw_write(bw, 0x2, 2);
        bw_write(bw, zz, k + 6);
    } else if (zz < (1ULL << (k + 12))) {
        bw_write(bw, 0x6, 3);
        bw_write(bw, zz, k + 12);
    } else if (zz < (1ULL << (k + 20))) {
        bw_write(bw, 0xE, 4);
        bw_write(bw, zz, k + 20);
    } else {
        bw_write(bw, 0xF, 4);
        bw_write(bw, (uint64_t)value >> 32, 32);
        bw_write(bw, (uint64_t)value & 0xFFFFFFFF, 32);
    }
}

static inline __attribute__((always_inline))
int64_t decode_residual(residc_bitreader_t *br, int k)
{
    if (br_read_bit(br) == 0) {
        return residc_zigzag_dec(br_read(br, k));
    }
    if (br_read_bit(br) == 0) {
        return residc_zigzag_dec(br_read(br, k + 6));
    }
    if (br_read_bit(br) == 0) {
        return residc_zigzag_dec(br_read(br, k + 12));
    }
    if (br_read_bit(br) == 0) {
        return residc_zigzag_dec(br_read(br, k + 20));
    }
    uint64_t hi = br_read(br, 32);
    uint64_t lo = br_read(br, 32);
    return (int64_t)((hi << 32) | lo);
}

/* ================================================================
 * Adaptive k
 * ================================================================ */

int residc_adaptive_k(uint64_t sum, uint32_t count, int min_k, int max_k)
{
    if (count == 0) return min_k;
    uint64_t avg = sum / count;
    int k = 0;
    while (avg > 0 && k < max_k) { k++; avg >>= 1; }
    return k < min_k ? min_k : k;
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
    for (int i = 0; i < RESIDC_MFU_SIZE; i++)
        mfu->chain[i] = 0xFFFF;
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

/* Periodic decay to adapt to changing instrument distributions */
static void mfu_decay(residc_mfu_table_t *mfu)
{
    for (int i = 0; i < mfu->num_entries; i++)
        mfu->entries[i].count >>= 1;
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

static int encode_fields(residc_state_t *state, const residc_schema_t *schema,
                         const void *msg, residc_bitwriter_t *bw)
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

            int k = residc_adaptive_k(state->ts_adapt_sum,
                                       state->ts_adapt_count,
                                       k_timestamp(state->regime),
                                       k_timestamp(state->regime) + 10);
            encode_residual(bw, residual, k);

            uint64_t zz = residc_zigzag_enc(residual);
            residc_adaptive_update(&state->ts_adapt_sum,
                                   &state->ts_adapt_count, zz);

            /* Update EMA: ema = ema + (gap - ema) / 4 in Q16 */
            int64_t gap_q16 = (int64_t)((uint64_t)gap << 16);
            state->timestamp_gap_ema +=
                (gap_q16 - state->timestamp_gap_ema) >> 2;
            state->last_timestamp = ts;
            state->last_timestamp_gap = (uint64_t)gap;
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
                    bw_write(bw, (uint64_t)mfu_idx,
                                    RESIDC_MFU_INDEX_BITS);
                } else {
                    bw_write(bw, 1, 1);  /* raw */
                    bw_write(bw, instrument_id, 14);
                }
            }
            /* Deferred: MFU update after commit */
            break;
        }

        case RESIDC_PRICE: {
            uint32_t price = (uint32_t)val;
            uint32_t predicted = (is && is->msg_count > 0)
                               ? is->last_price : 0;
            int64_t residual = (int64_t)price - (int64_t)predicted;

            int k = k_price(state->regime);

            /* Penny normalization */
            if (price % 100 == 0 && predicted % 100 == 0 &&
                (price > 0 || predicted > 0)) {
                bw_write(bw, 0, 1);  /* penny mode */
                encode_residual(bw, residual / 100, k);
            } else {
                bw_write(bw, 1, 1);  /* sub-penny mode */
                encode_residual(bw, residual, k);
            }

            /* Regime tracking */
            uint64_t abs_res = (uint64_t)(residual < 0 ? -(uint64_t)residual : (uint64_t)residual);
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
            break;
        }

        case RESIDC_QUANTITY: {
            uint32_t qty = (uint32_t)val;
            uint32_t predicted = (is && is->msg_count > 0)
                               ? is->last_qty : 100;
            int64_t residual = (int64_t)qty - (int64_t)predicted;

            int k = k_quantity(state->regime);

            if (residual == 0) {
                bw_write(bw, 0, 1);  /* same as predicted */
            } else {
                bw_write(bw, 1, 1);  /* different */
                if (qty % 100 == 0 && predicted % 100 == 0) {
                    bw_write(bw, 0, 1);  /* round lot */
                    encode_residual(bw, residual / 100, k);
                } else {
                    bw_write(bw, 1, 1);  /* odd lot */
                    encode_residual(bw, residual, k);
                }
            }
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
            encode_residual(bw, delta, k);

            uint64_t zz = residc_zigzag_enc(delta);
            residc_adaptive_update(&fs->adapt_sum, &fs->adapt_count, zz);
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
            encode_residual(bw, delta, k);
            break;
        }

        case RESIDC_DELTA_PRICE: {
            uint64_t ref_val = (f->ref_field >= 0)
                ? read_field(msg, schema->fields[f->ref_field].offset,
                             schema->fields[f->ref_field].size)
                : (is ? is->last_price : 0);
            int64_t delta = (int64_t)val - (int64_t)ref_val;
            int k = k_price(state->regime);
            encode_residual(bw, delta, k);
            break;
        }

        case RESIDC_COMPUTED:
            /* Not encoded — 0 bits */
            break;
        }
    }

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
        case RESIDC_PRICE:
            if (is) is->last_price = (uint32_t)val;
            break;
        case RESIDC_QUANTITY:
            if (is) is->last_qty = (uint32_t)val;
            break;
        case RESIDC_SEQUENTIAL_ID:
            state->field_state[fi].last_value = val;
            if (is) is->last_seq_id = val;
            break;
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
        commit_state(state, schema, msg);
        return pos;
    }

    /* Compressed frame */
    if (capacity < 1 + payload_len) return -1;
    out[0] = (uint8_t)payload_len;
    memcpy(out + 1, bw.buf, payload_len);

    commit_state(state, schema, msg);
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
            int64_t residual = decode_residual(br, k);
            int64_t gap = residual + predicted_gap;
            val = state->last_timestamp + (uint64_t)gap;

            uint64_t zz = residc_zigzag_enc(residual);
            residc_adaptive_update(&state->ts_adapt_sum,
                                   &state->ts_adapt_count, zz);

            int64_t gap_q16 = (int64_t)((uint64_t)gap << 16);
            state->timestamp_gap_ema +=
                (gap_q16 - state->timestamp_gap_ema) >> 2;
            state->last_timestamp = val;
            state->last_timestamp_gap = (uint64_t)gap;
            break;
        }

        case RESIDC_INSTRUMENT: {
            if (br_read_bit(br) == 0) {
                instrument_id = state->last_instrument_id;
            } else {
                if (br_read_bit(br) == 0) {
                    int idx = (int)br_read(br, RESIDC_MFU_INDEX_BITS);
                    instrument_id = state->mfu.entries[idx].instrument_id;
                } else {
                    instrument_id = (uint16_t)br_read(br, 14);
                }
            }
            is = (instrument_id < RESIDC_MAX_INSTRUMENTS)
                 ? &state->instruments[instrument_id] : NULL;
            val = instrument_id;
            break;
        }

        case RESIDC_PRICE: {
            uint32_t predicted = (is && is->msg_count > 0)
                               ? is->last_price : 0;
            int k = k_price(state->regime);

            int mode = br_read_bit(br);
            int64_t residual = decode_residual(br, k);

            uint32_t price;
            if (mode == 0) {
                price = (uint32_t)((int64_t)(predicted / 100) + residual) * 100;
            } else {
                price = (uint32_t)((int64_t)predicted + residual);
            }
            val = price;

            /* Regime tracking */
            int64_t full_res = (int64_t)price - (int64_t)predicted;
            uint64_t abs_res = (uint64_t)(full_res < 0 ? -(uint64_t)full_res : (uint64_t)full_res);
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
            break;
        }

        case RESIDC_QUANTITY: {
            uint32_t predicted = (is && is->msg_count > 0)
                               ? is->last_qty : 100;
            int k = k_quantity(state->regime);

            if (br_read_bit(br) == 0) {
                val = predicted;  /* same */
            } else {
                int mode = br_read_bit(br);
                int64_t residual = decode_residual(br, k);
                if (mode == 0) {
                    val = (uint32_t)((int64_t)(predicted / 100) +
                                     residual) * 100;
                } else {
                    val = (uint32_t)((int64_t)predicted + residual);
                }
            }
            break;
        }

        case RESIDC_SEQUENTIAL_ID: {
            residc_field_state_t *fs = &state->field_state[fi];
            uint64_t predicted = (is && is->last_seq_id > 0)
                               ? is->last_seq_id : fs->last_value;
            int k = residc_adaptive_k(fs->adapt_sum, fs->adapt_count,
                                       k_seqid(state->regime),
                                       k_seqid(state->regime) + 10);
            int64_t delta = decode_residual(br, k);
            val = predicted + (uint64_t)delta;

            uint64_t zz = residc_zigzag_enc(delta);
            residc_adaptive_update(&fs->adapt_sum, &fs->adapt_count, zz);
            break;
        }

        case RESIDC_ENUM: {
            residc_field_state_t *fs = &state->field_state[fi];
            if (br_read_bit(br) == 0) {
                val = fs->last_value;
            } else {
                val = br_read(br, f->size * 8);
            }
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
            int64_t delta = decode_residual(br, k);
            val = ref_val + (uint64_t)delta;
            break;
        }

        case RESIDC_DELTA_PRICE: {
            uint64_t ref_val = (f->ref_field >= 0)
                ? read_field(msg, schema->fields[f->ref_field].offset,
                             schema->fields[f->ref_field].size)
                : (is ? is->last_price : 0);
            int k = k_price(state->regime);
            int64_t delta = decode_residual(br, k);
            val = (uint64_t)((int64_t)ref_val + delta);
            break;
        }

        case RESIDC_COMPUTED:
            continue;  /* not decoded */
        }

        write_field(msg, f->offset, f->size, val);
    }

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
    commit_state(state, schema, msg);

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
        commit_state(state, schema, msg);
        state->last_msg_type_index = (uint8_t)type_idx;
        return pos;
    }

    out[0] = (uint8_t)payload_len;
    memcpy(out + 1, bw.buf, payload_len);

    commit_state(state, schema, msg);
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
    commit_state(state, schema, msg);
    state->last_msg_type_index = (uint8_t)type_idx;

    return 1 + payload_len;
}
