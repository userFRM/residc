/*
 * fuzz_decode.c — libFuzzer target for residc decode robustness.
 *
 * Feeds arbitrary bytes to residc_decode to verify no crashes,
 * no undefined behavior, and no buffer overflows.
 *
 * Build:
 *   clang -g -fsanitize=fuzzer,address -Icore -o fuzz/fuzz_decode \
 *     fuzz/fuzz_decode.c core/residc.c
 *
 * Run:
 *   ./fuzz/fuzz_decode -max_len=256 -runs=1000000
 */

#include "residc.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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
    static residc_state_t *state = NULL;
    if (!state) {
        state = calloc(1, sizeof(residc_state_t));
        if (!state) return 0;
        residc_init(state, &fuzz_schema);
    }

    FuzzQuote decoded;
    memset(&decoded, 0, sizeof(decoded));

    /* Don't check return — we're testing for crashes, not correctness */
    residc_decode(state, data, (int)size, &decoded);

    return 0;
}
