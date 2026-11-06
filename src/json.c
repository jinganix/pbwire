/* Protobuf JSON mapping: printing and parsing.
 *
 * The printer emits the canonical mapping the reference implementation
 * produces: field names in camelCase (`[full.name]` for extensions),
 * 64-bit integers quoted, bytes as standard base64, enums by name, floats in
 * the shortest round-tripping form, well-known types in their JSON form
 * (`{"@type": ...}` expansions, RFC 3339 timestamps, `"3.2s"` durations,
 * `Struct` as a nested object, `FieldMask` as one camelCase string, wrappers
 * as bare values). Unknown fields are not printed. The parser accepts what
 * the reference parser accepts in its default (legacy) mode: proto and
 * camelCase names, quoted or bare 64-bit integers, floats with integral or
 * exponential text, quoted bools, single-quoted and bare key strings,
 * trailing commas, `null` as unset, single elements for repeated fields,
 * nested-array flattening (outside `Value`/`ListValue`), the object form of
 * well-known types (`{"seconds": ...}`), base64 with either alphabet and
 * optional padding, unknown closed-enum numbers diverted to unknown fields
 * (like the binary decoder), and duplicate keys allowed except inside oneofs
 * and maps. Anything else unknown fails unless told to ignore it.
 *
 * Like the other formats, nesting is capped at PBW_MAX_DEPTH, string fields
 * reject invalid UTF-8, and a missing proto2 `required` field fails the parse
 * with PBW_ERR_REQUIRED.
 */

#include "internal.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------- json names */

/* protoc's ToJsonName: drop every '_', uppercase the letter after it. */
static size_t
j_json_name(const char *name, char *out, size_t cap)
{
    size_t n = 0;
    int upper = 0;

    for (; *name != '\0'; name++) {
        char c;

        if (*name == '_') {
            upper = 1;
            continue;
        }
        c = upper ? (char)toupper((unsigned char)*name) : *name;
        upper = 0;
        if (n + 1 < cap) {
            out[n] = c;
        }
        n++;
    }
    if (cap > 0) {
        out[n < cap ? n : cap - 1] = '\0';
    }
    return n;
}

/* protoc's ToCamelCase with a lowercased first letter. */
static size_t
j_camel_name(const char *name, char *out, size_t cap)
{
    size_t n = 0;
    int upper = 1;

    for (; *name != '\0'; name++) {
        char c;

        if (*name == '_') {
            upper = 1;
            continue;
        }
        c = upper ? (char)toupper((unsigned char)*name) : *name;
        upper = 0;
        if (n == 0) {
            c = (char)tolower((unsigned char)c);
        }
        if (n + 1 < cap) {
            out[n] = c;
        }
        n++;
    }
    if (cap > 0) {
        out[n < cap ? n : cap - 1] = '\0';
    }
    return n;
}

/* Field names print as their jsonName: protoc's ToCamelCase with a
 * lowercased first letter is j_camel_name() below, used for parsing;
 * printing keeps the mapping j_json_name() computes. */
static void
j_print_name(const pbw_field *field, char *out, size_t cap)
{
    j_json_name(field->name, out, cap);
}

/* The reference parser matches camelCase names, then proto names, then json
 * names, in that order. */
static int
j_field_by_name(const pbw_descriptor *desc, const char *key, size_t len,
                const pbw_field **out)
{
    char tmp[256];
    size_t i;

    for (i = 0; i < desc->field_count; i++) {
        const pbw_field *f = &desc->fields[i];
        size_t n = strlen(f->name);

        if (n != len) {
            continue;
        }
        j_camel_name(f->name, tmp, sizeof(tmp));
        if (strlen(tmp) == len && memcmp(tmp, key, len) == 0) {
            *out = f;
            return 1;
        }
    }
    for (i = 0; i < desc->field_count; i++) {
        const pbw_field *f = &desc->fields[i];

        if (strlen(f->name) == len && memcmp(f->name, key, len) == 0) {
            *out = f;
            return 1;
        }
    }
    for (i = 0; i < desc->field_count; i++) {
        const pbw_field *f = &desc->fields[i];

        j_json_name(f->name, tmp, sizeof(tmp));
        if (strlen(tmp) == len && memcmp(tmp, key, len) == 0) {
            *out = f;
            return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------- well-known */

/* The JSON mapping special-cases these by full message name. */
typedef enum {
    J_NOT_WKT = 0,
    J_ANY,
    J_TIMESTAMP,
    J_DURATION,
    J_STRUCT,
    J_VALUE,
    J_LIST,
    J_FIELDMASK,
    J_WRAPPER,
    J_EMPTY,
    J_NULLVALUE /* not a message, but routed through here for nulls */
} j_wkt;

static j_wkt
j_classify(const pbw_descriptor *desc)
{
    static const struct {
        const char *name;
        j_wkt kind;
    } table[] = {
        {"google.protobuf.Any", J_ANY},
        {"google.protobuf.Timestamp", J_TIMESTAMP},
        {"google.protobuf.Duration", J_DURATION},
        {"google.protobuf.Struct", J_STRUCT},
        {"google.protobuf.Value", J_VALUE},
        {"google.protobuf.ListValue", J_LIST},
        {"google.protobuf.FieldMask", J_FIELDMASK},
        {"google.protobuf.DoubleValue", J_WRAPPER},
        {"google.protobuf.FloatValue", J_WRAPPER},
        {"google.protobuf.Int64Value", J_WRAPPER},
        {"google.protobuf.UInt64Value", J_WRAPPER},
        {"google.protobuf.Int32Value", J_WRAPPER},
        {"google.protobuf.UInt32Value", J_WRAPPER},
        {"google.protobuf.BoolValue", J_WRAPPER},
        {"google.protobuf.StringValue", J_WRAPPER},
        {"google.protobuf.BytesValue", J_WRAPPER},
        {"google.protobuf.Empty", J_EMPTY},
        {"google.protobuf.NullValue", J_NULLVALUE},
    };
    size_t i;

    if (desc == NULL) {
        return J_NOT_WKT;
    }
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(desc->name, table[i].name) == 0) {
            return table[i].kind;
        }
    }
    return J_NOT_WKT;
}

/* ---------------------------------------------------------------- output */

typedef struct {
    pbw_buf buf;
    pbw_status status;
    int dot; /* the host locale's decimal point is '.' (checked once per call) */
} j_out;

static void
j_put(j_out *j, const void *data, size_t len)
{
    if (j->status != PBW_OK || len == 0) {
        return;
    }
    if (!pbw_buf_reserve(&j->buf, len)) {
        j->status = PBW_ERR_NOMEM;
        return;
    }
    memcpy(j->buf.data + j->buf.len, data, len);
    j->buf.len += len;
}

static void
j_str(j_out *j, const char *s)
{
    j_put(j, s, strlen(s));
}

static void
j_ch(j_out *j, char c)
{
    j_put(j, &c, 1);
}

static void
j_u64(j_out *j, uint64_t value)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)value);

    if (n > 0) {
        j_put(j, tmp, (size_t)n);
    }
}

static void
j_i64(j_out *j, int64_t value)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)value);

    if (n > 0) {
        j_put(j, tmp, (size_t)n);
    }
}

/* Shortest decimal that still parses back to the same bits. */
static void
j_double(j_out *j, double value, int is_float)
{
    int prec;
    int prec_max = 17;
    int prec_min = is_float ? 9 : 15;

    for (prec = prec_min; prec <= prec_max; prec++) {
        char tmp[32];
        int n = pbw_format_g(tmp, sizeof(tmp), prec, value, j->dot);
        double back;

        if (n <= 0 || (size_t)n >= sizeof(tmp)) {
            continue;
        }
        back = pbw_strtod(tmp, NULL, j->dot);
        if ((is_float ? (double)(float)back == value : back == value)) {
            j_put(j, tmp, (size_t)n);
            return;
        }
    }
    j_str(j, "0");
}

/* Writes the string quoted, with the reference printer's escapes. Invalid
 * UTF-8 fails: the reference printer reports it instead of emitting bytes. */
static void
j_quoted(j_out *j, const uint8_t *data, size_t len)
{
    static const char *hex = "0123456789abcdef";
    const uint8_t *p;
    const uint8_t *end;

    if (j == NULL || j->status != PBW_OK) {
        return;
    }
    if (data == NULL) {
        if (len > 0) {
            j->status = PBW_ERR_INVALID;
            return;
        }
        data = pbw_empty_input;
    }
    p = data;
    end = data + len;

    j_ch(j, '"');
    while (p < end) {
        uint8_t c = *p;

        switch (c) {
        case '"':
            j_str(j, "\\\"");
            p++;
            continue;
        case '\\':
            j_str(j, "\\\\");
            p++;
            continue;
        case '\b':
            j_str(j, "\\b");
            p++;
            continue;
        case '\f':
            j_str(j, "\\f");
            p++;
            continue;
        case '\n':
            j_str(j, "\\n");
            p++;
            continue;
        case '\r':
            j_str(j, "\\r");
            p++;
            continue;
        case '\t':
            j_str(j, "\\t");
            p++;
            continue;
        default:
            break;
        }
        if (c == '<') {
            j_str(j, "\\u003c");
            p++;
            continue;
        }
        if (c == '>') {
            j_str(j, "\\u003e");
            p++;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            char esc[6];

            esc[0] = '\\';
            esc[1] = 'u';
            esc[2] = '0';
            esc[3] = '0';
            esc[4] = hex[c >> 4];
            esc[5] = hex[c & 15];
            j_put(j, esc, 6);
            p++;
            continue;
        }
        if (c < 0x80) {
            j_ch(j, (char)c);
            p++;
            continue;
        }
        /* Pass UTF-8 through after validating the whole scalar. */
        {
            size_t left = pbw_remaining((const uint8_t *)end, (const uint8_t *)p);
            uint32_t rune = 0;
            size_t width = 0;

            if ((c & 0xe0) == 0xc0) {
                rune = c & 0x1f;
                width = 2;
            } else if ((c & 0xf0) == 0xe0) {
                rune = c & 0x0f;
                width = 3;
            } else if ((c & 0xf8) == 0xf0) {
                rune = c & 0x07;
                width = 4;
            } else {
                j->status = PBW_ERR_INVALID;
                return;
            }
            if (width > left) {
                j->status = PBW_ERR_INVALID;
                return;
            }
            {
                size_t k;

                for (k = 1; k < width; k++) {
                    if ((p[k] & 0xc0) != 0x80) {
                        j->status = PBW_ERR_INVALID;
                        return;
                    }
                    rune = (rune << 6) | (p[k] & 0x3f);
                }
            }
            if ((width == 2 && rune < 0x80) ||
                (width == 3 && rune < 0x800) ||
                (width == 4 && rune < 0x10000) ||
                rune > 0x10ffff || (rune >= 0xd800 && rune <= 0xdfff)) {
                j->status = PBW_ERR_INVALID;
                return;
            }
            j_put(j, p, width);
            p += width;
        }
    }
    j_ch(j, '"');
}

static void
j_base64(j_out *j, const uint8_t *data, size_t len)
{
    static const char *alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *p;
    const uint8_t *end;

    if (j == NULL || j->status != PBW_OK) {
        return;
    }
    if (data == NULL) {
        if (len > 0) {
            j->status = PBW_ERR_INVALID;
            return;
        }
        data = pbw_empty_input;
    }
    p = data;
    end = data + len;

    j_ch(j, '"');
    while (pbw_remaining(end, p) >= 3) {
        char out[4];

        out[0] = alphabet[p[0] >> 2];
        out[1] = alphabet[((p[0] & 3) << 4) | (p[1] >> 4)];
        out[2] = alphabet[((p[1] & 0xf) << 2) | (p[2] >> 6)];
        out[3] = alphabet[p[2] & 0x3f];
        j_put(j, out, 4);
        p += 3;
    }
    if (pbw_remaining((const uint8_t *)end, (const uint8_t *)p) == 2) {
        char out[4];

        out[0] = alphabet[p[0] >> 2];
        out[1] = alphabet[((p[0] & 3) << 4) | (p[1] >> 4)];
        out[2] = alphabet[(p[1] & 0xf) << 2];
        out[3] = '=';
        j_put(j, out, 4);
    } else if (pbw_remaining((const uint8_t *)end, (const uint8_t *)p) == 1) {
        char out[4];

        out[0] = alphabet[p[0] >> 2];
        out[1] = alphabet[(p[0] & 3) << 4];
        out[2] = '=';
        out[3] = '=';
        j_put(j, out, 4);
    }
    j_ch(j, '"');
}

/* Whether a singular scalar prints: proto3 drops defaults, proto2 prints
 * anything present, oneof members print when theirs is the set case. */
static int
j_should_print(const pbw_message *msg, size_t index)
{
    const pbw_field *field = &msg->desc->fields[index];
    const pbw_value *value = &msg->values[index];

    if (!pbw_msg_present(msg, index)) {
        return 0;
    }
    if (field->label == PBW_LABEL_REPEATED) {
        return value->kind == PBW_VALUE_LIST && value->as.list.len > 0;
    }
    if (field->type == PBW_TYPE_MESSAGE || field->type == PBW_TYPE_GROUP) {
        return 1;
    }
    if (field->oneof_index != 0) {
        return msg->oneof_case[field->oneof_index] == field->number;
    }
    if (msg->desc->proto3) {
        if (value->kind != PBW_VALUE_UINT) {
            return 1;
        }
        return value->as.u64 != 0;
    }
    return 1;
}

/* An empty google.protobuf.Value prints as nothing: the reference printer
 * silently discards it (and errors only at the top level, which never
 * happens here since the top message is a real type). */
static int
j_value_is_empty(const pbw_message *msg)
{
    size_t i;

    for (i = 0; i < msg->desc->field_count; i++) {
        if (pbw_msg_present(msg, i)) {
            return 0;
        }
    }
    return 1;
}

static void j_print_message(j_out *j, const pbw_message *msg);

static void
j_print_scalar(j_out *j, const pbw_field *field, const pbw_value *value)
{
    uint64_t bits = value->kind == PBW_VALUE_UINT ? value->as.u64 : 0;

    switch (field->type) {
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FLOAT: {
        double v;

        if (field->type == PBW_TYPE_FLOAT) {
            /* Floats are stored as their 4 raw bytes (like the wire). */
            float f;

            memcpy(&f, &bits, sizeof(f));
            v = (double)f;
        } else {
            memcpy(&v, &bits, sizeof(v));
        }
        if (isnan(v)) {
            j_str(j, "\"NaN\"");
        } else if (isinf(v)) {
            j_str(j, v < 0 ? "\"-Infinity\"" : "\"Infinity\"");
        } else {
            j_double(j, v, field->type == PBW_TYPE_FLOAT);
        }
        break;
    }
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
        j_ch(j, '"');
        j_i64(j, (int64_t)bits);
        j_ch(j, '"');
        break;
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64:
        j_ch(j, '"');
        j_u64(j, bits);
        j_ch(j, '"');
        break;
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
        j_i64(j, (int64_t)(int32_t)bits);
        break;
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32:
        j_u64(j, (uint64_t)(uint32_t)bits);
        break;
    case PBW_TYPE_BOOL:
        j_str(j, bits != 0 ? "true" : "false");
        break;
    case PBW_TYPE_STRING: {
        size_t len = 0;
        const uint8_t *data = NULL;

        if (value->kind == PBW_VALUE_BYTES) {
            data = value->as.bytes.data;
            len = value->as.bytes.len;
        }
        j_quoted(j, data == NULL ? (const uint8_t *)"" : data, len);
        break;
    }
    case PBW_TYPE_BYTES: {
        size_t len = 0;
        const uint8_t *data = NULL;

        if (value->kind == PBW_VALUE_BYTES) {
            data = value->as.bytes.data;
            len = value->as.bytes.len;
        }
        j_base64(j, data == NULL ? (const uint8_t *)"" : data, len);
        break;
    }
    case PBW_TYPE_ENUM: {
        const char *name = NULL;

        if (field->sub != NULL &&
            pbw_enum_find_name(field->sub, (int32_t)bits, &name)) {
            j_quoted(j, (const uint8_t *)name, strlen(name));
        } else {
            /* Open enums keep unknown numbers; they print as numbers. */
            j_i64(j, (int64_t)(int32_t)bits);
        }
        break;
    }
    case PBW_TYPE_MESSAGE:
    case PBW_TYPE_GROUP:
        if (value->kind == PBW_VALUE_MESSAGE && value->as.msg != NULL) {
            j_print_message(j, value->as.msg);
        } else {
            j_str(j, "{}");
        }
        break;
    }
}

