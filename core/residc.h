/*
 * residc.h — Schema-Driven Prediction-Residual Compression for Financial Data
 *
 * A general-purpose, domain-specific compression library for financial
 * messages. Users define a schema (field types), and the library applies
 * per-field prediction strategies optimized for financial data patterns.
 *
 * Key properties:
 *   - Per-message: each message compressed independently (random access)
 *   - Streaming: encoder/decoder maintain synchronized state
 *   - Zero-heap: no dynamic allocation, stack-only operation
 *   - Deterministic: bit-identical output on all conforming implementations
 *   - Integer-only: no floating-point arithmetic
 *
 * Measured performance (NASDAQ ITCH 5.0, 8M real messages):
 *   - 3.27:1 compression ratio (32.2 -> 9.9 bytes average)
 *   - 330+ MB/s encode, 348+ MB/s decode
 *   - Zero roundtrip errors
 *
 * Quick start:
 *
 *   // 1. Define your message struct
 *   typedef struct {
 *       uint64_t timestamp;
 *       uint16_t instrument_id;
 *       uint32_t price;
 *       uint32_t quantity;
 *       uint8_t  side;
 *   } Quote;
 *
 *   // 2. Define a schema (field type, offset, size, ref_field)
 *   static const residc_field_t quote_fields[] = {
 *       { RESIDC_TIMESTAMP,  offsetof(Quote, timestamp),     8, -1 },
 *       { RESIDC_INSTRUMENT, offsetof(Quote, instrument_id), 2, -1 },
 *       { RESIDC_PRICE,      offsetof(Quote, price),         4, -1 },
 *       { RESIDC_QUANTITY,   offsetof(Quote, quantity),       4, -1 },
 *       { RESIDC_ENUM,       offsetof(Quote, side),           1, -1 },
 *   };
 *
 *   static const residc_schema_t quote_schema = {
 *       .fields     = quote_fields,
 *       .num_fields = 5,
 *       .msg_size   = sizeof(Quote),
 *   };
 *
 *   // 3. Create encoder and decoder states
 *   residc_state_t enc, dec;
 *   residc_init(&enc, &quote_schema);
 *   residc_init(&dec, &quote_schema);
 *
 *   // 4. Encode
 *   Quote q = { .timestamp = 34200000000000, .instrument_id = 42,
 *               .price = 1500250, .quantity = 100, .side = 0 };
 *   uint8_t buf[64];
 *   int len = residc_encode(&enc, &q, buf, sizeof(buf));
 *
 *   // 5. Decode
 *   Quote decoded;
 *   residc_decode(&dec, buf, len, &decoded);
 *   // decoded == q  (bit-perfect roundtrip)
 *
 * License: MIT OR Apache-2.0
 * https://github.com/theta-gamma/residc
 */

#ifndef RESIDC_H
#define RESIDC_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Configuration (override before #include if needed)
 * ================================================================ */

/* Maximum number of instruments (per-instrument state tracking) */
#ifndef RESIDC_MAX_INSTRUMENTS
#define RESIDC_MAX_INSTRUMENTS  16384
#endif

/* MFU table size (most-frequently-used instruments) */
#ifndef RESIDC_MFU_SIZE
#define RESIDC_MFU_SIZE         256
#endif

/* Maximum fields per schema */
#ifndef RESIDC_MAX_FIELDS
#define RESIDC_MAX_FIELDS       32
#endif

/* Maximum message types (for multi-message schemas) */
#ifndef RESIDC_MAX_MSG_TYPES
#define RESIDC_MAX_MSG_TYPES    16
#endif

/* Adaptive k window size */
#ifndef RESIDC_ADAPT_WINDOW
#define RESIDC_ADAPT_WINDOW     8
#endif

/* Scratch buffer for bit packing */
#ifndef RESIDC_SCRATCH_BYTES
#define RESIDC_SCRATCH_BYTES    256
#endif

/* Regime detection window */
#ifndef RESIDC_REGIME_WINDOW
#define RESIDC_REGIME_WINDOW    64
#endif

/* MFU index bits (log2 of RESIDC_MFU_SIZE) */
#define RESIDC_MFU_INDEX_BITS   8

/* Regime types */
#define RESIDC_REGIME_CALM      0
#define RESIDC_REGIME_VOLATILE  1

/* Frame markers */
#define RESIDC_FRAME_LITERAL    0xFF

/* ================================================================
 * Field Types
 *
 * Each type has a built-in prediction strategy optimized for its
 * typical behavior in financial data.
 * ================================================================ */

