/*
 * residc SDK — Implementation wrapping the core C codec.
 *
 * License: MIT OR Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include "residc_sdk.h"
#include "../core/residc.h"

/* Internal codec structure (opaque to users) */
struct residc_codec {
    residc_state_t   state;
    residc_schema_t  schema;
    residc_field_t   fields[RESIDC_MAX_FIELDS];
    int              num_fields;
    int              msg_size;
};

/* Default byte sizes for each field type */
static uint8_t default_size(int type)
{
    switch (type) {
    case RESIDC_TIMESTAMP:     return 8;
    case RESIDC_INSTRUMENT:    return 2;
    case RESIDC_PRICE:         return 4;
    case RESIDC_QUANTITY:       return 4;
    case RESIDC_SEQUENTIAL_ID: return 8;
    case RESIDC_ENUM:          return 1;
    case RESIDC_BOOL:          return 1;
    case RESIDC_CATEGORICAL:   return 4;
    case RESIDC_RAW:           return 4;
    case RESIDC_DELTA_ID:      return 8;
    case RESIDC_DELTA_PRICE:   return 4;
    case RESIDC_COMPUTED:      return 0;
    default:                   return 0;
    }
}

residc_codec_t *residc_codec_create(const int *types, const int8_t *ref_fields,
                                    int num_fields)
{
    if (num_fields <= 0 || num_fields > RESIDC_MAX_FIELDS)
        return NULL;

    residc_codec_t *c = (residc_codec_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->num_fields = num_fields;
    int offset = 0;

    for (int i = 0; i < num_fields; i++) {
        c->fields[i].type = (residc_field_type_t)types[i];
        c->fields[i].size = default_size(types[i]);
        c->fields[i].offset = (uint16_t)offset;
        c->fields[i].ref_field = ref_fields ? ref_fields[i] : -1;
        offset += c->fields[i].size;
    }

    c->msg_size = offset;
    if (c->msg_size > 256) {
        free(c);
        return NULL;
    }
    c->schema.fields = c->fields;
    c->schema.num_fields = num_fields;
    c->schema.msg_size = c->msg_size;

    residc_init(&c->state, &c->schema);
    return c;
}

void residc_codec_destroy(residc_codec_t *codec)
{
    free(codec);
}

/* Pack uint64 values into a raw message buffer matching the schema layout */
static void pack_values(const residc_codec_t *c, const uint64_t *values,
                        uint8_t *msg)
{
    for (int i = 0; i < c->num_fields; i++) {
        const residc_field_t *f = &c->fields[i];
        uint64_t val = values[i];
        uint8_t *p = msg + f->offset;
        switch (f->size) {
        case 1: *p = (uint8_t)val; break;
        case 2: { uint16_t v = (uint16_t)val; memcpy(p, &v, 2); break; }
        case 4: { uint32_t v = (uint32_t)val; memcpy(p, &v, 4); break; }
        case 8: memcpy(p, &val, 8); break;
        }
    }
}

/* Unpack raw message buffer back to uint64 values */
static void unpack_values(const residc_codec_t *c, const uint8_t *msg,
                          uint64_t *values)
{
    for (int i = 0; i < c->num_fields; i++) {
        const residc_field_t *f = &c->fields[i];
        const uint8_t *p = msg + f->offset;
        switch (f->size) {
        case 1: values[i] = *p; break;
        case 2: { uint16_t v; memcpy(&v, p, 2); values[i] = v; break; }
        case 4: { uint32_t v; memcpy(&v, p, 4); values[i] = v; break; }
        case 8: { uint64_t v; memcpy(&v, p, 8); values[i] = v; break; }
        default: values[i] = 0; break;
        }
    }
}

int residc_codec_encode(residc_codec_t *codec, const uint64_t *values,
                        uint8_t *out, int capacity)
{
    if (!codec || !values || !out) return -1;
    uint8_t msg[256];
    memset(msg, 0, (size_t)codec->msg_size);
    pack_values(codec, values, msg);
    return residc_encode(&codec->state, msg, out, capacity);
}

int residc_codec_decode(residc_codec_t *codec, const uint8_t *in, int in_len,
                        uint64_t *values)
{
    if (!codec || !in || !values) return -1;
    uint8_t msg[256];
    memset(msg, 0, (size_t)codec->msg_size);
    int consumed = residc_decode(&codec->state, in, in_len, msg);
    if (consumed < 0) return consumed;
    unpack_values(codec, msg, values);
    return consumed;
}

residc_codec_t *residc_codec_snapshot(const residc_codec_t *codec)
{
    if (!codec) return NULL;
    residc_codec_t *snap = (residc_codec_t *)malloc(sizeof(*snap));
    if (!snap) return NULL;
    memcpy(snap, codec, sizeof(*snap));
    /* Fix internal pointer: schema.fields must point to snap's own copy */
    snap->schema.fields = snap->fields;
    snap->state.schema = &snap->schema;
    return snap;
}

void residc_codec_restore(residc_codec_t *codec, const residc_codec_t *snap)
{
    if (!codec || !snap) return;
    /* Copy state from snapshot, preserve our own schema setup */
    residc_state_t *s = &codec->state;
    const residc_schema_t *saved_schema = s->schema;
    memcpy(s, &snap->state, sizeof(*s));
    s->schema = saved_schema;
}

void residc_codec_reset(residc_codec_t *codec)
{
    if (!codec) return;
    residc_reset(&codec->state);
}

void residc_codec_seed_mfu(residc_codec_t *codec, const uint16_t *ids,
                           const uint16_t *counts, int n)
{
    if (!codec || !ids || !counts) return;
    residc_mfu_seed(&codec->state.mfu, ids, counts, n);
}