/* Map keys print quoted: integers decimal, bools as "true"/"false". */
static void
j_print_map_key(j_out *j, const pbw_field *key_field, const pbw_value *key)
{
    uint64_t bits = key->kind == PBW_VALUE_UINT ? key->as.u64 : 0;

    j_ch(j, '"');
    switch (key_field->type) {
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
        j_i64(j, (int64_t)bits);
        break;
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32:
        j_u64(j, bits);
        break;
    case PBW_TYPE_BOOL:
        j_str(j, bits != 0 ? "true" : "false");
        break;
    case PBW_TYPE_STRING: {
        /* j_quoted adds its own quotes; emit the payload raw here. */
        size_t len = 0;
        const uint8_t *data = NULL;

        if (key->kind == PBW_VALUE_BYTES) {
            data = key->as.bytes.data;
            len = key->as.bytes.len;
        }
        /* Reuse the escaper without double quotes: temporarily write into
         * the buffer through j_quoted then strip the quotes. */
        {
            size_t before = j->buf.len;

            j_quoted(j, data == NULL ? (const uint8_t *)"" : data, len);
            if (j->status == PBW_OK && j->buf.len >= before + 2) {
                memmove(j->buf.data + before, j->buf.data + before + 1,
                        j->buf.len - before - 2);
                j->buf.len -= 2;
            }
        }
        break;
    }
    case PBW_TYPE_ENUM: {
        const char *name = NULL;

        if (key_field->sub != NULL &&
            pbw_enum_find_name(key_field->sub, (int32_t)bits, &name)) {
            size_t before = j->buf.len;

            j_quoted(j, (const uint8_t *)name, strlen(name));
            if (j->status == PBW_OK && j->buf.len >= before + 2) {
                memmove(j->buf.data + before, j->buf.data + before + 1,
                        j->buf.len - before - 2);
                j->buf.len -= 2;
            }
        } else {
            j_i64(j, (int64_t)(int32_t)bits);
        }
        break;
    }
    default:
        j->status = PBW_ERR_INVALID;
        break;
    }
    j_ch(j, '"');
}

/* Flat map key for duplicate elimination: missing keys normalize to
 * (UINT, 0), exactly like the `&zero` default in the print loop below. */
typedef struct {
    uint32_t kind;
    uint64_t u64;
    const uint8_t *data;
    size_t len;
    int has_key; /* 0: entry omitted the key; never counts as a later dup */
} j_flatkey;

static int
j_flatkey_cmp(const j_flatkey *a, const j_flatkey *b)
{
    if (a->kind != b->kind) {
        return a->kind < b->kind ? -1 : 1;
    }
    if (a->kind == PBW_VALUE_BYTES) {
        size_t n = a->len < b->len ? a->len : b->len;
        int r = n > 0 ? memcmp(a->data, b->data, n) : 0;

        if (r != 0) {
            return r < 0 ? -1 : 1;
        }
        if (a->len != b->len) {
            return a->len < b->len ? -1 : 1;
        }
        return 0;
    }
    if (a->u64 != b->u64) {
        return a->u64 < b->u64 ? -1 : 1;
    }
    return 0;
}

/* Stable merge sort of indices by flat key (recursion is log2(n), like the
 * deterministic map sort on the encode side). Stability keeps wire order
 * inside an equal run, so the run's last countable member is the winner. */
static void
j_idx_sort(const j_flatkey *keys, size_t *idx, size_t *tmp, size_t n)
{
    size_t mid;
    size_t i;
    size_t j;
    size_t k;

    if (n < 2) {
        return;
    }
    mid = n / 2;
    j_idx_sort(keys, idx, tmp, mid);
    j_idx_sort(keys, idx + mid, tmp + mid, n - mid);
    i = 0;
    j = mid;
    k = 0;
    while (i < mid && j < n) {
        if (j_flatkey_cmp(&keys[idx[j]], &keys[idx[i]]) < 0) {
            tmp[k++] = idx[j++];
        } else {
            tmp[k++] = idx[i++];
        }
    }
    while (i < mid) {
        tmp[k++] = idx[i++];
    }
    while (j < n) {
        tmp[k++] = idx[j++];
    }
    memcpy(idx, tmp, n * sizeof(*idx));
}

/* Marks entries shadowed by a later equal key in caller-owned, zeroed
 * skip[]: 1 = keep the legacy loop (small maps and malformed input),
 * -1 = out of memory, 0 = skip[] filled. */
static int
j_map_winners(const pbw_value *list, uint8_t *skip)
{
    size_t n = list->as.list.len;
    j_flatkey *keys;
    size_t *idx;
    size_t *tmp;
    uint8_t *block;
    size_t i;

    if (n <= 32 || n > SIZE_MAX / (sizeof(*keys) + 2 * sizeof(*idx))) {
        return 1;
    }
    block =
        (uint8_t *)malloc(n * (sizeof(*keys) + 2 * sizeof(*idx)));
    if (block == NULL) {
        return -1;
    }
    keys = (j_flatkey *)(void *)block;
    idx = (size_t *)(void *)(keys + n);
    tmp = idx + n;
    for (i = 0; i < n; i++) {
        const pbw_value *item = list->as.list.items[i];
        const pbw_value *key;

        /* Malformed entries keep the exact legacy path (which reports them). */
        if (item == NULL || item->kind != PBW_VALUE_MESSAGE ||
            item->as.msg == NULL) {
            free(block);
            return 1;
        }
        key = pbw_message_get(item->as.msg, 1);
        keys[i].has_key = key != NULL;
        if (key == NULL) {
            static const pbw_value zero = {PBW_VALUE_UINT, 0, {0}};

            key = &zero;
        }
        keys[i].kind = (uint32_t)key->kind;
        if (key->kind == PBW_VALUE_BYTES) {
            keys[i].data = key->as.bytes.data;
            keys[i].len = key->as.bytes.len;
            keys[i].u64 = 0;
        } else {
            keys[i].data = NULL;
            keys[i].len = 0;
            keys[i].u64 = key->as.u64;
        }
        idx[i] = i;
    }
    j_idx_sort(keys, idx, tmp, n);
    /* One pass over equal runs: everything before the run's last countable
     * member is shadowed (a missing key never counts as the later dup). */
    i = 0;
    while (i < n) {
        size_t e = i + 1;
        size_t last = i;
        size_t m;

        while (e < n && j_flatkey_cmp(&keys[idx[e]], &keys[idx[i]]) == 0) {
            e++;
        }
        for (m = i; m < e; m++) {
            if (keys[idx[m]].has_key) {
                last = m;
            }
        }
        if (keys[idx[last]].has_key) {
            for (m = i; m < last; m++) {
                skip[idx[m]] = 1;
            }
        }
        i = e;
    }
    free(block);
    return 0;
}

static void
j_print_map(j_out *j, const pbw_field *field, const pbw_value *list)
{
    const pbw_descriptor *entry = field->sub;
    const pbw_field *key_field = NULL;
    const pbw_field *value_field = NULL;
    size_t i;

    if (entry != NULL) {
        key_field = pbw_descriptor_find_field(entry, 1);
        value_field = pbw_descriptor_find_field(entry, 2);
    }
    if (key_field == NULL || value_field == NULL) {
        j->status = PBW_ERR_INVALID;
        return;
    }
    /* Binary input may repeat a key (last wins); JSON objects cannot.
     * Past a few dozen entries the per-item later-scan turns quadratic,
     * so large maps precompute the shadowed entries once by sorting. */
    uint8_t *skip = NULL;
    if (list->as.list.len > 32) {
        int winners;

        skip = (uint8_t *)calloc(list->as.list.len, 1);
        if (skip == NULL) {
            j->status = PBW_ERR_NOMEM;
            return;
        }
        winners = j_map_winners(list, skip);
        if (winners < 0) {
            free(skip);
            j->status = PBW_ERR_NOMEM;
            return;
        }
        if (winners > 0) {
            /* Small or malformed input: the loop below scans as before. */
            free(skip);
            skip = NULL;
        }
    }
    int first = 1;

    j_ch(j, '{');
    for (i = 0; i < list->as.list.len; i++) {
        const pbw_value *item = list->as.list.items[i];
        const pbw_message *m;
        const pbw_value *key;
        const pbw_value *val;
        size_t k;
        int last = 1;

        if (item == NULL || item->kind != PBW_VALUE_MESSAGE ||
            item->as.msg == NULL) {
            j->status = PBW_ERR_INVALID;
            free(skip);
            return;
        }
        /* Binary input may omit the key or the value; both default. */
        m = item->as.msg;
        key = pbw_message_get(m, 1);
        val = pbw_message_get(m, 2);
        if (key == NULL) {
            static const pbw_value zero = {PBW_VALUE_UINT, 0, {0}};

            key = &zero;
        }
        if (val == NULL) {
            static const pbw_value zero = {PBW_VALUE_UINT, 0, {0}};
            static const pbw_value empty = {PBW_VALUE_BYTES, 0, {0}};

            if (value_field->type != PBW_TYPE_MESSAGE &&
                value_field->type != PBW_TYPE_GROUP) {
                if (value_field->type == PBW_TYPE_STRING ||
                    value_field->type == PBW_TYPE_BYTES) {
                    val = &empty;
                } else {
                    val = &zero;
                }
            }
            /* message values left NULL print as {} below */
        }
        if (skip != NULL) {
            last = !skip[i];
        } else {
            for (k = i + 1; k < list->as.list.len; k++) {
                const pbw_value *later = list->as.list.items[k];
                const pbw_value *later_key;

                if (later == NULL || later->kind != PBW_VALUE_MESSAGE ||
                    later->as.msg == NULL) {
                    continue;
                }
                later_key = pbw_message_get(later->as.msg, 1);
                if (later_key == NULL || later_key->kind != key->kind) {
                    continue;
                }
                if (key->kind == PBW_VALUE_BYTES) {
                    if (later_key->as.bytes.len == key->as.bytes.len &&
                        memcmp(later_key->as.bytes.data, key->as.bytes.data,
                               key->as.bytes.len) == 0) {
                        last = 0;
                        break;
                    }
                } else if (later_key->as.u64 == key->as.u64) {
                    last = 0;
                    break;
                }
            }
        }
        if (!last) {
            continue;
        }
        if (val != NULL && value_field->type == PBW_TYPE_MESSAGE &&
            val->kind == PBW_VALUE_MESSAGE && val->as.msg != NULL &&
            j_classify(val->as.msg->desc) == J_VALUE &&
            j_value_is_empty(val->as.msg)) {
            continue;
        }
        if (!first) {
            j_ch(j, ',');
        }
        first = 0;
        j_print_map_key(j, key_field, key);
        j_ch(j, ':');
        if (val == NULL) {
            j_str(j, "{}");
        } else {
            j_print_scalar(j, value_field, val);
        }
    }
    free(skip);
    j_ch(j, '}');
}

static void
j_print_repeated(j_out *j, const pbw_field *field, const pbw_value *list)
{
    size_t i;
    int first = 1;

    if (field->type == PBW_TYPE_MESSAGE && field->sub != NULL &&
        field->sub->map_entry) {
        j_print_map(j, field, list);
        return;
    }
    j_ch(j, '[');
    for (i = 0; i < list->as.list.len; i++) {
        const pbw_value *item = list->as.list.items[i];

        if (item == NULL) {
            j->status = PBW_ERR_INVALID;
            return;
        }
        if (field->type == PBW_TYPE_MESSAGE && field->sub != NULL) {
            if (item->kind == PBW_VALUE_MESSAGE && item->as.msg != NULL &&
                j_classify(item->as.msg->desc) == J_VALUE &&
                j_value_is_empty(item->as.msg)) {
                continue;
            }
        }
        if (!first) {
            j_ch(j, ',');
        }
        first = 0;
        j_print_scalar(j, field, item);
    }
    j_ch(j, ']');
}

/* "1970-01-01T00:00:00Z" with 0/3/6/9 fractional digits. */
static void
j_print_timestamp(j_out *j, const pbw_message *msg)
{
    const pbw_value *secs_v = pbw_message_get(msg, 1);
    const pbw_value *nanos_v = pbw_message_get(msg, 2);
    int64_t secs = secs_v != NULL && secs_v->kind == PBW_VALUE_UINT
                       ? (int64_t)secs_v->as.u64
                       : 0;
    int32_t nanos = nanos_v != NULL && nanos_v->kind == PBW_VALUE_UINT
                        ? (int32_t)nanos_v->as.u64
                        : 0;
    /* Julian Day -> Y/M/D, the same algorithm as the reference printer. */
    int32_t l;
    int32_t n;
    int32_t k;
    int32_t jj;
    int32_t i;
    int32_t year;
    int32_t month;
    int32_t day;
    int32_t hour;
    int32_t minute;
    int32_t second;
    uint32_t frac = 0;
    int digits = 0;
    char tmp[40];
    int len;

    if (secs < -62135596800ll || secs > 253402300799ll || nanos < 0 ||
        nanos > 999999999) {
        j->status = PBW_ERR_INVALID;
        return;
    }
    secs += 62135596800ll;
    l = (int32_t)(secs / 86400) - 719162 + 68569 + 2440588;
    n = 4 * l / 146097;
    l = l - (146097 * n + 3) / 4;
    i = 4000 * (l + 1) / 1461001;
    l = l - 1461 * i / 4 + 31;
    jj = 80 * l / 2447;
    k = l - 2447 * jj / 80;
    l = jj / 11;
    jj = jj + 2 - 12 * l;
    i = 100 * (n - 49) + i + l;
    year = i;
    month = jj;
    day = k;
    hour = (int32_t)((secs / 3600) % 24);
    minute = (int32_t)((secs / 60) % 60);
    second = (int32_t)(secs % 60);
    if (nanos != 0) {
        digits = 9;
        frac = (uint32_t)nanos;
        while (frac % 1000 == 0) {
            frac /= 1000;
            digits -= 3;
        }
    }
    if (digits == 0) {
        len = snprintf(tmp, sizeof(tmp),
                       "\"%04d-%02d-%02dT%02d:%02d:%02dZ\"", year, month, day,
                       hour, minute, second);
    } else {
        len = snprintf(tmp, sizeof(tmp),
                       "\"%04d-%02d-%02dT%02d:%02d:%02d.%.*uZ\"", year, month,
                       day, hour, minute, second, digits, frac);
    }
    if (len <= 0 || (size_t)len >= sizeof(tmp)) {
        j->status = PBW_ERR_INVALID;
        return;
    }
    j_put(j, tmp, (size_t)len);
}

static void
j_print_duration(j_out *j, const pbw_message *msg)
{
    const pbw_value *secs_v = pbw_message_get(msg, 1);
    const pbw_value *nanos_v = pbw_message_get(msg, 2);
    int64_t secs = secs_v != NULL && secs_v->kind == PBW_VALUE_UINT
                       ? (int64_t)secs_v->as.u64
                       : 0;
    int32_t nanos = nanos_v != NULL && nanos_v->kind == PBW_VALUE_UINT
                        ? (int32_t)nanos_v->as.u64
                        : 0;
    const int64_t max = (int64_t)3652500 * 86400;
    char tmp[48];
    int len;

    if (secs > max || secs < -max || nanos > 999999999 || nanos < -999999999 ||
        ((secs != 0) && (nanos != 0) && ((secs < 0) != (nanos < 0)))) {
        j->status = PBW_ERR_INVALID;
        return;
    }
    if (nanos == 0) {
        len = snprintf(tmp, sizeof(tmp), "\"%lld%s\"", (long long)secs, "s");
    } else {
        uint32_t frac = (uint32_t)(nanos < 0 ? -nanos : nanos);
        int digits = 9;
        const char *sign = (secs < 0 || nanos < 0) ? "-" : "";
        int64_t a = secs < 0 ? -secs : secs;

        while (frac % 1000 == 0) {
            frac /= 1000;
            digits -= 3;
        }
        len = snprintf(tmp, sizeof(tmp), "\"%s%lld.%.*us\"", sign,
                       (long long)a, digits, frac);
    }
    if (len <= 0 || (size_t)len >= sizeof(tmp)) {
        j->status = PBW_ERR_INVALID;
        return;
    }
    j_put(j, tmp, (size_t)len);
}