typedef enum {
    /*
     * TIMESTAMP — nanoseconds since midnight (or epoch)
     * Prediction: EMA of inter-message gaps
     * Encoding: tiered residual with adaptive k
     * Typical savings: 40-50% (timestamps are highly predictable)
     */
    RESIDC_TIMESTAMP = 0,

    /*
     * INSTRUMENT — instrument/security identifier (uint16)
     * Prediction: MFU (most-frequently-used) table
     * Encoding: 1-bit same flag, or MFU index (8 bits), or raw (14 bits)
     * Typical savings: 60-70% (a few instruments dominate trading)
     */
    RESIDC_INSTRUMENT = 1,

    /*
     * PRICE — price in fixed-point (uint32, e.g. price * 10000)
     * Prediction: per-instrument last price
     * Encoding: penny normalization + tiered residual
     * Typical savings: 50-70% (prices move in small increments)
     */
    RESIDC_PRICE = 2,

    /*
     * QUANTITY — share/lot quantity (uint32)
     * Prediction: per-instrument last quantity
     * Encoding: zero-residual flag + round-lot normalization + tiered residual
     * Typical savings: 40-60% (common lot sizes repeat)
     */
    RESIDC_QUANTITY = 3,

    /*
     * SEQUENTIAL_ID — monotonically increasing ID (uint64)
     * Prediction: delta from last seen (global or per-instrument)
     * Encoding: tiered residual with adaptive k
     * Typical savings: 60-80% (IDs increment by small amounts)
     */
    RESIDC_SEQUENTIAL_ID = 4,

    /*
     * ENUM — small enumeration (uint8, up to 256 values)
     * Prediction: same-as-last flag
     * Encoding: 1-bit same flag + raw bits if different
     * Typical savings: 70-90% (most fields repeat)
     */
    RESIDC_ENUM = 5,

    /*
     * BOOL — boolean (uint8, 0 or 1)
     * Prediction: none
     * Encoding: 1 bit
     */
    RESIDC_BOOL = 6,

    /*
     * CATEGORICAL — categorical value (uint32, e.g. account ID)
     * Prediction: same-as-last flag
     * Encoding: 1-bit same flag + raw 32 bits if different
     * Typical savings: 80-95% (same account/firm for many messages)
     */
    RESIDC_CATEGORICAL = 7,

    /*
     * RAW — uncompressed field (copied verbatim)
     * Prediction: none
     * Encoding: raw bytes
     * Use for fields with no predictable pattern
     */
    RESIDC_RAW = 8,

    /*
     * DELTA_ID — ID that's a delta from another field in the same message
     * Prediction: delta from a reference field (specified by ref_field)
     * Encoding: tiered residual
     * Use for: orig_cl_ord_id (delta from cl_ord_id), new_order_ref, etc.
     */
    RESIDC_DELTA_ID = 9,

    /*
     * DELTA_PRICE — price that's close to another price field
     * Prediction: value of reference field (specified by ref_field)
     * Encoding: tiered residual
     * Use for: last_px (delta from price), near_price (delta from far_price)
     */
    RESIDC_DELTA_PRICE = 10,

    /*
     * COMPUTED — field derived from other fields (not transmitted)
     * The decode callback reconstructs this field.
     * Encoding: 0 bits
     * Use for: leaves_qty = quantity - cum_qty, ord_status = f(exec_type)
     */
    RESIDC_COMPUTED = 11,

} residc_field_type_t;

/* ================================================================
 * Schema Definition
 * ================================================================ */

/*
 * Field descriptor — one per field in your message struct.
 *
 * Fields are encoded in schema order. The INSTRUMENT field (if any)
 * must appear before any PRICE/QUANTITY fields that depend on
 * per-instrument state.
 */
typedef struct {
    residc_field_type_t type;      /* Field type (prediction strategy) */
    uint16_t            offset;    /* offsetof(YourStruct, field) */
    uint8_t             size;      /* sizeof(field) in bytes: 1,2,4,8 */
    int8_t              ref_field; /* For DELTA_ID/DELTA_PRICE: index of reference field.
                                      For COMPUTED: -1. For others: -1 (unused). */
} residc_field_t;

/*
 * Schema — describes a message format.
 *
 * Create one schema per message type. For multi-type protocols,
 * use residc_multi_schema_t.
 */
typedef struct {
    const residc_field_t *fields;
    int                   num_fields;
    int                   msg_size;     /* sizeof(YourStruct) */
} residc_schema_t;

/*
 * Multi-message schema — for protocols with multiple message types.
 *
 * Message type is encoded with a Huffman-like prefix code.
 * Types should be ordered by frequency (most frequent = index 0).
 */
