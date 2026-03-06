/*
 * fuzz_roundtrip.c — libFuzzer target for residc roundtrip correctness.
 *
 * Interprets fuzzer data as field values, encodes, decodes, and
 * asserts bit-perfect roundtrip.
 *
 * Build:
 *   clang -g -fsanitize=fuzzer,address -Icore -o fuzz/fuzz_roundtrip \
 *     fuzz/fuzz_roundtrip.c core/residc.c
 *
 * Run:
 *   ./fuzz/fuzz_roundtrip -max_len=256 -runs=1000000
 */

#include "residc.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

typedef struct {
    uint64_t timestamp;
    uint16_t instrument_id;
    uint32_t price;
    uint32_t quantity;
    uint8_t  side;
} FuzzQuote;

static const residc_field_t fuzz_fields[] = {
    { RESIDC_TIMESTAMP,  offsetof(FuzzQuote, timestamp),     8, -1 },
    { RESIDC_INSTRUMENT, offsetof(FuzzQuote, instrument_id), 2, -1 },
    { RESIDC_PRICE,      offsetof(FuzzQuote, price),         4, -1 },
    { RESIDC_QUANTITY,   offsetof(FuzzQuote, quantity),       4, -1 },
    { RESIDC_ENUM,       offsetof(FuzzQuote, side),           1, -1 },
};

static const residc_schema_t fuzz_schema = {
    .fields     = fuzz_fields,
    .num_fields = 5,
    .msg_size   = sizeof(FuzzQuote),
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < sizeof(FuzzQuote)) return 0;

    static residc_state_t *enc = NULL, *dec = NULL;
    if (!enc) {
        enc = calloc(1, sizeof(residc_state_t));
        dec = calloc(1, sizeof(residc_state_t));
        if (!enc || !dec) return 0;
        residc_init(enc, &fuzz_schema);
        residc_init(dec, &fuzz_schema);
    }

    FuzzQuote q;
    memcpy(&q, data, sizeof(q));

    /* Clamp instrument_id to valid range */
    q.instrument_id %= RESIDC_MAX_INSTRUMENTS;

    uint8_t buf[128];
    int clen = residc_encode(enc, &q, buf, sizeof(buf));
    if (clen < 0) {
        /* Reset on error to keep enc/dec in sync */
        residc_reset(enc);
        residc_reset(dec);
        return 0;
    }

    FuzzQuote decoded;
    int dlen = residc_decode(dec, buf, clen, &decoded);
    if (dlen < 0) {
        residc_reset(enc);
        residc_reset(dec);
        return 0;
    }

    /* Bit-perfect roundtrip required */
    assert(q.timestamp == decoded.timestamp);
    assert(q.instrument_id == decoded.instrument_id);
    assert(q.price == decoded.price);
    assert(q.quantity == decoded.quantity);
    assert(q.side == decoded.side);

    return 0;
}