/* Paths print camelCase (`foo_bar` -> `fooBar`), joined with commas. */
static void
j_print_fieldmask(j_out *j, const pbw_message *msg)
{
    const pbw_value *paths = pbw_message_get(msg, 1);
    size_t i;

    j_ch(j, '"');
    if (paths != NULL && paths->kind == PBW_VALUE_LIST) {
        for (i = 0; i < paths->as.list.len; i++) {
            const pbw_value *item = paths->as.list.items[i];
            const uint8_t *p;
            size_t len;
            size_t k;
            int saw_under = 0;

            if (item == NULL || item->kind != PBW_VALUE_BYTES) {
                j->status = PBW_ERR_INVALID;
                return;
            }
            if (i > 0) {
                j_ch(j, ',');
            }
            p = item->as.bytes.data;
            len = item->as.bytes.len;
            for (k = 0; k < len; k++) {
                unsigned char c = p[k];

                if (c >= 'a' && c <= 'z' && saw_under) {
                    j_ch(j, (char)(c - 'a' + 'A'));
                } else if ((c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '.') {
                    j_ch(j, (char)c);
                } else if (c == '_') {
                    /* legacy mode is always on: keep swallowing */
                    saw_under = 1;
                    continue;
                } else {
                    if (saw_under) {
                        j_ch(j, '_');
                    }
                    j_ch(j, (char)c);
                }
                saw_under = 0;
            }
        }
    }
    j_ch(j, '"');
}

static void
j_print_struct_value(j_out *j, const pbw_message *msg);
static void
j_print_list_value(j_out *j, const pbw_message *msg);

/* google.protobuf.Value: exactly one of its six fields is set. An empty
 * Value never reaches here (the caller skips it); NaN/Inf fail. */
static void
j_print_value(j_out *j, const pbw_message *msg)
{
    const pbw_value *v;

    v = pbw_message_get(msg, 1);
    if (v != NULL) {
        j_str(j, "null");
        return;
    }
    v = pbw_message_get(msg, 2);
    if (v != NULL && v->kind == PBW_VALUE_UINT) {
        double d;

        memcpy(&d, &v->as.u64, sizeof(d));
        if (isnan(d) || isinf(d)) {
            j->status = PBW_ERR_INVALID;
            return;
        }
        j_double(j, d, 0);
        return;
    }
    v = pbw_message_get(msg, 3);
    if (v != NULL && v->kind == PBW_VALUE_BYTES) {
        j_quoted(j, v->as.bytes.data, v->as.bytes.len);
        return;
    }
    v = pbw_message_get(msg, 4);
    if (v != NULL) {
        j_str(j, v->kind == PBW_VALUE_UINT && v->as.u64 != 0 ? "true"
                                                             : "false");
        return;
    }
    v = pbw_message_get(msg, 5);
    if (v != NULL && v->kind == PBW_VALUE_MESSAGE && v->as.msg != NULL) {
        j_print_struct_value(j, v->as.msg);
        return;
    }
    v = pbw_message_get(msg, 6);
    if (v != NULL && v->kind == PBW_VALUE_MESSAGE && v->as.msg != NULL) {
        j_print_list_value(j, v->as.msg);
        return;
    }
    j->status = PBW_ERR_INVALID;
}

static void
j_print_struct_value(j_out *j, const pbw_message *msg)
{
    const pbw_value *fields;

    if (j == NULL || j->status != PBW_OK || msg == NULL ||
        msg->desc == NULL) {
        if (j != NULL && j->status == PBW_OK) {
            j->status = PBW_ERR_INVALID;
        }
        return;
    }
    fields = pbw_message_get(msg, 1);
    size_t i;

    j_ch(j, '{');
    if (fields != NULL && fields->kind == PBW_VALUE_LIST) {
        int first = 1;

        for (i = 0; i < fields->as.list.len; i++) {
            const pbw_value *item = fields->as.list.items[i];
            const pbw_message *m;
            const pbw_value *key;
            const pbw_value *val;

            if (item == NULL || item->kind != PBW_VALUE_MESSAGE ||
                item->as.msg == NULL) {
                j->status = PBW_ERR_INVALID;
                return;
            }
            m = item->as.msg;
            key = pbw_message_get(m, 1);
            val = pbw_message_get(m, 2);
            if (key == NULL || key->kind != PBW_VALUE_BYTES ||
                val == NULL) {
                j->status = PBW_ERR_INVALID;
                return;
            }
            if (val->kind == PBW_VALUE_MESSAGE && val->as.msg != NULL &&
                j_classify(val->as.msg->desc) == J_VALUE &&
                j_value_is_empty(val->as.msg)) {
                continue;
            }
            if (!first) {
                j_ch(j, ',');
            }
            first = 0;
            /* Struct keys are arbitrary strings, not field names. */
            j_quoted(j, key->as.bytes.data, key->as.bytes.len);
            j_ch(j, ':');
            if (val->kind == PBW_VALUE_MESSAGE && val->as.msg != NULL) {
                j_print_value(j, val->as.msg);
            } else {
                j->status = PBW_ERR_INVALID;
                return;
            }
        }
    }
    j_ch(j, '}');
}

static void
j_print_list_value(j_out *j, const pbw_message *msg)
{
    const pbw_value *values;

    if (j == NULL || j->status != PBW_OK || msg == NULL ||
        msg->desc == NULL) {
        if (j != NULL && j->status == PBW_OK) {
            j->status = PBW_ERR_INVALID;
        }
        return;
    }
    values = pbw_message_get(msg, 1);

    j_ch(j, '[');
    if (values != NULL && values->kind == PBW_VALUE_LIST) {
        size_t i;
        int first = 1;

        for (i = 0; i < values->as.list.len; i++) {
            const pbw_value *item = values->as.list.items[i];

            if (item == NULL || item->kind != PBW_VALUE_MESSAGE ||
                item->as.msg == NULL) {
                j->status = PBW_ERR_INVALID;
                return;
            }
            if (j_classify(item->as.msg->desc) == J_VALUE &&
                j_value_is_empty(item->as.msg)) {
                continue;
            }
            if (!first) {
                j_ch(j, ',');
            }
            first = 0;
            j_print_value(j, item->as.msg);
        }
    }
    j_ch(j, ']');
}

/* google.protobuf.Any: {"@type": url} plus the expanded payload, or just
 * the type for well-known payloads under a "value" key. */
static void
j_print_any(j_out *j, const pbw_message *msg)
{
    const pbw_value *url_v = pbw_message_get(msg, 1);
    const pbw_value *bytes_v = pbw_message_get(msg, 2);
    const uint8_t *url = NULL;
    size_t url_len = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    const char *slash;
    const pbw_descriptor *resolved;
    pbw_message *decoded = NULL;

    if (url_v == NULL) {
        if (bytes_v != NULL) {
            j->status = PBW_ERR_INVALID;
            return;
        }
        j_str(j, "{}");
        return;
    }
    if (url_v->kind == PBW_VALUE_BYTES) {
        url = url_v->as.bytes.data;
        url_len = url_v->as.bytes.len;
    }
    if (bytes_v != NULL && bytes_v->kind == PBW_VALUE_BYTES) {
        payload = bytes_v->as.bytes.data;
        payload_len = bytes_v->as.bytes.len;
    }
    /* The type name is whatever follows the last '/'. */
    slash = NULL;
    {
        size_t i;

        for (i = 0; i < url_len; i++) {
            if (url[i] == '/') {
                slash = (const char *)url + i;
            }
        }
    }
    {
        char name[256];
        size_t off = slash == NULL ? 0 : (size_t)(slash - (const char *)url) + 1;
        size_t n = url_len - off;

        if (n >= sizeof(name)) {
            j->status = PBW_ERR_INVALID;
            return;
        }
        memcpy(name, url + off, n);
        name[n] = '\0';
        resolved = msg->desc->schema == NULL
                       ? NULL
                       : pbw_schema_find(msg->desc->schema, name);
    }
    if (resolved == NULL || resolved->is_enum) {
        j->status = PBW_ERR_INVALID;
        return;
    }
    if (pbw_decode(resolved, payload, payload_len, &decoded) != PBW_OK) {
        j->status = PBW_ERR_NOMEM;
        return;
    }
    j_str(j, "{\"@type\":");
    j_quoted(j, url, url_len);
    /* Empty is not special inside Any: it expands inline (to nothing). */
    {
        j_wkt wkt = j_classify(resolved);

        if (wkt != J_NOT_WKT && wkt != J_EMPTY) {
            j_str(j, ",\"value\":");
            j_print_message(j, decoded);
        } else if (!j_value_is_empty(decoded)) {
            /* Regular payloads expand inline: {"a":1} becomes ,"a":1. */
            size_t mark = j->buf.len;

            j_print_message(j, decoded);
            if (j->status == PBW_OK && j->buf.len >= mark + 2 &&
                j->buf.data[mark] == '{' &&
                j->buf.data[j->buf.len - 1] == '}') {
                j->buf.data[mark] = ',';
                j->buf.len--;
            }
        }
    }
    pbw_message_free(decoded);
    j_ch(j, '}');
}

/* One field of an object body, with the separating comma. */
static void
j_print_one(j_out *j, const pbw_message *msg, const pbw_field *field,
            int *first)
{
    char key[288];
    const pbw_value *value = &msg->values[(size_t)(field - msg->desc->fields)];

    if (!*first) {
        j_ch(j, ',');
    }
    *first = 0;
    if (field->is_extension) {
        const char *full =
            field->full_name != NULL ? field->full_name : field->name;

        if (strlen(full) + 3 > sizeof(key)) {
            j->status = PBW_ERR_INVALID;
            return;
        }
        key[0] = '[';
        memcpy(key + 1, full, strlen(full));
        key[strlen(full) + 1] = ']';
        key[strlen(full) + 2] = '\0';
        j_quoted(j, (const uint8_t *)key, strlen(key));
    } else {
        j_print_name(field, key, sizeof(key));
        j_quoted(j, (const uint8_t *)key, strlen(key));
    }
    j_ch(j, ':');
    if (field->label == PBW_LABEL_REPEATED) {
        if (value->kind == PBW_VALUE_LIST) {
            j_print_repeated(j, field, value);
        } else {
            j->status = PBW_ERR_INVALID;
        }
    } else {
        j_print_scalar(j, field, value);
    }
}

static void
j_print_fields(j_out *j, const pbw_message *msg)
{
    /* Fields print in field-number order, extensions interleaved. */
    const pbw_field **order = NULL;
    size_t count = 0;
    size_t i;
    size_t k;
    int first = 1;

    if (msg->desc->schema != NULL && msg->desc->schema->linked) {
        /* link_message() already sorted the fields by number: print them in
         * place, with no per-message order array and no malloc. A printed tree
         * has one message per element, so this is the common case. */
        for (i = 0; i < msg->desc->field_count; i++) {
            if (!j_should_print(msg, i)) {
                continue;
            }
            j_print_one(j, msg, &msg->desc->fields[i], &first);
            if (j->status != PBW_OK) {
                return;
            }
        }
        return;
    }
    for (i = 0; i < msg->desc->field_count; i++) {
        if (j_should_print(msg, i)) {
            count++;
        }
    }
    if (count > 0) {
        order = (const pbw_field **)malloc(count * sizeof(*order));
        if (order == NULL) {
            j->status = PBW_ERR_NOMEM;
            return;
        }
    }
    count = 0;
    for (i = 0; i < msg->desc->field_count; i++) {
        if (!j_should_print(msg, i)) {
            continue;
        }
        order[count] = &msg->desc->fields[i];
        /* insertion sort by number; a hand-built schema is not sorted yet */
        k = count;
        while (k > 0 && order[k - 1]->number > order[k]->number) {
            const pbw_field *tmp = order[k - 1];

            order[k - 1] = order[k];
            order[k] = tmp;
            k--;
        }
        count++;
    }
    for (i = 0; i < count; i++) {
        j_print_one(j, msg, order[i], &first);
        if (j->status != PBW_OK) {
            break;
        }
    }
    free(order);
}

static void
j_print_message(j_out *j, const pbw_message *msg)
{
    if (j == NULL || j->status != PBW_OK || msg == NULL ||
        msg->desc == NULL) {
        if (j != NULL && j->status == PBW_OK) {
            j->status = PBW_ERR_INVALID;
        }
        return;
    }
    if (msg->desc->field_count > 0 &&
        (msg->values == NULL || msg->present == NULL)) {
        j->status = PBW_ERR_INVALID;
        return;
    }
    switch (j_classify(msg->desc)) {
    case J_ANY:
        j_print_any(j, msg);
        return;
    case J_TIMESTAMP:
        j_print_timestamp(j, msg);
        return;
    case J_DURATION:
        j_print_duration(j, msg);
        return;
    case J_FIELDMASK:
        j_print_fieldmask(j, msg);
        return;
    case J_STRUCT:
        j_print_struct_value(j, msg);
        return;
    case J_LIST:
        j_print_list_value(j, msg);
        return;
    case J_VALUE:
        j_print_value(j, msg);
        return;
    case J_WRAPPER: {
        const pbw_field *inner = pbw_descriptor_find_field(msg->desc, 1);
        const pbw_value *v = pbw_message_get(msg, 1);

        if (inner == NULL) {
            j->status = PBW_ERR_INVALID;
            return;
        }
        if (v == NULL) {
            /* A null wrapper element prints its default. */
            static const pbw_value zero = {PBW_VALUE_UINT, 0, {0}};

            v = &zero;
        }
        j_print_scalar(j, inner, v);
        return;
    }
    case J_EMPTY:
        j_str(j, "{}");
        return;
    default:
        break;
    }
    j_ch(j, '{');
    j_print_fields(j, msg);
    j_ch(j, '}');
}

pbw_status
pbw_json_print(const pbw_message *msg, char **out, size_t *len)
{
    j_out j;

    if (msg == NULL || out == NULL || len == NULL) {
        return PBW_ERR_INVALID;
    }
    pbw_buf_init(&j.buf);
    j.status = PBW_OK;
    j.dot = pbw_locale_dot();
    if (!pbw_buf_reserve(&j.buf, 64)) {
        return PBW_ERR_NOMEM;
    }
    j.buf.len = 0;
    j_print_message(&j, msg);
    if (j.status != PBW_OK) {
        pbw_buf_free(&j.buf);
        return j.status;
    }
    if (!pbw_buf_reserve(&j.buf, 1)) {
        pbw_buf_free(&j.buf);
        return PBW_ERR_NOMEM;
    }
    j.buf.data[j.buf.len] = '\0';
    *out = (char *)j.buf.data;
    (void)0;
    *len = j.buf.len;
    return PBW_OK;
}

/* --------------------------------------------------------------- parser */

typedef struct {
    const char *p;
    const char *end;
} j_cur;

typedef struct {
    j_cur cur;
    int ignore_unknown;
    int depth;
    int dot; /* the host locale's decimal point is '.' (checked once per call) */
} j_in;

typedef enum {
    JK_END,
    JK_OBJ,
    JK_ARR,
    JK_STR,
    JK_NUM,
    JK_TRUE,
    JK_FALSE,
    JK_NULL,
    JK_BAD
} j_kind;

static void
j_skip(j_cur *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' ||
                             *c->p == '\r')) {
        c->p++;
    }
}

static j_kind
j_peek_kind(j_in *j)
{
    j_skip(&j->cur);
    if (j->cur.p >= j->cur.end) {
        return JK_END;
    }
    switch (*j->cur.p) {
    case '{':
        return JK_OBJ;
    case '[':
        return JK_ARR;
    case '"':
    case '\'':
        return JK_STR;
    case 't':
        return JK_TRUE;
    case 'f':
        return JK_FALSE;
    case 'n':
        return JK_NULL;
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        return JK_NUM;
    default:
        return JK_BAD;
    }
}

static pbw_status
j_expect_lit(j_in *j, const char *lit)
{
    j_skip(&j->cur);
    for (; *lit != '\0'; lit++) {
        if (j->cur.p >= j->cur.end || *j->cur.p != *lit) {
            return PBW_ERR_INVALID;
        }
        j->cur.p++;
    }
    return PBW_OK;
}

static pbw_status
j_expect_ch(j_in *j, char c)
{
    j_skip(&j->cur);
    if (j->cur.p >= j->cur.end || *j->cur.p != c) {
        return PBW_ERR_INVALID;
    }
    j->cur.p++;
    return PBW_OK;
}