typedef struct {
    const residc_schema_t *schemas;    /* Array of per-type schemas */
    int                    num_types;  /* Number of message types */
    uint8_t                type_offset;/* offsetof(YourStruct, msg_type_field) */
    uint8_t                type_size;  /* sizeof(msg_type_field): 1 */
    /* Map from type_field value -> schema index (user provides) */
    int                  (*type_to_index)(uint8_t type_value);
    uint8_t              (*index_to_type)(int index);
} residc_multi_schema_t;

/* ================================================================
 * Codec State (maintained by both encoder and decoder)
 * ================================================================ */

/* Per-instrument state */
typedef struct {
    uint32_t last_price;
    uint32_t last_qty;
    uint32_t msg_count;
    uint64_t last_seq_id;
} residc_instrument_state_t;

/* MFU table entry */
typedef struct {
    uint16_t instrument_id;
    uint16_t count;
} residc_mfu_entry_t;

/* MFU table */
typedef struct {
    residc_mfu_entry_t entries[RESIDC_MFU_SIZE];
    uint16_t           num_entries;
    uint16_t           hash[256];
    uint16_t           chain[RESIDC_MFU_SIZE];
} residc_mfu_table_t;

/* Per-field adaptive state */
typedef struct {
    uint64_t last_value;      /* Last seen value for this field */
    uint64_t adapt_sum;       /* Running sum for adaptive k */
    uint32_t adapt_count;     /* Running count for adaptive k */
} residc_field_state_t;

/* Full codec state */
typedef struct {
    /* Schema pointer (set during init) */
    const residc_schema_t       *schema;
    const residc_multi_schema_t *multi_schema;

    /* Global counters */
    uint64_t msg_count;

    /* Timestamp prediction */
    uint64_t last_timestamp;
    int64_t  timestamp_gap_ema;   /* Q16 fixed-point */
    uint64_t last_timestamp_gap;
    uint64_t ts_adapt_sum;
    uint32_t ts_adapt_count;

    /* Instrument tracking */
    uint16_t last_instrument_id;
    residc_mfu_table_t mfu;
    uint32_t mfu_decay_counter;

    /* Regime detection */
    uint8_t  regime;
    uint32_t regime_counter;
    uint32_t recent_abs_price_sum;

    /* Per-field state (for ENUM, CATEGORICAL, SEQUENTIAL_ID) */
    residc_field_state_t field_state[RESIDC_MAX_FIELDS];

    /* Per-instrument state */
    residc_instrument_state_t instruments[RESIDC_MAX_INSTRUMENTS];

    /* Last message type (for multi-schema) */
    uint8_t last_msg_type_index;
} residc_state_t;

/* ================================================================
 * Bit Writer / Reader (exposed for advanced users)
 * ================================================================ */

typedef struct {
    uint8_t  buf[RESIDC_SCRATCH_BYTES];
    uint64_t accum;
    int      count;
    int      byte_pos;
} residc_bitwriter_t;

typedef struct {
    const uint8_t *data;
    int            len_bytes;
    uint64_t       accum;
    int            count;
    int            byte_pos;
} residc_bitreader_t;

/* ================================================================
 * Public API
 * ================================================================ */

/*
 * Initialize codec state for a single-type schema.
 * Call this for both encoder and decoder with the same schema.
 */
void residc_init(residc_state_t *state, const residc_schema_t *schema);

/*
 * Initialize codec state for a multi-type schema.
 */
void residc_init_multi(residc_state_t *state, const residc_multi_schema_t *multi);

/*
 * Encode a message.
 *
 * @param state     Codec state (updated after encoding)
 * @param msg       Pointer to your message struct
 * @param out       Output buffer for compressed data
 * @param capacity  Size of output buffer (64 bytes is usually enough)
 * @return          Compressed size in bytes, or -1 on error
 *
 * The first byte of output is a frame byte:
 *   1-254: compressed payload of that length follows
 *   0xFF:  literal (uncompressed) payload follows
 */
int residc_encode(residc_state_t *state, const void *msg,
                  uint8_t *out, int capacity);

/*
 * Decode a message.
 *
 * @param state     Codec state (updated after decoding)
 * @param in        Compressed data (including frame byte)
 * @param in_len    Length of compressed data
 * @param msg       Pointer to your message struct (filled on return)
 * @return          Number of bytes consumed, or -1 on error
 */
int residc_decode(residc_state_t *state, const uint8_t *in, int in_len,
                  void *msg);

/*
 * Encode a message with a multi-type schema.
 * The msg_type field is read from the struct.
 */
int residc_encode_multi(residc_state_t *state, const void *msg,
                        uint8_t *out, int capacity);

/*
 * Decode a message with a multi-type schema.
 * The msg_type field is written to the struct.
 */
