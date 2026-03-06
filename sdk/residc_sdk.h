/*
 * residc SDK — Clean C API for per-message financial data compression.
 *
 * This SDK wraps the core residc codec with:
 *   - Heap-allocated codec instances (no 330KB stack objects)
 *   - Simple uint64_t values[] interface (no raw struct pointers)
 *   - Automatic field layout computation
 *
 * Build: link against libresdc.so or libresdc.a
 *
 * License: MIT OR Apache-2.0
 */

#ifndef RESIDC_SDK_H
#define RESIDC_SDK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Field Types (same values as core/residc.h)
 * ================================================================ */

#define RESIDC_SDK_TIMESTAMP      0
#define RESIDC_SDK_INSTRUMENT     1
#define RESIDC_SDK_PRICE          2
#define RESIDC_SDK_QUANTITY       3
#define RESIDC_SDK_SEQUENTIAL_ID  4
#define RESIDC_SDK_ENUM           5
#define RESIDC_SDK_BOOL           6
#define RESIDC_SDK_CATEGORICAL    7
#define RESIDC_SDK_RAW            8
#define RESIDC_SDK_DELTA_ID       9
#define RESIDC_SDK_DELTA_PRICE    10
#define RESIDC_SDK_COMPUTED       11

/* ================================================================
 * Opaque Codec Handle
 * ================================================================ */

typedef struct residc_codec residc_codec_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */

/*
 * Create a codec.
 *
 * @param types       Array of field types (RESIDC_SDK_TIMESTAMP, etc.)
 * @param ref_fields  Array of reference field indices for DELTA_* types.
 *                    Use -1 for non-delta fields. Pass NULL if no deltas.
 * @param num_fields  Number of fields
 * @return            Heap-allocated codec, or NULL on error.
 *                    Caller must call residc_codec_destroy() when done.
 */
residc_codec_t *residc_codec_create(const int *types, const int8_t *ref_fields,
                                    int num_fields);

/*
 * Destroy a codec (or snapshot). Frees all resources.
 */
void residc_codec_destroy(residc_codec_t *codec);

/* ================================================================
 * Encode / Decode
 * ================================================================ */

/*
 * Encode a message.
 *
 * @param codec     Codec state (updated on success)
 * @param values    Array of field values (one uint64 per field)
 * @param out       Output buffer for compressed data
 * @param capacity  Size of output buffer (64 bytes is usually enough)
 * @return          Compressed size in bytes, or -1 on error
 */
int residc_codec_encode(residc_codec_t *codec, const uint64_t *values,
                        uint8_t *out, int capacity);

/*
 * Decode a message.
 *
 * @param codec     Codec state (updated on success)
 * @param in        Compressed data (as returned by encode)
 * @param in_len    Length of compressed data
 * @param values    Output array (one uint64 per field, caller-allocated)
 * @return          Bytes consumed from input, or -1 on error
 */
int residc_codec_decode(residc_codec_t *codec, const uint8_t *in, int in_len,
                        uint64_t *values);

/* ================================================================
 * State Management
 * ================================================================ */

/*
 * Take a snapshot for gap recovery.
 * @return  Heap-allocated snapshot. Free with residc_codec_destroy().
 */
residc_codec_t *residc_codec_snapshot(const residc_codec_t *codec);

/*
 * Restore codec state from a snapshot.
 */
void residc_codec_restore(residc_codec_t *codec, const residc_codec_t *snap);

/*
 * Reset codec to initial state.
 */
void residc_codec_reset(residc_codec_t *codec);

/* ================================================================
 * MFU Pre-Seeding
 * ================================================================ */

/*
 * Pre-seed instrument frequency table for better early compression.
 * Must be called identically on encoder and decoder.
 *
 * @param ids     Instrument IDs sorted by descending frequency
 * @param counts  Frequency counts (parallel to ids)
 * @param n       Number of entries (max 64)
 */
void residc_codec_seed_mfu(residc_codec_t *codec, const uint16_t *ids,
                           const uint16_t *counts, int n);

#ifdef __cplusplus
}
#endif

#endif /* RESIDC_SDK_H */