/* Structural UTF-8 check, shared by the string parser. */
static int
j_valid_utf8(const uint8_t *data, size_t len)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;

    while (p < end) {
        uint8_t c = *p;
        size_t width = 0;
        uint32_t rune = 0;
        size_t k;

        if (c < 0x80) {
            p++;
            continue;
        }
        if ((c & 0xe0) == 0xc0) {
            rune = c & 0x1f;
            width = 2;
        } else if ((c & 0xf0) == 0xe0) {
            rune = c & 0x0f;
            width = 3;
        } else if ((c & 0xf8) == 0xf0) {
            rune = c & 0x07;
            width = 4;
        } else {
            return 0;
        }
        if (width > pbw_remaining((const uint8_t *)end, (const uint8_t *)p)) {
            return 0;
        }
        for (k = 1; k < width; k++) {
            if ((p[k] & 0xc0) != 0x80) {
                return 0;
            }
            rune = (rune << 6) | (p[k] & 0x3f);
        }
        if ((width == 2 && rune < 0x80) ||
            (width == 3 && rune < 0x800) ||
            (width == 4 && rune < 0x10000) || rune > 0x10ffff ||
            (rune >= 0xd800 && rune <= 0xdfff)) {
            return 0;
        }
        p += width;
    }
    return 1;
}

/* Parses a JSON string (single quotes accepted, legacy mode), unescaping
 * into `out`. */
static pbw_status
j_parse_string(j_in *j, pbw_buf *out)
{
    char quote;
    uint8_t seq[4];

    j_skip(&j->cur);
    if (j->cur.p >= j->cur.end ||
        (*j->cur.p != '"' && *j->cur.p != '\'')) {
        return PBW_ERR_INVALID;
    }
    quote = *j->cur.p++;
    for (;;) {
        unsigned char c;

        if (j->cur.p >= j->cur.end) {
            return PBW_ERR_INVALID;
        }
        c = (unsigned char)*j->cur.p++;
        if (c == (unsigned char)quote) {
            if (!j_valid_utf8(out->data, out->len)) {
                return PBW_ERR_INVALID;
            }
            return PBW_OK;
        }
        if (c == '\\') {
            unsigned char e;
            uint32_t rune;
            int width;
            int k;

            if (j->cur.p >= j->cur.end) {
                return PBW_ERR_INVALID;
            }
            e = (unsigned char)*j->cur.p++;
            switch (e) {
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case '\'':
                c = '\'';
                break;
            case 'u':
            case 'U': {
                uint32_t hi;
                int i;

                /* `\\U` reads four hex digits just like `\\u` (legacy). */
                hi = 0;
                for (i = 0; i < 4; i++) {
                    unsigned char h;

                    if (j->cur.p >= j->cur.end) {
                        return PBW_ERR_INVALID;
                    }
                    h = (unsigned char)*j->cur.p++;
                    hi <<= 4;
                    if (h >= '0' && h <= '9') {
                        hi |= (uint32_t)(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        hi |= (uint32_t)(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        hi |= (uint32_t)(h - 'A' + 10);
                    } else {
                        return PBW_ERR_INVALID;
                    }
                }
                rune = hi;
                if (rune >= 0xd800 && rune <= 0xdbff) {
                    uint32_t lo = 0;

                    if (pbw_remaining((const uint8_t *)j->cur.end, (const uint8_t *)j->cur.p) < 2 ||
                        j->cur.p[0] != '\\' || j->cur.p[1] != 'u') {
                        return PBW_ERR_INVALID;
                    }
                    j->cur.p += 2;
                    for (i = 0; i < 4; i++) {
                        unsigned char h;

                        if (j->cur.p >= j->cur.end) {
                            return PBW_ERR_INVALID;
                        }
                        h = (unsigned char)*j->cur.p++;
                        lo <<= 4;
                        if (h >= '0' && h <= '9') {
                            lo |= (uint32_t)(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            lo |= (uint32_t)(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            lo |= (uint32_t)(h - 'A' + 10);
                        } else {
                            return PBW_ERR_INVALID;
                        }
                    }
                    if (lo < 0xdc00 || lo > 0xdfff) {
                        return PBW_ERR_INVALID;
                    }
                    rune = ((hi & 0x3ff) << 10) | (lo & 0x3ff);
                    rune += 0x10000;
                } else if (rune >= 0xdc00 && rune <= 0xdfff) {
                    return PBW_ERR_INVALID;
                }
                if (rune > 0x10ffff) {
                    return PBW_ERR_INVALID;
                }
                if (rune <= 0x7f) {
                    seq[0] = (uint8_t)rune;
                    width = 1;
                } else if (rune <= 0x7ff) {
                    seq[0] = (uint8_t)(((rune >> 6) & 0x1f) | 0xc0);
                    seq[1] = (uint8_t)((rune & 0x3f) | 0x80);
                    width = 2;
                } else if (rune <= 0xffff) {
                    seq[0] = (uint8_t)(((rune >> 12) & 0x0f) | 0xe0);
                    seq[1] = (uint8_t)(((rune >> 6) & 0x3f) | 0x80);
                    seq[2] = (uint8_t)((rune & 0x3f) | 0x80);
                    width = 3;
                } else {
                    seq[0] = (uint8_t)(((rune >> 18) & 0x07) | 0xf0);
                    seq[1] = (uint8_t)(((rune >> 12) & 0x3f) | 0x80);
                    seq[2] = (uint8_t)(((rune >> 6) & 0x3f) | 0x80);
                    seq[3] = (uint8_t)((rune & 0x3f) | 0x80);
                    width = 4;
                }
                if (!pbw_buf_reserve(out, (size_t)width)) {
                    return PBW_ERR_NOMEM;
                }
                for (k = 0; k < width; k++) {
                    out->data[out->len++] = seq[k];
                }
                continue;
            }
            default:
                return PBW_ERR_INVALID;
            }
        }
        if (!pbw_buf_reserve(out, 1)) {
            return PBW_ERR_NOMEM;
        }
        out->data[out->len++] = c;
    }
}

/* Scans a JSON number token without consuming what follows it. */
static pbw_status
j_parse_raw(j_in *j, const char **start, size_t *len)
{
    const char *p;
    const char *end = j->cur.end;
    enum {
        ST_INT,
        ST_FRAC,
        ST_EXP
    } state = ST_INT;
    char prev = 0;

    j_skip(&j->cur);
    p = j->cur.p;
    for (; p < end; p++) {
        char c = *p;
        int is_digit = c >= '0' && c <= '9';
        int last_was_int = prev >= '0' && prev <= '9';

        if (is_digit) {
            /* keep going */
        } else if (state == ST_INT && c == '-') {
            if (last_was_int) {
                break;
            }
        } else if (state == ST_INT && last_was_int && c == '.') {
            state = ST_FRAC;
        } else if (state != ST_EXP && last_was_int &&
                   (c == 'e' || c == 'E')) {
            state = ST_EXP;
        } else if ((prev == 'e' || prev == 'E') && (c == '-' || c == '+')) {
            /* keep going */
        } else {
            break;
        }
        prev = c;
    }
    *start = j->cur.p;
    *len = (size_t)(p - j->cur.p);
    if (*len == 0 || (*len == 1 && **start == '-')) {
        return PBW_ERR_INVALID;
    }
    {
        const char *t = *start;
        size_t n = *len;

        if (*t == '-') {
            t++;
            n--;
        }
        if (n > 1 && t[0] == '0' && t[1] >= '0' && t[1] <= '9') {
            return PBW_ERR_INVALID;
        }
        if ((*start)[*len - 1] == '.') {
            return PBW_ERR_INVALID;
        }
    }
    j->cur.p = p;
    /* A number must end where a value may end. */
    j_skip(&j->cur);
    if (j->cur.p < j->cur.end && *j->cur.p != ',' && *j->cur.p != ']' &&
        *j->cur.p != '}') {
        return PBW_ERR_INVALID;
    }
    return PBW_OK;
}

/* Strict [+-]?digits with 64-bit range checking (for quoted values and
 * number tokens; surrounding whitespace is rejected by the caller). */
static pbw_status
j_atoi64(const char *text, size_t len, int64_t *out)
{
    uint64_t acc = 0;
    int neg = 0;
    size_t i = 0;

    if (len > 0 && (text[0] == '-' || text[0] == '+')) {
        neg = text[0] == '-';
        i = 1;
    }
    if (i >= len) {
        return PBW_ERR_INVALID;
    }
    for (; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        uint64_t limit;

        if (c < '0' || c > '9') {
            return PBW_ERR_INVALID;
        }
        limit = neg ? (uint64_t)INT64_MIN : (uint64_t)INT64_MAX;
        if (acc > (limit - (uint64_t)(c - '0')) / 10) {
            return PBW_ERR_INVALID;
        }
        acc = acc * 10 + (uint64_t)(c - '0');
    }
    if (neg) {
        /* INT64_MIN negates to itself in two's complement. */
        *out = (int64_t)(~(acc - 1));
    } else {
        *out = (int64_t)acc;
    }
    return PBW_OK;
}

static pbw_status
j_atou64(const char *text, size_t len, uint64_t *out)
{
    uint64_t acc = 0;
    size_t i = 0;

    if (len > 0 && text[0] == '+') {
        i = 1;
    }
    if (i >= len) {
        return PBW_ERR_INVALID;
    }
    for (; i < len; i++) {
        unsigned char c = (unsigned char)text[i];

        if (c < '0' || c > '9') {
            return PBW_ERR_INVALID;
        }
        if (acc > (UINT64_MAX - (uint64_t)(c - '0')) / 10) {
            return PBW_ERR_INVALID;
        }
        acc = acc * 10 + (uint64_t)(c - '0');
    }
    *out = acc;
    return PBW_OK;
}

/* Whitespace-tolerant variant for map keys and quoted enum numbers, where
 * the reference uses SimpleAtoi (which skips surrounding whitespace). */
static void
j_trim(const char **text, size_t *len)
{
    while (*len > 0 && (**text == ' ' || **text == '\t' ||
                        **text == '\n' || **text == '\r')) {
        (*text)++;
        (*len)--;
    }
    while (*len > 0 && ((*text)[*len - 1] == ' ' ||
                        (*text)[*len - 1] == '\t' ||
                        (*text)[*len - 1] == '\n' ||
                        (*text)[*len - 1] == '\r')) {
        (*len)--;
    }
}

/* strtod with full-consumption check, like SimpleAtod. */
static pbw_status
j_atod(const char *text, size_t len, int dot, double *out)
{
    char stack[64];
    char *tmp = stack;
    char *stop;
    double v;

    if (text == NULL || out == NULL) {
        return PBW_ERR_INVALID;
    }
    /* strtod skips leading whitespace itself, but not trailing garbage. A
     * number longer than the stack buffer is unusual, not invalid: the
     * reference parser has no length limit either. */
    if (len >= sizeof(stack)) {
        if (len == SIZE_MAX) {
            return PBW_ERR_NOMEM;
        }
        tmp = (char *)malloc(len + 1);
        if (tmp == NULL) {
            return PBW_ERR_NOMEM;
        }
    }
    memcpy(tmp, text, len);
    tmp[len] = '\0';
    if (tmp == NULL) {
        return PBW_ERR_INVALID;
    }
    v = pbw_strtod(tmp, &stop, dot);
    if (stop == tmp) {
        goto invalid;
    }
    while (*stop == ' ' || *stop == '\t' || *stop == '\n' ||
           *stop == '\r') {
        stop++;
    }
    if (*stop != '\0') {
        goto invalid;
    }
    if (tmp != stack) {
        free(tmp);
    }
    *out = v;
    return PBW_OK;
invalid:
    if (tmp != stack) {
        free(tmp);
    }
    return PBW_ERR_INVALID;
}

/* absl::SimpleAtob's word set, case-insensitive. */
static pbw_status
j_atob(const char *text, size_t len, int *out)
{
    static const struct {
        const char *word;
        int value;
    } table[] = {{"true", 1}, {"1", 1}, {"t", 1}, {"y", 1}, {"yes", 1}, {"false", 0}, {"0", 0}, {"f", 0}, {"n", 0}, {"no", 0}};
    char lower[8];
    size_t i;

    if (len >= sizeof(lower)) {
        return PBW_ERR_INVALID;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];

        lower[i] = (char)tolower(c);
    }
    lower[len] = '\0';
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(lower, table[i].word) == 0) {
            *out = table[i].value;
            return PBW_OK;
        }
    }
    return PBW_ERR_INVALID;
}

static pbw_status
j_parse_key(j_in *j, pbw_buf *out)
{
    if (j_peek_kind(j) == JK_STR) {
        if (out == NULL) {
            pbw_buf scratch;
            pbw_status st;

            pbw_buf_init(&scratch);
            st = j_parse_string(j, &scratch);
            pbw_buf_free(&scratch);
            return st;
        }
        return j_parse_string(j, out);
    }
    {
        const char *p = j->cur.p;
        size_t n = 0;

        j_skip(&j->cur);
        p = j->cur.p;
        for (; p < j->cur.end; p++) {
            char c = *p;

            if ((c < '0' || c > '9') && (c < 'a' || c > 'z') &&
                (c < 'A' || c > 'Z') && c != '_') {
                break;
            }
            n++;
        }
        if (n == 0) {
            return PBW_ERR_INVALID;
        }
        if (j->cur.p >= j->cur.end || (j->cur.p[0] >= '0' && j->cur.p[0] <= '9')) {
            return PBW_ERR_INVALID;
        }
        if ((n == 4 && memcmp(j->cur.p, "true", 4) == 0) ||
            (n == 5 &&
             (memcmp(j->cur.p, "false", 5) == 0 ||
              memcmp(j->cur.p, "null", 4) == 0))) {
            return PBW_ERR_INVALID;
        }
        if (out != NULL) {
            if (!pbw_buf_reserve(out, n)) {
                return PBW_ERR_NOMEM;
            }
            memcpy(out->data + out->len, j->cur.p, n);
            out->len += n;
        }
        j->cur.p = p;
        return PBW_OK;
    }
}

/* Parses a key (quoted or bare) into `out`; NULL discards it. */
static pbw_status j_parse_key(j_in *j, pbw_buf *out); /* defined below */
static pbw_status j_parse_named_field(j_in *j, pbw_message *msg,
                                      const uint8_t *key, size_t len);
static pbw_status j_parse_list_of(j_in *j, pbw_message *msg);

/* Skips one JSON value (for unknown fields and Any scans). */
static pbw_status
j_skip_value(j_in *j)
{
    pbw_buf scratch;
    const char *start;
    size_t len;

    switch (j_peek_kind(j)) {
    case JK_OBJ: {
        pbw_status st;

        if (j_expect_ch(j, '{') != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        if (j->depth >= PBW_MAX_DEPTH) {
            return PBW_ERR_LIMIT;
        }
        j->depth++;
        j_skip(&j->cur);
        if (j->cur.p < j->cur.end && *j->cur.p == '}') {
            j->cur.p++;
            j->depth--;
            return PBW_OK;
        }
        for (;;) {
            /* Bare keys are accepted in legacy mode. */
            st = j_parse_key(j, NULL);
            if (st != PBW_OK) {
                j->depth--;
                return st;
            }
            if (j_expect_ch(j, ':') != PBW_OK) {
                j->depth--;
                return PBW_ERR_INVALID;
            }
            st = j_skip_value(j);
            if (st != PBW_OK) {
                j->depth--;
                return st == PBW_ERR_LIMIT ? st : PBW_ERR_INVALID;
            }
            j_skip(&j->cur);
            if (j->cur.p < j->cur.end && *j->cur.p == ',') {
                j->cur.p++;
                continue;
            }
            if (j_expect_ch(j, '}') != PBW_OK) {
                j->depth--;
                return PBW_ERR_INVALID;
            }
            j->depth--;
            return PBW_OK;
        }
    }
    case JK_ARR: {
        pbw_status st = PBW_OK;

        if (j_expect_ch(j, '[') != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        if (j->depth >= PBW_MAX_DEPTH) {
            return PBW_ERR_LIMIT;
        }
        j->depth++;
        j_skip(&j->cur);
        if (j->cur.p < j->cur.end && *j->cur.p == ']') {
            j->cur.p++;
            j->depth--;
            return PBW_OK;
        }
        for (;;) {
            st = j_skip_value(j);
            if (st != PBW_OK) {
                j->depth--;
                return st;
            }
            j_skip(&j->cur);
            if (j->cur.p < j->cur.end && *j->cur.p == ',') {
                j->cur.p++;
                continue;
            }
            if (j_expect_ch(j, ']') != PBW_OK) {
                j->depth--;
                return PBW_ERR_INVALID;
            }
            j->depth--;
            return PBW_OK;
        }
    }
    case JK_STR: {
        pbw_status st;

        pbw_buf_init(&scratch);
        st = j_parse_string(j, &scratch);
        pbw_buf_free(&scratch);
        return st;
    }
    case JK_NUM:
        return j_parse_raw(j, &start, &len);
    case JK_TRUE:
        return j_expect_lit(j, "true");
    case JK_FALSE:
        return j_expect_lit(j, "false");
    case JK_NULL:
        return j_expect_lit(j, "null");
    default:
        return PBW_ERR_INVALID;
    }
}

/* -------------------------------------------------- value storage */

static pbw_status j_parse_message(j_in *j, pbw_message *msg);
static pbw_status j_parse_object_body(j_in *j, pbw_message *msg);
static pbw_status j_parse_field_value(j_in *j, pbw_message *msg,
                                      const pbw_field *field, int in_array);

/* Stores a uint; repeated fields append. */
static pbw_status
j_store_uint(j_in *j, pbw_message *msg, const pbw_field *field, uint64_t bits)
{
    pbw_value *item;

    (void)j;
    if (field->label == PBW_LABEL_REPEATED) {
        item = pbw_value_new_uint_a(msg->arena, bits);
        if (item == NULL) {
            return PBW_ERR_NOMEM;
        }
        {
            pbw_value *list = pbw_message_list_for(msg, field);

            if (list == NULL ||
                !pbw_value_list_push(list, item, msg->arena)) {
                pbw_value_free(item);
                return PBW_ERR_NOMEM;
            }
        }
        return PBW_OK;
    }
    {
        pbw_value value;

        value.kind = PBW_VALUE_UINT;
        value.flags = msg->arena != NULL ? PBW_VF_ARENA : 0;
        value.as.u64 = bits;
        pbw_message_put(msg, (size_t)(field - msg->desc->fields), &value);
        return PBW_OK;
    }
}

/* Stores bytes; repeated fields append. */
static pbw_status
j_store_bytes(j_in *j, pbw_message *msg, const pbw_field *field,
              const uint8_t *data, size_t len)
{
    (void)j;
    if (field->label == PBW_LABEL_REPEATED) {
        pbw_value *item = pbw_value_new_bytes_a(msg->arena, data, len);
        pbw_value *list;

        if (item == NULL) {
            return PBW_ERR_NOMEM;
        }
        list = pbw_message_list_for(msg, field);
        if (list == NULL || !pbw_value_list_push(list, item, msg->arena)) {
            pbw_value_free(item);
            return PBW_ERR_NOMEM;
        }
        return PBW_OK;
    }
    {
        pbw_value value;

        if (!pbw_value_init_bytes(&value, msg->arena, data, len)) {
            return PBW_ERR_NOMEM;
        }
        pbw_message_put(msg, (size_t)(field - msg->desc->fields), &value);
        return PBW_OK;
    }
}

/* Stores a submessage; repeated fields append, singular fields replace. */
static pbw_status
j_store_message(j_in *j, pbw_message *msg, const pbw_field *field,
                pbw_message *sub)
{
    (void)j;
    if (field->label == PBW_LABEL_REPEATED) {
        pbw_value *item = pbw_value_new_message(sub);
        pbw_value *list;

        if (item == NULL) {
            pbw_message_free(sub);
            return PBW_ERR_NOMEM;
        }
        list = pbw_message_list_for(msg, field);
        if (list == NULL || !pbw_value_list_push(list, item, msg->arena)) {
            pbw_value_free(item);
            return PBW_ERR_NOMEM;
        }
        return PBW_OK;
    }
    {
        pbw_value value;

        value.kind = PBW_VALUE_MESSAGE;
        value.flags = msg->arena != NULL ? PBW_VF_ARENA : 0;
        value.as.msg = sub;
        pbw_message_put(msg, (size_t)(field - msg->desc->fields), &value);
        return PBW_OK;
    }
}

/* An unknown number for a closed enum moves to the unknown fields, exactly
 * like the binary decoder does. */
static pbw_status
j_store_unknown_enum(j_in *j, pbw_message *msg, const pbw_field *field,
                     int32_t number)
{
    uint8_t tmp[16];
    size_t n;

    (void)j;
    /* int32 negatives encode sign-extended, like on the wire */
    n = (size_t)pbw_varint_write(tmp, ((uint64_t)field->number << 3));
    n += (size_t)pbw_varint_write(tmp + n, (uint64_t)(int64_t)number);
    if ((pbw_status)pbw_message_add_unknown(msg, tmp, n) != PBW_OK) {
        return PBW_ERR_NOMEM;
    }
    return PBW_OK;
}

/* -------------------------------------------------- scalar values */

/* Integer rules shared by tokens and quoted strings: the token is strict,
 * quoted strings reject surrounding whitespace (map keys and quoted enum
 * numbers allow it, like SimpleAtoi). */
static pbw_status
j_parse_int_value(j_in *j, const char *text, size_t len, int quoted,
                  int is_signed, int bits32, uint64_t *out)
{
    int64_t s = 0;
    uint64_t u = 0;

    (void)j;
    if (quoted) {
        size_t i;

        for (i = 0; i < len; i++) {
            if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                text[i] == '\r') {
                return PBW_ERR_INVALID;
            }
        }
    }
    if (is_signed) {
        if (j_atoi64(text, len, &s) != PBW_OK) {
            double d;

            /* 1e3 and 1.0 spell integers; 1.5 does not. */
            if (j_atod(text, len, j->dot, &d) != PBW_OK || !isfinite(d) ||
                d < -9007199254740992.0 || d > 9007199254740992.0) {
                return PBW_ERR_INVALID;
            }
            s = (int64_t)d;
            if ((double)s != d) {
                return PBW_ERR_INVALID;
            }
        }
        if (bits32 && (s < INT32_MIN || s > INT32_MAX)) {
            return PBW_ERR_INVALID;
        }
        *out = (uint64_t)s;
        return PBW_OK;
    }
    if (j_atou64(text, len, &u) != PBW_OK) {
        double d;

        if (j_atod(text, len, j->dot, &d) != PBW_OK || !isfinite(d) || d < 0 ||
            d > 18014398509481984.0) {
            return PBW_ERR_INVALID;
        }
        /* The bounds above keep the conversion defined. */
        u = (uint64_t)d;
        if ((double)u != d) {
            return PBW_ERR_INVALID;
        }
    }
    if (bits32 && u > UINT32_MAX) {
        return PBW_ERR_INVALID;
    }
    *out = u;
    return PBW_OK;
}

static pbw_status
j_parse_double_value(j_in *j, const char *text, size_t len, int is_float,
                     double *out)
{
    static const struct {
        const char *word;
        double v;
    } specials[] = {{"NaN", NAN},
                    {"Infinity", INFINITY},
                    {"-Infinity", -INFINITY}};
    double d;
    size_t i;

    (void)j;
    for (i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
        if (strlen(specials[i].word) == len &&
            memcmp(text, specials[i].word, len) == 0) {
            *out = specials[i].v;
            return PBW_OK;
        }
    }
    /* Quoted floats allow surrounding whitespace (SimpleAtod does). */
    if (j_atod(text, len, j->dot, &d) != PBW_OK) {
        return PBW_ERR_INVALID;
    }
    if (is_float && isfinite(d) && !isfinite((float)d)) {
        return PBW_ERR_INVALID;
    }
    *out = d;
    return PBW_OK;
}

static int
j_base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+' || c == '-') {
        return 62;
    }
    if (c == '/' || c == '_') {
        return 63;
    }
    return -1;
}