int residc_decode_multi(residc_state_t *state, const uint8_t *in, int in_len,
                        void *msg);

/*
 * Reset codec state to initial values.
 * Both encoder and decoder must be reset together.
 */
void residc_reset(residc_state_t *state);

/*
 * Get the raw (uncompressed) size of a message given its schema.
 */
int residc_raw_size(const residc_schema_t *schema);

/* ================================================================
 * Building Blocks (for advanced users building custom codecs)
 *
 * These are the primitives used internally. You can use them
 * to build custom codecs that go beyond the schema-driven API.
 * ================================================================ */

/* --- Bit writer --- */
void    residc_bw_init(residc_bitwriter_t *bw);
void    residc_bw_write(residc_bitwriter_t *bw, uint64_t val, int nbits);
int     residc_bw_finish(residc_bitwriter_t *bw);

/* --- Bit reader --- */
void    residc_br_init(residc_bitreader_t *br, const uint8_t *data, int len);
uint64_t residc_br_read(residc_bitreader_t *br, int nbits);
int     residc_br_read_bit(residc_bitreader_t *br);

/* --- Zigzag encoding (signed <-> unsigned) --- */
static inline uint64_t residc_zigzag_enc(int64_t v)
{ return (uint64_t)((v << 1) ^ (v >> 63)); }

static inline int64_t residc_zigzag_dec(uint64_t u)
{ return (int64_t)((u >> 1) ^ -(u & 1)); }

/*
 * Tiered residual encoding.
 *
 * Encodes a signed residual using a 5-tier variable-width code:
 *   Tier 0: 0  + k bits          (values < 2^k)
 *   Tier 1: 10 + (k+6) bits      (values < 2^(k+6))
 *   Tier 2: 110 + (k+12) bits    (values < 2^(k+12))
 *   Tier 3: 1110 + (k+20) bits   (values < 2^(k+20))
 *   Tier 4: 1111 + 64 raw bits   (any value)
 *
 * The k parameter controls the boundary between tiers.
 * Small k = fewer bits for small residuals (good when predictions are accurate).
 * Large k = fewer bits for large residuals (good for volatile data).
 */
void    residc_encode_residual(residc_bitwriter_t *bw, int64_t value, int k);
int64_t residc_decode_residual(residc_bitreader_t *br, int k);

/*
 * Adaptive k computation (JPEG-LS style).
 *
 * Given a running average of zigzag-encoded residuals, computes
 * the optimal k parameter: k = floor(log2(avg + 1)).
 */
int     residc_adaptive_k(uint64_t sum, uint32_t count, int min_k, int max_k);
void    residc_adaptive_update(uint64_t *sum, uint32_t *count, uint64_t val);

/*
 * MFU table operations.
 *
 * The MFU (Most Frequently Used) table tracks the top instruments
 * by frequency and provides fast lookup via hash+chain.
 */
void    residc_mfu_init(residc_mfu_table_t *mfu);
int     residc_mfu_lookup(const residc_mfu_table_t *mfu, uint16_t id);
void    residc_mfu_update(residc_mfu_table_t *mfu, uint16_t id);

/* ================================================================
 * State Checkpoint / Restore
 *
 * For gap recovery: snapshot codec state periodically, restore
 * and replay from the snapshot if messages are lost.
 * The snapshot is a full copy of the state struct (~332KB).
 * ================================================================ */

/*
 * Take a snapshot of codec state.
 * @param state  Source codec state
 * @param snap   Destination (caller-allocated residc_state_t)
 */
void residc_snapshot(const residc_state_t *state, residc_state_t *snap);

/*
 * Restore codec state from a snapshot.
 * Preserves the current schema/multi_schema pointers.
 * @param state  Destination codec state (to restore into)
 * @param snap   Source snapshot
 */
void residc_restore(residc_state_t *state, const residc_state_t *snap);

/* ================================================================
 * MFU Pre-Seeding
 *
 * Initialize the MFU table with known instruments for faster
 * warm-up. Must be called identically on encoder and decoder.
 * ================================================================ */

/*
 * Pre-seed the MFU table with known instrument frequencies.
 * @param mfu     MFU table (from state->mfu)
 * @param ids     Array of instrument IDs (sorted by descending frequency)
 * @param counts  Array of frequency counts (parallel to ids)
 * @param n       Number of entries (capped at RESIDC_MFU_SIZE)
 */
void residc_mfu_seed(residc_mfu_table_t *mfu, const uint16_t *ids,
                     const uint16_t *counts, int n);

#ifdef __cplusplus
}
#endif

#endif /* RESIDC_H */