/* Base64 with either alphabet and optional padding, like the reference. */
static pbw_status
j_parse_base64(const char *text, size_t len, pbw_buf *out)
{
    size_t n;
    size_t i = 0;
    size_t need;

    if (out == NULL) {
        return PBW_ERR_INVALID;
    }
    if (text == NULL && len > 0) {
        return PBW_ERR_INVALID;
    }
    if (text == NULL) {
        text = (const char *)pbw_empty_input;
    }
    n = len;

    /* Padding, if any, is 1-2 '=' at the very end. */
    while (n > 0 && text[n - 1] == '=') {
        n--;
    }
    if (len - n > 2) {
        return PBW_ERR_INVALID;
    }
    if (n == 0) {
        /* Empty is fine; padding alone is not. */
        return len == 0 ? PBW_OK : PBW_ERR_INVALID;
    }
    if (len == n ? (n % 4 == 1) : (len % 4 != 0)) {
        /* Unpadded tails of 2-3 chars are fine, but padded input must
         * fill complete quanta, like the reference. */
        return PBW_ERR_INVALID;
    }
    /* (n/4)*3+3 cannot wrap: n <= SIZE_MAX, so (n/4)*3 <= n on both
     * 32- and 64-bit targets. */
    need = (n / 4) * 3 + 3;
    if (need < (n / 4) * 3) {
        return PBW_ERR_NOMEM;
    }
    if (!pbw_buf_reserve(out, need)) {
        return PBW_ERR_NOMEM;
    }
    while (i < (n & ~(size_t)3)) {
        int a = j_base64_value(text[i]);
        int b = j_base64_value(text[i + 1]);
        int c = j_base64_value(text[i + 2]);
        int d = j_base64_value(text[i + 3]);

        if (a < 0 || b < 0 || c < 0 || d < 0) {
            return PBW_ERR_INVALID;
        }
        out->data[out->len++] = (uint8_t)((a << 2) | (b >> 4));
        out->data[out->len++] = (uint8_t)(((b & 15) << 4) | (c >> 2));
        out->data[out->len++] = (uint8_t)(((c & 3) << 6) | d);
        i += 4;
    }
    if (n - i == 2) {
        int a = j_base64_value(text[i]);
        int b = j_base64_value(text[i + 1]);

        if (a < 0 || b < 0) {
            return PBW_ERR_INVALID;
        }
        out->data[out->len++] = (uint8_t)((a << 2) | (b >> 4));
    } else if (n - i == 3) {
        int a = j_base64_value(text[i]);
        int b = j_base64_value(text[i + 1]);
        int c = j_base64_value(text[i + 2]);

        if (a < 0 || b < 0 || c < 0) {
            return PBW_ERR_INVALID;
        }
        out->data[out->len++] = (uint8_t)((a << 2) | (b >> 4));
        out->data[out->len++] = (uint8_t)(((b & 15) << 4) | (c >> 2));
    }
    return PBW_OK;
}

/* Parses one scalar (non-message) value of `field`. */
static pbw_status
j_parse_scalar_value(j_in *j, pbw_message *msg, const pbw_field *field)
{
    j_kind kind = j_peek_kind(j);
    uint64_t bits = 0;

    switch (field->type) {
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FLOAT: {
        double v = 0;
        pbw_status st;

        if (kind == JK_NUM) {
            const char *text;
            size_t len;

            if (j_parse_raw(j, &text, &len) != PBW_OK) {
                return PBW_ERR_INVALID;
            }
            st = j_parse_double_value(
                j, text, len, field->type == PBW_TYPE_FLOAT, &v);
            /* Raw numbers must be finite (the lexer bars inf/nan spellings,
             * but not overflow like 1e400). */
            if (st == PBW_OK && !isfinite(v)) {
                st = PBW_ERR_INVALID;
            }
        } else if (kind == JK_STR) {
            pbw_buf raw;

            pbw_buf_init(&raw);
            if (j_parse_string(j, &raw) != PBW_OK) {
                pbw_buf_free(&raw);
                return PBW_ERR_INVALID;
            }
            st = j_parse_double_value(j, (const char *)raw.data, raw.len,
                                      field->type == PBW_TYPE_FLOAT, &v);
            pbw_buf_free(&raw);
        } else {
            return PBW_ERR_INVALID;
        }
        if (st != PBW_OK) {
            return st;
        }
        if (field->type == PBW_TYPE_FLOAT) {
            float f = (float)v;

            memcpy(&bits, &f, 4);
            bits &= 0xffffffffull;
        } else {
            memcpy(&bits, &v, 8);
        }
        return j_store_uint(j, msg, field, bits);
    }
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32: {
        int is_signed = field->type != PBW_TYPE_UINT64 &&
                        field->type != PBW_TYPE_FIXED64 &&
                        field->type != PBW_TYPE_UINT32 &&
                        field->type != PBW_TYPE_FIXED32;
        int bits32 = field->type == PBW_TYPE_INT32 ||
                     field->type == PBW_TYPE_SINT32 ||
                     field->type == PBW_TYPE_SFIXED32 ||
                     field->type == PBW_TYPE_UINT32 ||
                     field->type == PBW_TYPE_FIXED32;
        pbw_status st;

        if (kind == JK_NUM) {
            const char *text;
            size_t len;

            if (j_parse_raw(j, &text, &len) != PBW_OK) {
                return PBW_ERR_INVALID;
            }
            st = j_parse_int_value(j, text, len, 0, is_signed, bits32,
                                   &bits);
        } else if (kind == JK_STR) {
            pbw_buf raw;

            pbw_buf_init(&raw);
            if (j_parse_string(j, &raw) != PBW_OK) {
                pbw_buf_free(&raw);
                return PBW_ERR_INVALID;
            }
            st = j_parse_int_value(j, (const char *)raw.data, raw.len, 1,
                                   is_signed, bits32, &bits);
            pbw_buf_free(&raw);
        } else {
            return PBW_ERR_INVALID;
        }
        if (st != PBW_OK) {
            return st;
        }
        /* SINT stores decoded, like the binary decoder. */
        return j_store_uint(j, msg, field, bits);
    }
    case PBW_TYPE_BOOL: {
        int flag = 0;

        if (kind == JK_TRUE) {
            if (j_expect_lit(j, "true") != PBW_OK) {
                return PBW_ERR_INVALID;
            }
            flag = 1;
        } else if (kind == JK_FALSE) {
            if (j_expect_lit(j, "false") != PBW_OK) {
                return PBW_ERR_INVALID;
            }
        } else if (kind == JK_STR) {
            pbw_buf raw;
            pbw_status st;

            pbw_buf_init(&raw);
            if (j_parse_string(j, &raw) != PBW_OK) {
                pbw_buf_free(&raw);
                return PBW_ERR_INVALID;
            }
            st = j_atob((const char *)raw.data, raw.len, &flag);
            pbw_buf_free(&raw);
            if (st != PBW_OK) {
                return st;
            }
        } else {
            return PBW_ERR_INVALID;
        }
        return j_store_uint(j, msg, field, flag ? 1 : 0);
    }
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES: {
        pbw_buf raw;
        pbw_status st;

        if (kind != JK_STR) {
            return PBW_ERR_INVALID;
        }
        pbw_buf_init(&raw);
        if (j_parse_string(j, &raw) != PBW_OK) {
            pbw_buf_free(&raw);
            return PBW_ERR_INVALID;
        }
        if (field->type == PBW_TYPE_STRING) {
            st = j_store_bytes(j, msg, field, raw.data, raw.len);
        } else {
            pbw_buf decoded;

            pbw_buf_init(&decoded);
            st = j_parse_base64((const char *)raw.data, raw.len, &decoded);
            if (st == PBW_OK) {
                st = j_store_bytes(j, msg, field, decoded.data,
                                   decoded.len);
            }
            pbw_buf_free(&decoded);
        }
        pbw_buf_free(&raw);
        return st;
    }
    case PBW_TYPE_ENUM:
    case PBW_TYPE_MESSAGE:
    case PBW_TYPE_GROUP:
        break;
    }
    return PBW_ERR_INVALID;
}

/* Parses an enum value; `*stored` is 0 when an unknown name was skipped
 * under ignore_unknown (the caller stores nothing then). */
static pbw_status
j_parse_enum_value(j_in *j, pbw_message *msg, const pbw_field *field,
                   int *stored)
{
    j_kind kind = j_peek_kind(j);
    int32_t number = 0;

    *stored = 1;
    if (kind == JK_STR) {
        pbw_buf raw;
        const char *text;
        size_t len;

        pbw_buf_init(&raw);
        if (j_parse_string(j, &raw) != PBW_OK ||
            !pbw_buf_reserve(&raw, 1)) {
            pbw_buf_free(&raw);
            return PBW_ERR_INVALID;
        }
        raw.data[raw.len] = '\0';
        text = (const char *)raw.data;
        len = raw.len;
        if (field->sub != NULL &&
            pbw_enum_find_value(field->sub, text, &number)) {
            pbw_buf_free(&raw);
        } else {
            /* A quoted number spells the value; anything else is unknown. */
            const char *t = text;
            size_t n = len;
            int64_t v = 0;

            j_trim(&t, &n);
            if (j_atoi64(t, n, &v) != PBW_OK || v < INT32_MIN ||
                v > INT32_MAX) {
                pbw_buf_free(&raw);
                if (j->ignore_unknown) {
                    *stored = 0;
                    return PBW_OK;
                }
                return PBW_ERR_INVALID;
            }
            number = (int32_t)v;
            pbw_buf_free(&raw);
        }
    } else if (kind == JK_NUM) {
        const char *text;
        size_t len;
        uint64_t bits = 0;

        if (j_parse_raw(j, &text, &len) != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        if (j_parse_int_value(j, text, len, 0, 1, 1, &bits) != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        number = (int32_t)bits;
    } else {
        return PBW_ERR_INVALID;
    }
    if (field->sub != NULL &&
        pbw_enum_find_name(field->sub, number, NULL)) {
        return j_store_uint(j, msg, field, (uint64_t)(int64_t)number);
    }
    /* Unknown number: open enums keep it, closed enums divert it. */
    if (field->sub == NULL || !pbw_enum_is_closed(field)) {
        return j_store_uint(j, msg, field, (uint64_t)(int64_t)number);
    }
    return j_store_unknown_enum(j, msg, field, number);
}

/* ------------------------------------------------- message parsing */

/* Well-known types with a string form. */
static pbw_status
j_parse_timestamp(j_in *j, pbw_message *msg)
{
    pbw_buf raw;
    const char *data;
    size_t left;
    uint32_t year = 0;
    uint32_t mon = 0;
    uint32_t day = 0;
    uint32_t hour = 0;
    uint32_t min = 0;
    uint32_t sec = 0;
    uint32_t frac = 0;
    size_t i;
    int64_t secs;
    uint32_t m_adj;
    uint32_t carry;
    uint32_t y_adj;
    uint32_t month_days;
    uint32_t leap_days;
    int32_t epoch_days;
    uint32_t parts[6];
    const char *suffix[6] = {"-", "-", "T", ":", ":", ""};

    if (j_peek_kind(j) == JK_NULL) {
        return j_expect_lit(j, "null");
    }
    pbw_buf_init(&raw);
    if (j_parse_string(j, &raw) != PBW_OK) {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    /* strptime-free: fixed offsets, exactly like the reference. */
    data = (const char *)raw.data;
    left = raw.len;
    if (left < 20) {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    for (i = 0; i < 6; i++) {
        int max = (i == 0) ? 4 : 2;
        int limit = max;
        uint32_t val = 0;

        /* The limit check runs before the digit test (like TakeTimeDigits),
         * so a 5-digit year or 3-digit month fails even at the suffix. */
        while (left > 0) {
            unsigned char c = (unsigned char)*data;

            if (limit-- < 0) {
                pbw_buf_free(&raw);
                return PBW_ERR_INVALID;
            }
            if (c < '0' || c > '9') {
                break;
            }
            val = val * 10 + (uint32_t)(c - '0');
            data++;
            left--;
        }
        if (suffix[i][0] != '\0') {
            if (left == 0 || *data != suffix[i][0]) {
                pbw_buf_free(&raw);
                return PBW_ERR_INVALID;
            }
            data++;
            left--;
        }
        parts[i] = val;
    }
    year = parts[0];
    mon = parts[1];
    day = parts[2];
    hour = parts[3];
    min = parts[4];
    sec = parts[5];
    if (year == 0 || mon == 0 || day == 0) {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    /* Fractional seconds: '.' plus 1-9 digits. */
    if (left > 0 && *data == '.') {
        size_t frac_digits = 0;

        data++;
        left--;
        while (left > 0 && *data >= '0' && *data <= '9') {
            if (frac_digits >= 9) {
                pbw_buf_free(&raw);
                return PBW_ERR_INVALID;
            }
            frac = frac * 10 + (uint32_t)(*data - '0');
            frac_digits++;
            data++;
            left--;
        }
        if (frac_digits == 0) {
            pbw_buf_free(&raw);
            return PBW_ERR_INVALID;
        }
        for (i = frac_digits; i < 9; i++) {
            frac *= 10;
        }
    }
    if (left == 0) {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    m_adj = mon - 3;
    carry = m_adj > mon ? 1 : 0;
    y_adj = year + 4800 - carry;
    secs = 0;
    /* March-based epoch days, exactly like the reference. */
    month_days = ((m_adj + carry * 12) * 62719 + 769) / 2048;
    leap_days = y_adj / 4 - y_adj / 100 + y_adj / 400;
    epoch_days = (int32_t)(y_adj * 365 + leap_days + month_days + (day - 1) -
                           2472632);
    secs = (int64_t)epoch_days * 86400 + (int64_t)hour * 3600 +
           (int64_t)min * 60 + (int64_t)sec;
    if (*data == 'Z') {
        if (left != 1) {
            pbw_buf_free(&raw);
            return PBW_ERR_INVALID;
        }
    } else if (*data == '+' || *data == '-') {
        int neg = *data == '-';
        uint32_t off_hour = 0;
        uint32_t off_min = 0;
        int64_t offset;
        int limit;
        const char *off_start;

        /* Exactly 6 chars (hh:mm); each part takes up to 3 digits. */
        if (left != 6) {
            pbw_buf_free(&raw);
            return PBW_ERR_INVALID;
        }
        data++;
        left--;
        off_start = data;
        limit = 2;
        while (left > 0) {
            if (limit-- < 0) {
                pbw_buf_free(&raw);
                return PBW_ERR_INVALID;
            }
            if (*data < '0' || *data > '9') {
                break;
            }
            off_hour = off_hour * 10 + (uint32_t)(*data - '0');
            data++;
            left--;
        }
        if (data == off_start || left == 0 || *data != ':') {
            pbw_buf_free(&raw);
            return PBW_ERR_INVALID;
        }
        data++;
        left--;
        limit = 2;
        while (left > 0) {
            if (limit-- < 0) {
                pbw_buf_free(&raw);
                return PBW_ERR_INVALID;
            }
            if (*data < '0' || *data > '9') {
                break;
            }
            off_min = off_min * 10 + (uint32_t)(*data - '0');
            data++;
            left--;
        }
        if (left != 0) {
            pbw_buf_free(&raw);
            return PBW_ERR_INVALID;
        }
        offset = ((int64_t)off_hour * 60 + (int64_t)off_min) * 60;
        secs += neg ? offset : -offset;
    } else {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    pbw_buf_free(&raw);
    if (j_store_uint(j, msg, pbw_descriptor_find_field(msg->desc, 1),
                     (uint64_t)secs) != PBW_OK ||
        j_store_uint(j, msg, pbw_descriptor_find_field(msg->desc, 2),
                     frac) != PBW_OK) {
        return PBW_ERR_NOMEM;
    }
    return PBW_OK;
}

static pbw_status
j_parse_duration(j_in *j, pbw_message *msg)
{
    pbw_buf raw;
    const char *text;
    size_t len;
    size_t int_end = 0;
    int64_t secs = 0;
    int32_t nanos = 0;
    int neg = 0;
    const char *rest;
    size_t rest_len;
    size_t i;

    if (j_peek_kind(j) == JK_NULL) {
        return j_expect_lit(j, "null");
    }
    pbw_buf_init(&raw);
    if (j_parse_string(j, &raw) != PBW_OK) {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    /* NUL-terminate for the integer scan. */
    if (!pbw_buf_reserve(&raw, 1)) {
        pbw_buf_free(&raw);
        return PBW_ERR_NOMEM;
    }
    raw.data[raw.len] = '\0';
    text = (const char *)raw.data;
    len = raw.len;
    while (int_end < len &&
           ((text[int_end] >= '0' && text[int_end] <= '9') ||
            text[int_end] == '-')) {
        int_end++;
    }
    if (int_end == 0 || j_atoi64(text, int_end, &secs) != PBW_OK) {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    if (secs > (int64_t)3652500 * 86400 ||
        secs < -((int64_t)3652500 * 86400)) {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    rest = text + int_end;
    rest_len = len - int_end;
    nanos = 0;
    if (rest_len > 0 && rest[0] == '.') {
        size_t frac_digits = 0;
        uint32_t frac = 0;

        rest++;
        rest_len--;
        while (rest_len > 0 && *rest >= '0' && *rest <= '9') {
            if (frac_digits >= 9) {
                pbw_buf_free(&raw);
                return PBW_ERR_INVALID;
            }
            frac = frac * 10 + (uint32_t)(*rest - '0');
            frac_digits++;
            rest++;
            rest_len--;
        }
        if (frac_digits == 0) {
            pbw_buf_free(&raw);
            return PBW_ERR_INVALID;
        }
        for (i = frac_digits; i < 9; i++) {
            frac *= 10;
        }
        nanos = (int32_t)frac;
        if (frac > 999999999) {
            pbw_buf_free(&raw);
            return PBW_ERR_INVALID;
        }
    }
    neg = secs < 0 || (int_end > 0 && text[0] == '-');
    if (neg) {
        nanos = -nanos;
    }
    if (rest_len != 1 || rest[0] != 's') {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    pbw_buf_free(&raw);
    if (j_store_uint(j, msg, pbw_descriptor_find_field(msg->desc, 1),
                     (uint64_t)secs) != PBW_OK ||
        j_store_uint(j, msg, pbw_descriptor_find_field(msg->desc, 2),
                     (uint64_t)(int64_t)nanos) != PBW_OK) {
        return PBW_ERR_NOMEM;
    }
    return PBW_OK;
}

static pbw_status
j_parse_fieldmask(j_in *j, pbw_message *msg)
{
    pbw_buf raw;
    const pbw_field *paths;
    const char *p;
    const char *end;

    if (j_peek_kind(j) == JK_NULL) {
        return j_expect_lit(j, "null");
    }
    pbw_buf_init(&raw);
    if (j_parse_string(j, &raw) != PBW_OK) {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    paths = pbw_descriptor_find_field(msg->desc, 1);
    if (paths == NULL) {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    /* An empty string is an empty mask, not one empty path. */
    if (raw.len == 0) {
        pbw_buf_free(&raw);
        return PBW_OK;
    }
    p = (const char *)raw.data;
    end = p + raw.len;
    for (;;) {
        const char *comma = p;
        pbw_buf snake;

        while (comma < end && *comma != ',') {
            comma++;
        }
        /* One path: uppercase becomes _lower, the rest passes through. */
        pbw_buf_init(&snake);
        for (; p < comma; p++) {
            char c = *p;

            if (!pbw_buf_reserve(&snake, 2)) {
                pbw_buf_free(&snake);
                pbw_buf_free(&raw);
                return PBW_ERR_NOMEM;
            }
            if (c >= 'A' && c <= 'Z') {
                snake.data[snake.len++] = '_';
                snake.data[snake.len++] = (char)(c - 'A' + 'a');
            } else {
                snake.data[snake.len++] = (uint8_t)c;
            }
        }
        if (j_store_bytes(j, msg, paths, snake.data, snake.len) != PBW_OK) {
            pbw_buf_free(&snake);
            pbw_buf_free(&raw);
            return PBW_ERR_NOMEM;
        }
        pbw_buf_free(&snake);
        if (comma >= end) {
            break;
        }
        p = comma + 1;
    }
    pbw_buf_free(&raw);
    return PBW_OK;
}

/* google.protobuf.Value from the upcoming JSON kind. */
static pbw_status
j_parse_value_msg(j_in *j, pbw_message *msg)
{
    const pbw_field *f1 = pbw_descriptor_find_field(msg->desc, 1);
    const pbw_field *f2 = pbw_descriptor_find_field(msg->desc, 2);
    const pbw_field *f3 = pbw_descriptor_find_field(msg->desc, 3);
    const pbw_field *f4 = pbw_descriptor_find_field(msg->desc, 4);
    const pbw_field *f5 = pbw_descriptor_find_field(msg->desc, 5);
    const pbw_field *f6 = pbw_descriptor_find_field(msg->desc, 6);
    j_kind kind = j_peek_kind(j);

    if (f1 == NULL || f2 == NULL || f3 == NULL || f4 == NULL || f5 == NULL ||
        f6 == NULL) {
        return PBW_ERR_INVALID;
    }
    switch (kind) {
    case JK_NULL:
        if (j_expect_lit(j, "null") != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        return j_store_uint(j, msg, f1, 0);
    case JK_NUM: {
        const char *text;
        size_t len;
        double d;

        if (j_parse_raw(j, &text, &len) != PBW_OK ||
            j_atod(text, len, j->dot, &d) != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        {
            pbw_value value;

            value.kind = PBW_VALUE_UINT;
            value.flags = msg->arena != NULL ? PBW_VF_ARENA : 0;
            memcpy(&value.as.u64, &d, 8);
            pbw_message_put(msg, (size_t)(f2 - msg->desc->fields), &value);
        }
        return PBW_OK;
    }
    case JK_STR: {
        pbw_buf raw;
        pbw_status st;

        pbw_buf_init(&raw);
        if (j_parse_string(j, &raw) != PBW_OK) {
            pbw_buf_free(&raw);
            return PBW_ERR_INVALID;
        }
        st = j_store_bytes(j, msg, f3, raw.data, raw.len);
        pbw_buf_free(&raw);
        return st;
    }
    case JK_TRUE:
        if (j_expect_lit(j, "true") != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        return j_store_uint(j, msg, f4, 1);
    case JK_FALSE:
        if (j_expect_lit(j, "false") != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        return j_store_uint(j, msg, f4, 0);
    case JK_OBJ: {
        pbw_message *sub = pbw_message_new(f5->sub);
        pbw_status st;

        if (sub == NULL) {
            return PBW_ERR_NOMEM;
        }
        /* A depth-limit breach stays LIMIT so callers (and users) can
         * tell unbounded input from malformed input. */
        st = j_parse_message(j, sub);
        if (st != PBW_OK) {
            pbw_message_free(sub);
            return st == PBW_ERR_LIMIT ? st : PBW_ERR_INVALID;
        }
        return j_store_message(j, msg, f5, sub);
    }
    case JK_ARR: {
        pbw_message *sub = pbw_message_new(f6->sub);
        pbw_status st;

        if (sub == NULL) {
            return PBW_ERR_NOMEM;
        }
        st = j_parse_message(j, sub);
        if (st != PBW_OK) {
            pbw_message_free(sub);
            return st == PBW_ERR_LIMIT ? st : PBW_ERR_INVALID;
        }
        return j_store_message(j, msg, f6, sub);
    }
    default:
        return PBW_ERR_INVALID;
    }
}

/* google.protobuf.Struct: a map<string, Value> in disguise. */
static pbw_status
j_parse_struct(j_in *j, pbw_message *msg)
{
    const pbw_field *entry_field = pbw_descriptor_find_field(msg->desc, 1);
    const pbw_field *key_field;
    const pbw_field *value_field;
    const pbw_descriptor *entry;

    if (entry_field == NULL || entry_field->sub == NULL) {
        return PBW_ERR_INVALID;
    }
    entry = entry_field->sub;
    key_field = pbw_descriptor_find_field(entry, 1);
    value_field = pbw_descriptor_find_field(entry, 2);
    if (key_field == NULL || value_field == NULL) {
        return PBW_ERR_INVALID;
    }
    if (j_peek_kind(j) == JK_NULL) {
        return j_expect_lit(j, "null");
    }
    if (j_expect_ch(j, '{') != PBW_OK) {
        return PBW_ERR_INVALID;
    }
    if (j->depth >= PBW_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    j->depth++;
    j_skip(&j->cur);
    if (j->cur.p < j->cur.end && *j->cur.p == '}') {
        j->cur.p++;
        j->depth--;
        return PBW_OK;
    }
    for (;;) {
        pbw_buf key;
        pbw_message *item;
        pbw_message *val;
        pbw_status st;

        pbw_buf_init(&key);
        if (j_parse_key(j, &key) != PBW_OK) {
            pbw_buf_free(&key);
            j->depth--;
            return PBW_ERR_INVALID;
        }
        if (j_expect_ch(j, ':') != PBW_OK) {
            pbw_buf_free(&key);
            j->depth--;
            return PBW_ERR_INVALID;
        }
        /* Duplicate struct keys fail, like map keys. */
        {
            pbw_value *list = pbw_message_list_for(msg, entry_field);
            size_t i;

            if (list == NULL) {
                pbw_buf_free(&key);
                j->depth--;
                return PBW_ERR_NOMEM;
            }
            for (i = 0; i < list->as.list.len; i++) {
                const pbw_value *e = list->as.list.items[i];
                const pbw_value *k;

                if (e == NULL || e->kind != PBW_VALUE_MESSAGE ||
                    e->as.msg == NULL) {
                    continue;
                }
                k = pbw_message_get(e->as.msg, 1);
                if (k != NULL && k->kind == PBW_VALUE_BYTES &&
                    k->as.bytes.len == key.len &&
                    memcmp(k->as.bytes.data, key.data, key.len) == 0) {
                    pbw_buf_free(&key);
                    j->depth--;
                    return PBW_ERR_INVALID;
                }
            }
        }
        item = pbw_message_new(entry);
        if (item == NULL) {
            pbw_buf_free(&key);
            j->depth--;
            return PBW_ERR_NOMEM;
        }
        st = j_store_bytes(j, item, key_field, key.data, key.len);
        pbw_buf_free(&key);
        if (st != PBW_OK) {
            pbw_message_free(item);
            j->depth--;
            return st;
        }
        val = pbw_message_new(value_field->sub);
        if (val == NULL) {
            pbw_message_free(item);
            j->depth--;
            return PBW_ERR_NOMEM;
        }
        /* The value parses as a Value message, like a map value would. */
        st = j_parse_message(j, val);
        if (st != PBW_OK) {
            pbw_message_free(val);
            pbw_message_free(item);
            j->depth--;
            return st == PBW_ERR_LIMIT ? st : PBW_ERR_INVALID;
        }
        st = j_store_message(j, item, value_field, val);
        if (st != PBW_OK) {
            pbw_message_free(item);
            j->depth--;
            return st;
        }
        st = j_store_message(j, msg, entry_field, item);
        if (st != PBW_OK) {
            j->depth--;
            return st;
        }
        j_skip(&j->cur);
        if (j->cur.p < j->cur.end && *j->cur.p == ',') {
            j->cur.p++;
            /* Trailing commas are accepted in legacy mode. */
            j_skip(&j->cur);
            if (j->cur.p < j->cur.end && *j->cur.p == '}') {
                j->cur.p++;
                j->depth--;
                return PBW_OK;
            }
            continue;
        }
        if (j_expect_ch(j, '}') != PBW_OK) {
            j->depth--;
            return PBW_ERR_INVALID;
        }
        j->depth--;
        return PBW_OK;
    }
}

/* Emits the reference's null element for repeated fields: a default value
 * (an empty message for message fields). */
static pbw_status
j_emit_null(j_in *j, pbw_message *msg, const pbw_field *field)
{
    switch (field->type) {
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_BOOL:
    case PBW_TYPE_ENUM:
        return j_store_uint(j, msg, field, 0);
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
        return j_store_bytes(j, msg, field, NULL, 0);
    case PBW_TYPE_MESSAGE:
    case PBW_TYPE_GROUP: {
        pbw_message *sub;

        if (field->sub == NULL) {
            return PBW_ERR_INVALID;
        }
        sub = pbw_message_new(field->sub);
        if (sub == NULL) {
            return PBW_ERR_NOMEM;
        }
        return j_store_message(j, msg, field, sub);
    }
    }
    return PBW_ERR_INVALID;
}

/* Parses one array element of a repeated field. */
static pbw_status
j_parse_element(j_in *j, pbw_message *msg, const pbw_field *field)
{
    j_kind kind = j_peek_kind(j);
    j_wkt wkt = field->sub != NULL ? j_classify(field->sub) : J_NOT_WKT;

    if (kind == JK_NULL) {
        if ((field->type == PBW_TYPE_MESSAGE && wkt == J_VALUE) ||
            (field->type == PBW_TYPE_ENUM && field->sub != NULL &&
             j_classify(field->sub) == J_NULLVALUE)) {
            return j_parse_field_value(j, msg, field, 1);
        }
        if (j_expect_lit(j, "null") != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        return j_emit_null(j, msg, field);
    }
    /* Nested arrays flatten into the current one (legacy mode), except
     * inside Value and ListValue. */
    if (kind == JK_ARR && wkt != J_VALUE && wkt != J_LIST) {
        if (j_expect_ch(j, '[') != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        if (j->depth >= PBW_MAX_DEPTH) {
            return PBW_ERR_LIMIT;
        }
        j->depth++;
        j_skip(&j->cur);
        if (j->cur.p < j->cur.end && *j->cur.p == ']') {
            j->cur.p++;
            j->depth--;
            return PBW_OK;
        }
        for (;;) {
            if (j_parse_element(j, msg, field) != PBW_OK) {
                j->depth--;
                return PBW_ERR_INVALID;
            }
            j_skip(&j->cur);
            if (j->cur.p < j->cur.end && *j->cur.p == ',') {
                j->cur.p++;
                j_skip(&j->cur);
                if (j->cur.p < j->cur.end && *j->cur.p == ']') {
                    j->cur.p++;
                    j->depth--;
                    return PBW_OK;
                }
                continue;
            }
            if (j_expect_ch(j, ']') != PBW_OK) {
                j->depth--;
                return PBW_ERR_INVALID;
            }
            j->depth--;
            return PBW_OK;
        }
    }
    return j_parse_field_value(j, msg, field, 1);
}

static pbw_status
j_parse_array(j_in *j, pbw_message *msg, const pbw_field *field)
{
    if (j_expect_ch(j, '[') != PBW_OK) {
        return PBW_ERR_INVALID;
    }
    if (j->depth >= PBW_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    j->depth++;
    j_skip(&j->cur);
    if (j->cur.p < j->cur.end && *j->cur.p == ']') {
        j->cur.p++;
        j->depth--;
        return PBW_OK;
    }
    for (;;) {
        if (j_parse_element(j, msg, field) != PBW_OK) {
            j->depth--;
            return PBW_ERR_INVALID;
        }
        j_skip(&j->cur);
        if (j->cur.p < j->cur.end && *j->cur.p == ',') {
            j->cur.p++;
            /* Trailing commas are accepted in legacy mode. */
            j_skip(&j->cur);
            if (j->cur.p < j->cur.end && *j->cur.p == ']') {
                j->cur.p++;
                j->depth--;
                return PBW_OK;
            }
            continue;
        }
        if (j_expect_ch(j, ']') != PBW_OK) {
            j->depth--;
            return PBW_ERR_INVALID;
        }
        j->depth--;
        return PBW_OK;
    }
}

/* Map keys: strict integers, exact bools, raw strings. */
static pbw_status
j_parse_map_key(const char *text, size_t len, const pbw_field *key_field,
                pbw_message *entry)
{
    switch (key_field->type) {
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32: {
        const char *t = text;
        size_t n = len;
        int64_t v = 0;

        j_trim(&t, &n);
        if (j_atoi64(t, n, &v) != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        if ((key_field->type == PBW_TYPE_INT32 ||
             key_field->type == PBW_TYPE_SINT32 ||
             key_field->type == PBW_TYPE_SFIXED32) &&
            (v < INT32_MIN || v > INT32_MAX)) {
            return PBW_ERR_INVALID;
        }
        return j_store_uint(NULL, entry, key_field, (uint64_t)v);
    }
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32: {
        const char *t = text;
        size_t n = len;
        uint64_t v = 0;

        j_trim(&t, &n);
        if (j_atou64(t, n, &v) != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        if ((key_field->type == PBW_TYPE_UINT32 ||
             key_field->type == PBW_TYPE_FIXED32) &&
            v > UINT32_MAX) {
            return PBW_ERR_INVALID;
        }
        return j_store_uint(NULL, entry, key_field, v);
    }
    case PBW_TYPE_BOOL:
        /* Map keys want exactly "true"/"false", not the Atob set. */
        if (len == 4 && memcmp(text, "true", 4) == 0) {
            return j_store_uint(NULL, entry, key_field, 1);
        }
        if (len == 5 && memcmp(text, "false", 5) == 0) {
            return j_store_uint(NULL, entry, key_field, 0);
        }
        return PBW_ERR_INVALID;
    case PBW_TYPE_STRING:
        return j_store_bytes(NULL, entry, key_field, (const uint8_t *)text,
                             len);
    default:
        return PBW_ERR_INVALID;
    }
}

static pbw_status
j_parse_map(j_in *j, pbw_message *msg, const pbw_field *field)
{
    const pbw_descriptor *entry;
    const pbw_field *key_field;
    const pbw_field *value_field;

    if (field->sub == NULL) {
        return PBW_ERR_INVALID;
    }
    entry = field->sub;
    key_field = pbw_descriptor_find_field(entry, 1);
    value_field = pbw_descriptor_find_field(entry, 2);
    if (key_field == NULL || value_field == NULL) {
        return PBW_ERR_INVALID;
    }
    if (j_expect_ch(j, '{') != PBW_OK) {
        return PBW_ERR_INVALID;
    }
    if (j->depth >= PBW_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    j->depth++;
    j_skip(&j->cur);
    if (j->cur.p < j->cur.end && *j->cur.p == '}') {
        j->cur.p++;
        j->depth--;
        return PBW_OK;
    }
    for (;;) {
        pbw_buf key;
        pbw_message *item;
        pbw_status st;
        size_t i;

        pbw_buf_init(&key);
        if (j_parse_key(j, &key) != PBW_OK) {
            pbw_buf_free(&key);
            j->depth--;
            return PBW_ERR_INVALID;
        }
        if (j_expect_ch(j, ':') != PBW_OK) {
            pbw_buf_free(&key);
            j->depth--;
            return PBW_ERR_INVALID;
        }
        item = pbw_message_new(entry);
        if (item == NULL) {
            pbw_buf_free(&key);
            j->depth--;
            return PBW_ERR_INVALID;
        }
        /* Keys compare by their stored form; integers normalize first, so
         * "1" and "01" collide exactly like two identical strings. */
        if (j_parse_map_key((const char *)key.data, key.len, key_field,
                            item) != PBW_OK) {
            pbw_message_free(item);
            pbw_buf_free(&key);
            j->depth--;
            return PBW_ERR_INVALID;
        }
        pbw_buf_free(&key);
        /* Duplicate map keys fail. Keys compare by their stored form;
         * integers normalize first, so "1" and "01" collide exactly
         * like two identical strings. The loop stays inline (no helper
         * call): large maps scan this per key. */
        {
            pbw_value *list = pbw_message_list_for(msg, field);
            const pbw_value *knew;

            if (list == NULL) {
                pbw_message_free(item);
                j->depth--;
                return PBW_ERR_NOMEM;
            }
            knew = pbw_message_get(item, 1);
            if (knew != NULL &&
                (knew->kind == PBW_VALUE_BYTES ||
                 knew->kind == PBW_VALUE_UINT)) {
                for (i = 0; i < list->as.list.len; i++) {
                    const pbw_value *e = list->as.list.items[i];
                    const pbw_value *k;

                    if (e == NULL || e->kind != PBW_VALUE_MESSAGE ||
                        e->as.msg == NULL) {
                        continue;
                    }
                    k = pbw_message_get(e->as.msg, 1);
                    if (k == NULL || k->kind != knew->kind) {
                        continue;
                    }
                    if (k->kind == PBW_VALUE_BYTES) {
                        if (k->as.bytes.len == knew->as.bytes.len &&
                            memcmp(k->as.bytes.data, knew->as.bytes.data,
                                   k->as.bytes.len) == 0) {
                            pbw_message_free(item);
                            j->depth--;
                            return PBW_ERR_INVALID;
                        }
                    } else if (k->as.u64 == knew->as.u64) {
                        pbw_message_free(item);
                        j->depth--;
                        return PBW_ERR_INVALID;
                    }
                }
            }
        }
        if (value_field->type == PBW_TYPE_ENUM) {
            int stored = 0;

            /* Unknown enum values are dropped with ignore_unknown. */
            if (j_parse_enum_value(j, item, value_field, &stored) !=
                PBW_OK) {
                pbw_message_free(item);
                j->depth--;
                return PBW_ERR_INVALID;
            }
            if (!stored) {
                pbw_message_free(item);
            } else {
                st = j_store_message(j, msg, field, item);
                if (st != PBW_OK) {
                    j->depth--;
                    return st;
                }
            }
        } else if (value_field->type == PBW_TYPE_MESSAGE ||
                   value_field->type == PBW_TYPE_GROUP) {
            pbw_message *sub = pbw_message_new(value_field->sub);

            if (sub == NULL) {
                pbw_message_free(item);
                j->depth--;
                return PBW_ERR_NOMEM;
            }
            if (j_parse_message(j, sub) != PBW_OK) {
                pbw_message_free(sub);
                pbw_message_free(item);
                j->depth--;
                return PBW_ERR_INVALID;
            }
            st = j_store_message(j, item, value_field, sub);
            if (st != PBW_OK) {
                pbw_message_free(item);
                j->depth--;
                return st;
            }
            st = j_store_message(j, msg, field, item);
            if (st != PBW_OK) {
                j->depth--;
                return st;
            }
        } else {
            if (j_parse_scalar_value(j, item, value_field) != PBW_OK) {
                pbw_message_free(item);
                j->depth--;
                return PBW_ERR_INVALID;
            }
            st = j_store_message(j, msg, field, item);
            if (st != PBW_OK) {
                j->depth--;
                return st;
            }
        }
        j_skip(&j->cur);
        if (j->cur.p < j->cur.end && *j->cur.p == ',') {
            j->cur.p++;
            /* Trailing commas are accepted in legacy mode. */
            j_skip(&j->cur);
            if (j->cur.p < j->cur.end && *j->cur.p == '}') {
                j->cur.p++;
                j->depth--;
                return PBW_OK;
            }
            continue;
        }
        if (j_expect_ch(j, '}') != PBW_OK) {
            j->depth--;
            return PBW_ERR_INVALID;
        }
        j->depth--;
        return PBW_OK;
    }
}

/* google.protobuf.Any: buffer the object, find @type, then parse. */
static pbw_status
j_parse_any(j_in *j, pbw_message *msg)
{
    const pbw_field *url_field = pbw_descriptor_find_field(msg->desc, 1);
    const pbw_field *value_field = pbw_descriptor_find_field(msg->desc, 2);
    const char *start;
    const char *end;
    int depth0;
    char type_name[256];
    size_t type_len = 0;
    const pbw_descriptor *resolved = NULL;
    pbw_message *sub = NULL;
    pbw_buf buf;
    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    pbw_status st;

    if (url_field == NULL || value_field == NULL) {
        return PBW_ERR_INVALID;
    }
    /* Scan the object once to find @type (values are skipped). */
    if (j_expect_ch(j, '{') != PBW_OK) {
        return PBW_ERR_INVALID;
    }
    depth0 = j->depth;
    start = j->cur.p;
    if (j->depth >= PBW_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    j->depth++;
    j_skip(&j->cur);
    if (j->cur.p < j->cur.end && *j->cur.p == '}') {
        j->cur.p++;
        j->depth--;
        /* Empty objects parse as an empty Any in legacy mode. */
        return PBW_OK;
    }
    for (;;) {
        pbw_buf key;
        int is_type;

        pbw_buf_init(&key);
        if (j_parse_key(j, &key) != PBW_OK) {
            pbw_buf_free(&key);
            j->depth = depth0;
            return PBW_ERR_INVALID;
        }
        if (j_expect_ch(j, ':') != PBW_OK) {
            pbw_buf_free(&key);
            j->depth = depth0;
            return PBW_ERR_INVALID;
        }
        is_type = key.len == 5 && memcmp(key.data, "@type", 5) == 0;
        if (is_type) {
            pbw_buf url;

            if (type_len != 0) {
                pbw_buf_free(&key);
                j->depth = depth0;
                return PBW_ERR_INVALID;
            }
            pbw_buf_init(&url);
            if (j_parse_string(j, &url) != PBW_OK) {
                pbw_buf_free(&url);
                pbw_buf_free(&key);
                j->depth = depth0;
                return PBW_ERR_INVALID;
            }
            /* The stored type_url is the full URL; resolution strips it. */
            if (url.len >= sizeof(type_name)) {
                pbw_buf_free(&url);
                pbw_buf_free(&key);
                j->depth = depth0;
                return PBW_ERR_INVALID;
            }
            memcpy(type_name, url.data, url.len);
            type_len = url.len;
            type_name[type_len] = '\0';
            pbw_buf_free(&url);
        } else if (j_skip_value(j) != PBW_OK) {
            pbw_buf_free(&key);
            j->depth = depth0;
            return PBW_ERR_INVALID;
        }
        pbw_buf_free(&key);
        j_skip(&j->cur);
        if (j->cur.p < j->cur.end && *j->cur.p == ',') {
            j->cur.p++;
            j_skip(&j->cur);
            if (j->cur.p < j->cur.end && *j->cur.p == '}') {
                j->cur.p++;
                break;
            }
            continue;
        }
        if (j_expect_ch(j, '}') != PBW_OK) {
            j->depth = depth0;
            return PBW_ERR_INVALID;
        }
        break;
    }
    end = j->cur.p;
    j->depth = depth0;
    if (type_len == 0) {
        /* A non-empty object without @type fails. */
        return PBW_ERR_INVALID;
    }
    /* The type name is whatever follows the last '/'; the stored type_url
     * keeps the full URL. */
    {
        const char *slash = NULL;
        char name[256];
        size_t off;
        size_t i;

        for (i = 0; i < type_len; i++) {
            if (type_name[i] == '/') {
                slash = type_name + i;
            }
        }
        off = slash == NULL ? 0 : (size_t)(slash - type_name) + 1;
        if (type_len - off >= sizeof(name)) {
            return PBW_ERR_INVALID;
        }
        memcpy(name, type_name + off, type_len - off);
        name[type_len - off] = '\0';
        resolved = msg->desc->schema == NULL
                       ? NULL
                       : pbw_schema_find(msg->desc->schema, name);
    }
    if (resolved == NULL || resolved->is_enum) {
        if (j->ignore_unknown) {
            return PBW_OK;
        }
        return PBW_ERR_INVALID;
    }
    sub = pbw_message_new(resolved);
    if (sub == NULL) {
        return PBW_ERR_NOMEM;
    }
    /* Re-walk the buffered object, skipping @type. */
    {
        j_in inner;
        j_wkt wkt = j_classify(resolved);

        inner.cur.p = start;
        inner.cur.end = end;
        inner.ignore_unknown = j->ignore_unknown;
        inner.depth = j->depth + 1;
        /* start already points past the '{' the scan consumed. */
        for (;;) {
            pbw_buf key;
            int is_type;
            int is_value;

            j_skip(&inner.cur);
            if (inner.cur.p < inner.cur.end && *inner.cur.p == '}') {
                inner.cur.p++;
                break;
            }
            pbw_buf_init(&key);
            if (j_parse_key(&inner, &key) != PBW_OK ||
                j_expect_ch(&inner, ':') != PBW_OK) {
                pbw_buf_free(&key);
                pbw_message_free(sub);
                return PBW_ERR_INVALID;
            }
            is_type = key.len == 5 && memcmp(key.data, "@type", 5) == 0;
            is_value = key.len == 5 && memcmp(key.data, "value", 5) == 0;
            if (is_type) {
                pbw_buf_free(&key);
                if (j_skip_value(&inner) != PBW_OK) {
                    pbw_message_free(sub);
                    return PBW_ERR_INVALID;
                }
            } else if (wkt != J_NOT_WKT) {
                /* Well-known payloads live under a "value" key. */
                pbw_buf_free(&key);
                if (!is_value || j_parse_message(&inner, sub) != PBW_OK) {
                    pbw_message_free(sub);
                    return PBW_ERR_INVALID;
                }
            } else {
                /* Regular fields parse by name, like the outer object. */
                pbw_status fst = j_parse_named_field(&inner, sub, key.data,
                                                     key.len);

                pbw_buf_free(&key);
                if (fst != PBW_OK) {
                    pbw_message_free(sub);
                    return fst;
                }
            }
            j_skip(&inner.cur);
            if (inner.cur.p < inner.cur.end && *inner.cur.p == ',') {
                inner.cur.p++;
                continue;
            }
            if (j_expect_ch(&inner, '}') != PBW_OK) {
                pbw_message_free(sub);
                return PBW_ERR_INVALID;
            }
            break;
        }
    }
    /* Serialize the expansion into the Any's value bytes. */
    pbw_buf_init(&buf);
    st = pbw_encode(sub, &bytes, &bytes_len);
    pbw_message_free(sub);
    if (st != PBW_OK) {
        return st;
    }
    if (j_store_bytes(j, msg, url_field, (const uint8_t *)type_name,
                      type_len) != PBW_OK ||
        (bytes_len > 0 &&
         j_store_bytes(j, msg, value_field, bytes, bytes_len) != PBW_OK)) {
        pbw_free(bytes);
        return PBW_ERR_NOMEM;
    }
    pbw_free(bytes);
    return PBW_OK;
}

/* Parses the value of one already-looked-up key. */
static pbw_status
j_parse_named_field(j_in *j, pbw_message *msg, const uint8_t *key,
                    size_t len)
{
    const pbw_field *field = NULL;

    if (len >= 2 && key[0] == '[' && key[len - 1] == ']') {
        /* Extension names are dotted and global, like the text format. */
        char name[256];
        const pbw_field *ext;

        if (len - 2 >= sizeof(name)) {
            return PBW_ERR_INVALID;
        }
        memcpy(name, key + 1, len - 2);
        name[len - 2] = '\0';
        ext = msg->desc->schema == NULL
                  ? NULL
                  : pbw_schema_find_extension(msg->desc->schema, name);
        if (ext == NULL) {
            if (j->ignore_unknown) {
                return j_skip_value(j);
            }
            return PBW_ERR_INVALID;
        }
        /* The extension must belong to this message. */
        if (pbw_descriptor_find_field(msg->desc, ext->number) != ext) {
            return PBW_ERR_INVALID;
        }
        field = ext;
    } else if (!j_field_by_name(msg->desc, (const char *)key, len, &field)) {
        if (j->ignore_unknown) {
            return j_skip_value(j);
        }
        return PBW_ERR_INVALID;
    }
    return j_parse_field_value(j, msg, field, 0);
}

/* The `{...}` body after the brace is consumed. */
static pbw_status
j_parse_object_body(j_in *j, pbw_message *msg)
{
    if (j->depth >= PBW_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    j->depth++;
    j_skip(&j->cur);
    if (j->cur.p < j->cur.end && *j->cur.p == '}') {
        j->cur.p++;
        j->depth--;
        return PBW_OK;
    }
    for (;;) {
        pbw_buf key;
        pbw_status st;

        pbw_buf_init(&key);
        if (j_parse_key(j, &key) != PBW_OK) {
            pbw_buf_free(&key);
            j->depth--;
            return PBW_ERR_INVALID;
        }
        if (j_expect_ch(j, ':') != PBW_OK) {
            pbw_buf_free(&key);
            j->depth--;
            return PBW_ERR_INVALID;
        }
        st = j_parse_named_field(j, msg, key.data, key.len);
        pbw_buf_free(&key);
        if (st != PBW_OK) {
            j->depth--;
            return st;
        }
        j_skip(&j->cur);
        if (j->cur.p < j->cur.end && *j->cur.p == ',') {
            j->cur.p++;
            /* Trailing commas are accepted in legacy mode. */
            j_skip(&j->cur);
            if (j->cur.p < j->cur.end && *j->cur.p == '}') {
                j->cur.p++;
                j->depth--;
                return PBW_OK;
            }
            continue;
        }
        if (j_expect_ch(j, '}') != PBW_OK) {
            j->depth--;
            return PBW_ERR_INVALID;
        }
        j->depth--;
        return PBW_OK;
    }
}

/* Parses the value of one field: nulls, oneof/duplicate rules, dispatch. */
static pbw_status
j_parse_field_value(j_in *j, pbw_message *msg, const pbw_field *field,
                    int in_array)
{
    int is_value = field->type == PBW_TYPE_MESSAGE && field->sub != NULL &&
                   j_classify(field->sub) == J_VALUE;
    int is_nullvalue = field->type == PBW_TYPE_ENUM && field->sub != NULL &&
                       j_classify(field->sub) == J_NULLVALUE;

    (void)in_array;
    /* Any `null` is a no-op, except for Value and NullValue. */
    if (j_peek_kind(j) == JK_NULL && !is_value && !is_nullvalue) {
        return j_expect_lit(j, "null");
    }
    /* Singular fields parse once per oneof; anything else overwrites. */
    if (field->label != PBW_LABEL_REPEATED) {
        size_t index = (size_t)(field - msg->desc->fields);

        if (field->oneof_index != 0) {
            if (msg->oneof_case[field->oneof_index] != 0) {
                return PBW_ERR_INVALID;
            }
        } else if (pbw_msg_present(msg, index)) {
            /* Legacy mode allows the duplicate (last wins): fall through
             * without clearing, like the reference. */
        }
    }
    if (field->type == PBW_TYPE_MESSAGE || field->type == PBW_TYPE_GROUP) {
        if (field->sub != NULL && field->sub->map_entry) {
            return j_parse_map(j, msg, field);
        }
        if (field->label == PBW_LABEL_REPEATED) {
            /* Like the reference, a repeated message field wants an array:
             * a single value does not parse (see SingleValue tests). Array
             * elements arrive here with in_array set and parse as one. */
            if (!in_array && j_peek_kind(j) != JK_ARR) {
                return PBW_ERR_INVALID;
            }
            if (!in_array) {
                return j_parse_array(j, msg, field);
            }
        }
        {
            pbw_message *sub = pbw_message_new(field->sub);
            pbw_status st;

            if (sub == NULL) {
                return PBW_ERR_NOMEM;
            }
            /* Like j_parse_value_msg: a depth breach stays LIMIT. */
            st = j_parse_message(j, sub);
            if (st != PBW_OK) {
                pbw_message_free(sub);
                return st == PBW_ERR_LIMIT ? st : PBW_ERR_INVALID;
            }
            return j_store_message(j, msg, field, sub);
        }
    }
    if (field->label == PBW_LABEL_REPEATED) {
        /* Like the reference, a repeated scalar field wants an array. */
        if (!in_array && j_peek_kind(j) != JK_ARR) {
            return PBW_ERR_INVALID;
        }
        if (!in_array) {
            return j_parse_array(j, msg, field);
        }
        if (field->type == PBW_TYPE_ENUM) {
            int stored = 0;

            return j_parse_enum_value(j, msg, field, &stored);
        }
        return j_parse_scalar_value(j, msg, field);
    }
    if (field->type == PBW_TYPE_ENUM) {
        int stored = 0;
        pbw_status st = j_parse_enum_value(j, msg, field, &stored);

        /* Unknown names are skipped under ignore_unknown, even for
         * proto3 implicit-presence enums (which the reference sets to 0,
         * indistinguishable from unset). */
        (void)stored;
        return st;
    }
    return j_parse_scalar_value(j, msg, field);
}

/* Routes a message value by kind: Any/Value/Struct intercept objects and
 * everything else; List/wrappers/Timestamp/Duration/FieldMask only
 * intercept their special (non-object) form. */
static pbw_status
j_parse_message(j_in *j, pbw_message *msg)
{
    j_wkt wkt = j_classify(msg->desc);
    j_kind kind;

    if (msg->desc->is_enum) {
        return PBW_ERR_INVALID;
    }
    switch (wkt) {
    case J_ANY:
        if (j_peek_kind(j) == JK_NULL) {
            return j_expect_lit(j, "null");
        }
        return j_parse_any(j, msg);
    case J_VALUE:
        return j_parse_value_msg(j, msg);
    case J_STRUCT:
        return j_parse_struct(j, msg);
    default:
        break;
    }
    kind = j_peek_kind(j);
    if (kind != JK_OBJ) {
        switch (wkt) {
        case J_LIST:
            return j_parse_list_of(j, msg);
        case J_WRAPPER: {
            const pbw_field *inner =
                pbw_descriptor_find_field(msg->desc, 1);

            if (inner == NULL) {
                return PBW_ERR_INVALID;
            }
            return j_parse_field_value(j, msg, inner, 0);
        }
        case J_TIMESTAMP:
            return j_parse_timestamp(j, msg);
        case J_DURATION:
            return j_parse_duration(j, msg);
        case J_FIELDMASK:
            return j_parse_fieldmask(j, msg);
        case J_EMPTY:
            return PBW_ERR_INVALID;
        default:
            return PBW_ERR_INVALID;
        }
    }
    if (j_expect_ch(j, '{') != PBW_OK) {
        return PBW_ERR_INVALID;
    }
    return j_parse_object_body(j, msg);
}

/* google.protobuf.ListValue: an array of Values. No flattening here. */
static pbw_status
j_parse_list_of(j_in *j, pbw_message *msg)
{
    const pbw_field *entry_field = pbw_descriptor_find_field(msg->desc, 1);

    if (entry_field == NULL || entry_field->sub == NULL) {
        return PBW_ERR_INVALID;
    }
    if (j_peek_kind(j) == JK_NULL) {
        return j_expect_lit(j, "null");
    }
    if (j_expect_ch(j, '[') != PBW_OK) {
        return PBW_ERR_INVALID;
    }
    if (j->depth >= PBW_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    j->depth++;
    j_skip(&j->cur);
    if (j->cur.p < j->cur.end && *j->cur.p == ']') {
        j->cur.p++;
        j->depth--;
        return PBW_OK;
    }
    for (;;) {
        pbw_message *item = pbw_message_new(entry_field->sub);
        pbw_status st;

        if (item == NULL) {
            j->depth--;
            return PBW_ERR_NOMEM;
        }
        st = j_parse_value_msg(j, item);
        if (st != PBW_OK) {
            pbw_message_free(item);
            j->depth--;
            return st == PBW_ERR_LIMIT ? st : PBW_ERR_INVALID;
        }
        st = j_store_message(j, msg, entry_field, item);
        if (st != PBW_OK) {
            j->depth--;
            return st;
        }
        j_skip(&j->cur);
        if (j->cur.p < j->cur.end && *j->cur.p == ',') {
            j->cur.p++;
            /* Trailing commas are accepted in legacy mode. */
            j_skip(&j->cur);
            if (j->cur.p < j->cur.end && *j->cur.p == ']') {
                j->cur.p++;
                j->depth--;
                return PBW_OK;
            }
            continue;
        }
        if (j_expect_ch(j, ']') != PBW_OK) {
            j->depth--;
            return PBW_ERR_INVALID;
        }
        j->depth--;
        return PBW_OK;
    }
}

pbw_status
pbw_json_parse_opts(const pbw_descriptor *desc, const char *text,
                    size_t len, const pbw_json_parse_options *options,
                    pbw_message **out)
{
    j_in j;
    pbw_message *msg;

    if (out != NULL) {
        *out = NULL;
    }
    if (desc == NULL || (text == NULL && len != 0) || out == NULL) {
        return PBW_ERR_INVALID;
    }
    if (desc->is_enum) {
        return PBW_ERR_TYPE;
    }
    if (desc->schema != NULL && !desc->schema->linked) {
        return PBW_ERR_INVALID;
    }
    if (text == NULL) {
        text = (const char *)pbw_empty_input;
    }
    msg = pbw_message_new(desc);
    if (msg == NULL) {
        return PBW_ERR_NOMEM;
    }
    j.cur.p = text;
    j.cur.end = text + len;
    j.ignore_unknown =
        options != NULL && options->ignore_unknown_fields;
    j.depth = 0;
    j.dot = pbw_locale_dot();
    {
        pbw_status st = j_parse_message(&j, msg);

        if (st != PBW_OK) {
            pbw_message_free(msg);
            return st;
        }
    }
    j_skip(&j.cur);
    if (j.cur.p != j.cur.end) {
        pbw_message_free(msg);
        return PBW_ERR_INVALID;
    }
    /* A missing proto2 `required` field fails, like the binary decoder. */
    if (desc->has_required && !pbw_message_is_initialized(msg)) {
        pbw_message_free(msg);
        return PBW_ERR_REQUIRED;
    }
    *out = msg;
    return PBW_OK;
}

pbw_status
pbw_json_parse(const pbw_descriptor *desc, const char *text, size_t len,
               pbw_message **out)
{
    return pbw_json_parse_opts(desc, text, len, NULL, out);
}
