/* Protobuf text format: human-readable message printing and parsing.
 *
 * The printer emits what the reference implementation prints (multi-line,
 * two-space indent, `name: value` for scalars, `name { ... }` for messages,
 * `[full.name]` for extensions, shortest round-tripping floats), so its
 * output parses back to the same message. The parser accepts what the
 * reference parser accepts: optional colons before `{`/`<`, required colons
 * before scalar values, `[a, b]` lists, `<`/`>` delimiters, group type names,
 * extensions, `Any` expansions, C escapes, decimal/hex/octal integers and the
 * same range errors. Unknown field names fail unless reserved; reserved names
 * are skipped silently, like the reference.
 *
 * Like the binary decoder, nesting is capped at PBW_MAX_DEPTH and a missing
 * proto2 `required` field fails the parse. Unlike the binary format, string
 * fields reject invalid UTF-8 (the reference text parser does too).
 */

#include "internal.h"

#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ output */

typedef struct {
    pbw_buf buf;
    int indent;
    int print_unknown;
    pbw_status status;
    int dot; /* the host locale's decimal point is '.' (checked once per call) */
} text_out;

static void
t_put(text_out *t, const void *data, size_t len)
{
    if (t->status != PBW_OK || len == 0) {
        return;
    }
    if (!pbw_buf_reserve(&t->buf, len)) {
        t->status = PBW_ERR_NOMEM;
        return;
    }
    memcpy(t->buf.data + t->buf.len, data, len);
    t->buf.len += len;
}

static void
t_str(text_out *t, const char *s)
{
    t_put(t, s, strlen(s));
}

static void
t_indent(text_out *t, int extra)
{
    int i;
    int n = t->indent + extra;

    for (i = 0; i < n; i++) {
        t_put(t, "  ", 2);
    }
}

static void
t_u64(text_out *t, uint64_t value)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)value);

    if (n > 0) {
        t_put(t, tmp, (size_t)n);
    }
}

static void
t_i64(text_out *t, int64_t value)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)value);

    if (n > 0) {
        t_put(t, tmp, (size_t)n);
    }
}

/* Shortest decimal that still parses back to the same bits. */
static void
t_double(text_out *t, double value, int is_float)
{
    int prec;
    int prec_max = 17;
    int prec_min = is_float ? 9 : 15;

    if (isnan(value)) {
        t_str(t, "nan");
        return;
    }
    if (isinf(value)) {
        t_str(t, value < 0 ? "-inf" : "inf");
        return;
    }
    for (prec = prec_min; prec <= prec_max; prec++) {
        char tmp[32];
        int n = pbw_format_g(tmp, sizeof(tmp), prec, value, t->dot);
        double back;

        if (n <= 0 || (size_t)n >= sizeof(tmp)) {
            continue;
        }
        back = pbw_strtod(tmp, NULL, t->dot);
        if ((is_float ? (double)(float)back == value : back == value)) {
            t_put(t, tmp, (size_t)n);
            return;
        }
    }
    /* %.17g always round-trips a double; reaching here means snprintf failed,
     * which cannot happen for a finite value. */
    t_str(t, "0");
}

/* C escapes, exactly like the reference printer (absl::CEscape): short
 * escapes only for tab/lf/cr, backslash escapes for both quotes, three-digit
 * octal for the rest. */
static void
t_quoted(text_out *t, const uint8_t *data, size_t len)
{
    size_t i;

    if (data == NULL) {
        if (len > 0) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        data = pbw_empty_input;
    }
    t_put(t, "\"", 1);
    for (i = 0; i < len; i++) {
        uint8_t c = data[i];

        if (c == '\t') {
            t_str(t, "\\t");
        } else if (c == '\n') {
            t_str(t, "\\n");
        } else if (c == '\r') {
            t_str(t, "\\r");
        } else if (c == '"') {
            t_str(t, "\\\"");
        } else if (c == '\\') {
            t_str(t, "\\\\");
        } else if (c == '\'') {
            t_str(t, "\\'");
        } else if (c >= 32 && c < 127) {
            t_put(t, &c, 1);
        } else {
            char tmp[5];

            tmp[0] = '\\';
            tmp[1] = (char)('0' + ((c >> 6) & 7));
            tmp[2] = (char)('0' + ((c >> 3) & 7));
            tmp[3] = (char)('0' + (c & 7));
            tmp[4] = '\0';
            t_put(t, tmp, 4);
        }
    }
    t_put(t, "\"", 1);
}

/* Short unqualified name: the reference prints groups by their type name. */
static const char *
short_name(const char *fq)
{
    const char *dot = strrchr(fq, '.');

    return dot == NULL ? fq : dot + 1;
}

static int
is_any_message(const pbw_descriptor *desc)
{
    return desc != NULL && !desc->is_enum &&
           strcmp(desc->name, "google.protobuf.Any") == 0;
}

static void t_print_message(text_out *t, const pbw_message *msg, uint32_t depth);

static void
t_print_scalar(text_out *t, const pbw_field *field, const pbw_value *value)
{
    if (t == NULL || field == NULL || value == NULL) {
        if (t != NULL && t->status == PBW_OK) {
            t->status = PBW_ERR_INVALID;
        }
        return;
    }
    if ((field->type == PBW_TYPE_STRING || field->type == PBW_TYPE_BYTES)) {
        /* checked in the case below */
    } else if (field->type == PBW_TYPE_MESSAGE || field->type == PBW_TYPE_GROUP) {
        if (value->kind != PBW_VALUE_MESSAGE) {
            t->status = PBW_ERR_INVALID;
            return;
        }
    } else {
        if (value->kind != PBW_VALUE_UINT) {
            t->status = PBW_ERR_INVALID;
            return;
        }
    }
    switch (field->type) {
    case PBW_TYPE_DOUBLE: {
        double d;

        memcpy(&d, &value->as.u64, sizeof(d));
        t_double(t, d, 0);
        return;
    }
    case PBW_TYPE_FLOAT: {
        /* Same mask as the JSON printer: the 4 float bytes are the low half
         * of the stored bit pattern, not its first 4 memory bytes. */
        uint32_t bits32 = (uint32_t)value->as.u64;
        float f;

        memcpy(&f, &bits32, sizeof(f));
        t_double(t, (double)f, 1);
        return;
    }
    case PBW_TYPE_BOOL:
        t_str(t, value->as.u64 != 0 ? "true" : "false");
        return;
    case PBW_TYPE_ENUM: {
        const char *name = NULL;

        if (field->sub != NULL &&
            pbw_enum_find_name(field->sub, (int32_t)value->as.u64, &name)) {
            t_str(t, name);
            return;
        }
        t_i64(t, (int64_t)(int32_t)value->as.u64);
        return;
    }
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
        if (value->kind != PBW_VALUE_BYTES) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        if (value->as.bytes.len > 0 && value->as.bytes.data == NULL) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        t_quoted(t, value->as.bytes.data, value->as.bytes.len);
        return;
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
        t_i64(t, (int64_t)(int32_t)value->as.u64);
        return;
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
        t_i64(t, (int64_t)value->as.u64);
        return;
    default:
        t_u64(t, value->as.u64);
        return;
    }
}

/* Tries to read `data` as a sequence of fields; 1 when it parses cleanly, so
 * the printer can show an unknown length-delimited value as a message. */
static int
unknown_is_message(const uint8_t *data, size_t len, uint32_t depth)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;

    if (len == 0 || depth >= PBW_MAX_DEPTH) {
        return 0;
    }
    while (p < end) {
        uint32_t tag;
        size_t used;
        uint32_t wire;

        if (!pbw_tag_read(p, end, &tag, &used)) {
            return 0;
        }
        p += used;
        wire = tag & 7;
        if ((tag >> 3) == 0 || wire == PBW_WIRE_END_GROUP ||
            wire > PBW_WIRE_32BIT) {
            return 0;
        }
        switch (wire) {
        case PBW_WIRE_VARINT: {
            uint64_t scratch;
            size_t n;

            if (!pbw_varint_read(p, end, &scratch, &n)) {
                return 0;
            }
            p += n;
            break;
        }
        case PBW_WIRE_64BIT:
            if (pbw_remaining((const uint8_t *)end, (const uint8_t *)p) < 8) {
                return 0;
            }
            p += 8;
            break;
        case PBW_WIRE_32BIT:
            if (pbw_remaining((const uint8_t *)end, (const uint8_t *)p) < 4) {
                return 0;
            }
            p += 4;
            break;
        case PBW_WIRE_LEN: {
            size_t ilen;
            size_t n;

            if (!pbw_len_read(p, end, &ilen, &n)) {
                return 0;
            }
            p += n;
            if (ilen > pbw_remaining((const uint8_t *)end, (const uint8_t *)p)) {
                return 0;
            }
            p += ilen;
            break;
        }
        case PBW_WIRE_START_GROUP: {
            /* skip to the matching END_GROUP, counting nesting */
            uint32_t number = tag >> 3;
            uint32_t nesting = 1;

            while (p < end && nesting > 0) {
                if (!pbw_tag_read(p, end, &tag, &used)) {
                    return 0;
                }
                p += used;
                wire = tag & 7;
                if ((tag >> 3) == 0 || wire > PBW_WIRE_32BIT) {
                    return 0;
                }
                if (wire == PBW_WIRE_START_GROUP) {
                    nesting++;
                } else if (wire == PBW_WIRE_END_GROUP) {
                    if (nesting == 1 && (tag >> 3) != number) {
                        return 0;
                    }
                    nesting--;
                } else if (wire == PBW_WIRE_VARINT) {
                    uint64_t scratch;
                    size_t n;

                    if (!pbw_varint_read(p, end, &scratch, &n)) {
                        return 0;
                    }
                    p += n;
                } else if (wire == PBW_WIRE_64BIT) {
                    if (pbw_remaining((const uint8_t *)end, (const uint8_t *)p) < 8) {
                        return 0;
                    }
                    p += 8;
                } else if (wire == PBW_WIRE_32BIT) {
                    if (pbw_remaining((const uint8_t *)end, (const uint8_t *)p) < 4) {
                        return 0;
                    }
                    p += 4;
                } else if (wire == PBW_WIRE_LEN) {
                    size_t ilen;
                    size_t n;

                    if (!pbw_len_read(p, end, &ilen, &n)) {
                        return 0;
                    }
                    p += n;
                    if (ilen > pbw_remaining((const uint8_t *)end, (const uint8_t *)p)) {
                        return 0;
                    }
                    p += ilen;
                } else {
                    return 0;
                }
            }
            if (nesting != 0) {
                return 0;
            }
            break;
        }
        default:
            return 0;
        }
    }
    return 1;
}

static void t_print_unknowns(text_out *t, const uint8_t *p,
                             const uint8_t *end, uint32_t depth);

/* Prints one unknown value; `p` points past its tag. Indentation comes from
 * t->indent, like known fields. */
static void
t_print_unknown_value(text_out *t, uint32_t number, uint32_t wire,
                      const uint8_t **pp, const uint8_t *end, uint32_t depth)
{
    const uint8_t *p = *pp;
    char num[24];
    int n;

    if (depth >= PBW_MAX_DEPTH) {
        t->status = PBW_ERR_LIMIT;
        return;
    }
    t_indent(t, 0);
    n = snprintf(num, sizeof(num), "%u", number);
    if (n > 0) {
        t_put(t, num, (size_t)n);
    }
    switch (wire) {
    case PBW_WIRE_VARINT: {
        uint64_t value;
        size_t used;

        if (!pbw_varint_read(p, end, &value, &used)) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        t_str(t, ": ");
        t_u64(t, value);
        t_str(t, "\n");
        *pp = p + used;
        return;
    }
    case PBW_WIRE_64BIT: {
        char tmp[24];
        uint64_t v = 0;
        int i;

        if (pbw_remaining((const uint8_t *)end, (const uint8_t *)p) < 8) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        /* value order, like the reference: lowest byte first on the wire */
        for (i = 7; i >= 0; i--) {
            v = v * 256 + p[i];
        }
        snprintf(tmp, sizeof(tmp), ": 0x%016llx\n",
                 (unsigned long long)v);
        t_str(t, tmp);
        *pp = p + 8;
        return;
    }
    case PBW_WIRE_32BIT: {
        char tmp[16];
        uint32_t v = 0;
        int i;

        if (pbw_remaining((const uint8_t *)end, (const uint8_t *)p) < 4) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        for (i = 3; i >= 0; i--) {
            v = v * 256 + p[i];
        }
        snprintf(tmp, sizeof(tmp), ": 0x%08x\n", v);
        t_str(t, tmp);
        *pp = p + 4;
        return;
    }
    case PBW_WIRE_LEN: {
        size_t len;
        size_t used;

        if (!pbw_len_read(p, end, &len, &used)) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        p += used;
        if (len > pbw_remaining((const uint8_t *)end, (const uint8_t *)p)) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        if (unknown_is_message(p, len, depth)) {
            t_str(t, " {\n");
            t->indent++;
            t_print_unknowns(t, p, p + len, depth + 1);
            t->indent--;
            t_indent(t, 0);
            t_str(t, "}\n");
        } else {
            t_str(t, ": ");
            t_quoted(t, p, len);
            t_str(t, "\n");
        }
        *pp = p + len;
        return;
    }
    case PBW_WIRE_START_GROUP: {
        /* find the matching END_GROUP first, then print the body */
        const uint8_t *body = p;
        const uint8_t *bend = NULL;
        uint32_t nesting = 1;
        const uint8_t *q = p;

        while (q < end && nesting > 0) {
            uint32_t tag;
            size_t used;

            if (!pbw_tag_read(q, end, &tag, &used)) {
                t->status = PBW_ERR_INVALID;
                return;
            }
            q += used;
            if ((tag & 7) == PBW_WIRE_START_GROUP) {
                nesting++;
            } else if ((tag & 7) == PBW_WIRE_END_GROUP) {
                nesting--;
                if (nesting == 0) {
                    if ((tag >> 3) != number) {
                        t->status = PBW_ERR_INVALID;
                        return;
                    }
                    bend = q - used;
                }
            } else {
                /* skip the value to find the group end */
                uint32_t w = tag & 7;
                if (w == PBW_WIRE_VARINT) {
                    uint64_t scratch;
                    size_t nn;

                    if (!pbw_varint_read(q, end, &scratch, &nn)) {
                        t->status = PBW_ERR_INVALID;
                        return;
                    }
                    q += nn;
                } else if (w == PBW_WIRE_64BIT || w == PBW_WIRE_32BIT) {
                    size_t need = w == PBW_WIRE_64BIT ? 8u : 4u;

                    /* Bounds-check before advancing: q + 8 past one-past-end
                     * is undefined even when nothing is read. */
                    if (pbw_remaining((const uint8_t *)end,
                                      (const uint8_t *)q) < need) {
                        t->status = PBW_ERR_INVALID;
                        return;
                    }
                    q += need;
                } else if (w == PBW_WIRE_LEN) {
                    size_t ilen;
                    size_t nn;

                    if (!pbw_len_read(q, end, &ilen, &nn)) {
                        t->status = PBW_ERR_INVALID;
                        return;
                    }
                    q += nn;
                    if (ilen > pbw_remaining((const uint8_t *)end,
                                             (const uint8_t *)q)) {
                        t->status = PBW_ERR_INVALID;
                        return;
                    }
                    q += ilen;
                } else {
                    t->status = PBW_ERR_INVALID;
                    return;
                }
            }
        }
        if (nesting != 0 || bend == NULL) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        t_str(t, " {\n");
        t->indent++;
        t_print_unknowns(t, body, bend, depth + 1);
        t->indent--;
        t_indent(t, 0);
        t_str(t, "}\n");
        *pp = q;
        return;
    }
    default:
        t->status = PBW_ERR_INVALID;
        return;
    }
}

static void
t_print_unknowns(text_out *t, const uint8_t *p, const uint8_t *end,
                 uint32_t depth)
{
    while (p < end && t->status == PBW_OK) {
        uint32_t tag;
        size_t used;

        if (!pbw_tag_read(p, end, &tag, &used)) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        p += used;
        t_print_unknown_value(t, tag >> 3, tag & 7, &p, end, depth);
    }
}

static void
t_print_field_value(text_out *t, const pbw_field *field,
                    const pbw_value *value, int indent, uint32_t depth)
{
    if (t == NULL || field == NULL || value == NULL) {
        if (t != NULL && t->status == PBW_OK) {
            t->status = PBW_ERR_INVALID;
        }
        return;
    }
    if (field->type == PBW_TYPE_MESSAGE || field->type == PBW_TYPE_GROUP) {
        if (value->kind != PBW_VALUE_MESSAGE || value->as.msg == NULL) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        if (depth >= PBW_MAX_DEPTH) {
            t->status = PBW_ERR_LIMIT;
            return;
        }
        t_str(t, " {\n");
        t->indent++;
        t_print_message(t, value->as.msg, depth + 1);
        t->indent--;
        t_indent(t, indent);
        t_str(t, "}\n");
        return;
    }
    t_str(t, ": ");
    t_print_scalar(t, field, value);
    t_str(t, "\n");
}

static void
t_print_field(text_out *t, const pbw_field *field, const pbw_value *value,
              uint32_t depth)
{
    size_t j;

    if (t == NULL || field == NULL || value == NULL) {
        if (t != NULL && t->status == PBW_OK) {
            t->status = PBW_ERR_INVALID;
        }
        return;
    }
    if (field->label == PBW_LABEL_REPEATED) {
        if (value->kind != PBW_VALUE_LIST) {
            return;
        }
        if (value->as.list.len > 0 && value->as.list.items == NULL) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        for (j = 0; j < value->as.list.len; j++) {
            if (value->as.list.items[j] == NULL) {
                t->status = PBW_ERR_INVALID;
                return;
            }
            t_indent(t, 0);
            if (field->is_extension && field->full_name != NULL) {
                t_put(t, "[", 1);
                t_str(t, field->full_name);
                t_put(t, "]", 1);
            } else if (field->is_extension) {
                t_put(t, "[", 1);
                t_str(t, field->name);
                t_put(t, "]", 1);
            } else if (field->type == PBW_TYPE_GROUP) {
                if (field->sub == NULL || field->sub->name == NULL) {
                    t->status = PBW_ERR_INVALID;
                    return;
                }
                t_str(t, short_name(field->sub->name));
            } else {
                t_str(t, field->name);
            }
            t_print_field_value(t, field, value->as.list.items[j], 0, depth);
            if (t->status != PBW_OK) {
                return;
            }
        }
        return;
    }
    t_indent(t, 0);
    if (field->is_extension && field->full_name != NULL) {
        t_put(t, "[", 1);
        t_str(t, field->full_name);
        t_put(t, "]", 1);
    } else if (field->is_extension) {
        t_put(t, "[", 1);
        t_str(t, field->name);
        t_put(t, "]", 1);
    } else if (field->type == PBW_TYPE_GROUP) {
        if (field->sub == NULL || field->sub->name == NULL) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        t_str(t, short_name(field->sub->name));
    } else {
        t_str(t, field->name);
    }
    t_print_field_value(t, field, value, 0, depth);
}

static void
t_print_message(text_out *t, const pbw_message *msg, uint32_t depth)
{
    size_t i;

    if (t == NULL || msg == NULL || msg->desc == NULL) {
        if (t != NULL && t->status == PBW_OK) {
            t->status = PBW_ERR_INVALID;
        }
        return;
    }
    if (msg->desc->field_count > 0 &&
        (msg->values == NULL || msg->present == NULL)) {
        t->status = PBW_ERR_INVALID;
        return;
    }
    if (t->status != PBW_OK) {
        return;
    }
    if (depth > PBW_MAX_DEPTH) {
        t->status = PBW_ERR_LIMIT;
        return;
    }
    /* Any values print as their raw type_url/value fields, like the reference
     * printer; the expanded form is only accepted on input. */
    for (i = 0; i < msg->desc->field_count; i++) {
        const pbw_field *field = &msg->desc->fields[i];

        if (!pbw_msg_present(msg, i)) {
            continue;
        }
        /* proto3 implicit presence: the reference printer omits defaults. */
        if (pbw_field_implicit_presence(field, msg->desc) &&
            pbw_value_is_default(&msg->values[i])) {
            continue;
        }
        t_print_field(t, field, &msg->values[i], depth);
        if (t->status != PBW_OK) {
            return;
        }
    }
    if (t->print_unknown && msg->unknown_len > 0) {
        if (msg->unknown == NULL) {
            t->status = PBW_ERR_INVALID;
            return;
        }
        t_print_unknowns(t, msg->unknown, msg->unknown + msg->unknown_len,
                         depth);
    }
}

pbw_status
pbw_text_print_opts(const pbw_message *msg, int print_unknown_fields,
                    char **out, size_t *len)
{
    text_out t;

    if (msg == NULL || out == NULL || len == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    *len = 0;
    pbw_buf_init(&t.buf);
    t.indent = 0;
    t.print_unknown = print_unknown_fields != 0;
    t.status = PBW_OK;
    t.dot = pbw_locale_dot();
    if (!pbw_buf_reserve(&t.buf, 64)) {
        return PBW_ERR_NOMEM;
    }
    t.buf.len = 0;
    t_print_message(&t, msg, 0);
    if (t.status != PBW_OK) {
        pbw_buf_free(&t.buf);
        return t.status;
    }
    if (!pbw_buf_reserve(&t.buf, 1)) {
        pbw_buf_free(&t.buf);
        return PBW_ERR_NOMEM;
    }
    t.buf.data[t.buf.len] = '\0';
    *out = (char *)t.buf.data;
    *len = t.buf.len;
    return PBW_OK;
}

pbw_status
pbw_text_print(const pbw_message *msg, char **out, size_t *len)
{
    /* same default as the reference printer: unknown fields are shown */
    return pbw_text_print_opts(msg, 1, out, len);
}

/* ------------------------------------------------------------------ parsing */

typedef struct {
    const char *p;
    const char *end;
} text_cur;

static void
t_skip(text_cur *c)
{
    for (;;) {
        while (c->p < c->end &&
               (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' ||
                *c->p == '\v' || *c->p == '\f' || *c->p == '\r')) {
            c->p++;
        }
        if (c->p < c->end && *c->p == '#') {
            while (c->p < c->end && *c->p != '\n') {
                c->p++;
            }
            continue;
        }
        return;
    }
}

static int
t_is_ident_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int
t_is_ident_char(char c)
{
    return t_is_ident_start(c) || (c >= '0' && c <= '9');
}

static int
t_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int
t_is_hex(char c)
{
    return t_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Parses an identifier; the span covers the name. */
static int
t_ident(text_cur *c, const char **start, size_t *len)
{
    const char *p;

    t_skip(c);
    p = c->p;
    if (p >= c->end || !t_is_ident_start(*p)) {
        return 0;
    }
    while (p < c->end && t_is_ident_char(*p)) {
        p++;
    }
    *start = c->p;
    *len = (size_t)(p - c->p);
    c->p = p;
    return 1;
}

typedef enum { T_NUM_INT,
               T_NUM_FLOAT,
               T_NUM_BAD } t_num_kind;

typedef struct {
    t_num_kind kind;
    int is_hex;
    int is_oct;
    const char *start;
    size_t len;
} t_number;

/* Lexes a number starting at a digit or a dot-followed-by-digit. Mirrors the
 * reference tokenizer: hex needs digits, octal rejects 8/9 later at value
 * time, `e` needs exponent digits, `f/F` suffix makes a float. */
static int
t_lex_number(text_cur *c, t_number *out)
{
    const char *p = c->p;

    out->is_hex = 0;
    out->is_oct = 0;
    if (p < c->end && *p == '0' && p + 1 < c->end &&
        (p[1] == 'x' || p[1] == 'X')) {
        const char *q = p + 2;

        while (q < c->end && t_is_hex(*q)) {
            q++;
        }
        if (q == p + 2) {
            return 0; /* "0x" without digits */
        }
        out->kind = T_NUM_INT;
        out->is_hex = 1;
        out->start = p;
        out->len = (size_t)(q - p);
        c->p = q;
        return 1;
    }
    {
        const char *q = p;
        int is_float = 0;

        while (q < c->end && t_is_digit(*q)) {
            q++;
        }
        if (p < c->end && *p == '0' && q > p + 1) {
            out->is_oct = 1;
        }
        if (q < c->end && *q == '.') {
            is_float = 1;
            q++;
            while (q < c->end && t_is_digit(*q)) {
                q++;
            }
        }
        if (q < c->end && (*q == 'e' || *q == 'E')) {
            const char *r = q + 1;

            if (r < c->end && (*r == '-' || *r == '+')) {
                r++;
            }
            if (r >= c->end || !t_is_digit(*r)) {
                /* "1e" without exponent: a float-shaped token that no value
                 * parser accepts, like the reference. */
                out->kind = T_NUM_FLOAT;
                out->start = p;
                out->len = (size_t)(q - p);
                c->p = q;
                return 1;
            }
            while (r < c->end && t_is_digit(*r)) {
                r++;
            }
            q = r;
            is_float = 1;
        }
        if (q < c->end && (*q == 'f' || *q == 'F')) {
            q++;
            is_float = 1;
        }
        if (q == p) {
            return 0;
        }
        out->kind = is_float ? T_NUM_FLOAT : T_NUM_INT;
        out->start = p;
        out->len = (size_t)(q - p);
        c->p = q;
        return 1;
    }
}

/* A letter right after a number is an error ("need space"), like the
 * reference tokenizer. */
static int
t_number_end(text_cur *c)
{
    if (c->p < c->end && t_is_ident_start(*c->p)) {
        return 0;
    }
    return 1;
}

static int
t_parse_number(text_cur *c, t_number *out)
{
    int ok;

    t_skip(c);
    if (c->p >= c->end) {
        return 0;
    }
    if (t_is_digit(*c->p)) {
        ok = t_lex_number(c, out);
        return ok && t_number_end(c);
    }
    if (*c->p == '.' && c->p + 1 < c->end && t_is_digit(c->p[1])) {
        /* leading-dot float, no hex/octal here */
        const char *p = c->p + 1;
        const char *q = p;

        while (q < c->end && t_is_digit(*q)) {
            q++;
        }
        if (q < c->end && (*q == 'e' || *q == 'E')) {
            const char *r = q + 1;

            if (r < c->end && (*r == '-' || *r == '+')) {
                r++;
            }
            if (r >= c->end || !t_is_digit(*r)) {
                out->kind = T_NUM_FLOAT;
                out->is_hex = 0;
                out->is_oct = 0;
                out->start = c->p;
                out->len = (size_t)(q - c->p);
                c->p = q;
                return 1;
            }
            while (r < c->end && t_is_digit(*r)) {
                r++;
            }
            q = r;
        }
        if (q < c->end && (*q == 'f' || *q == 'F')) {
            q++;
        }
        out->kind = T_NUM_FLOAT;
        out->is_hex = 0;
        out->is_oct = 0;
        out->start = c->p;
        out->len = (size_t)(q - c->p);
        c->p = q;
        return t_number_end(c);
    }
    return 0;
}

/* Parses one quoted string; the span includes the quotes. Adjacent strings
 * are concatenated by the caller. */
static int
t_parse_string(text_cur *c, const char **start, size_t *len)
{
    const char *p;
    char quote;

    t_skip(c);
    if (c->p >= c->end || (*c->p != '"' && *c->p != '\'')) {
        return 0;
    }
    quote = *c->p;
    p = c->p + 1;
    for (;;) {
        if (p >= c->end) {
            return 0; /* unterminated */
        }
        if (*p == '\0') {
            return 0; /* NUL ends input for the reference too */
        }
        if (*p == '\n') {
            return 0; /* no multiline strings */
        }
        if (*p == '\\') {
            p++;
            if (p >= c->end || *p == '\n' || *p == '\r') {
                return 0;
            }
            p++;
            continue;
        }
        if (*p == quote) {
            p++;
            break;
        }
        p++;
    }
    *start = c->p;
    *len = (size_t)(p - c->p);
    c->p = p;
    return 1;
}

static int
t_hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

/* Standard UTF-8 check, rejecting surrogates and overlong forms. */
static int
t_valid_utf8(const uint8_t *data, size_t len)
{
    size_t i = 0;

    while (i < len) {
        uint8_t c = data[i];

        if (c < 0x80) {
            i++;
        } else if ((c & 0xe0) == 0xc0) {
            if (c < 0xc2 || i + 1 >= len || (data[i + 1] & 0xc0) != 0x80) {
                return 0;
            }
            i += 2;
        } else if ((c & 0xf0) == 0xe0) {
            if (i + 2 >= len || (data[i + 1] & 0xc0) != 0x80 ||
                (data[i + 2] & 0xc0) != 0x80) {
                return 0;
            }
            {
                uint32_t v = ((uint32_t)(c & 15) << 12) |
                             ((uint32_t)(data[i + 1] & 63) << 6) |
                             (uint32_t)(data[i + 2] & 63);

                if (v < 0x800 || (v >= 0xd800 && v <= 0xdfff)) {
                    return 0;
                }
            }
            i += 3;
        } else if ((c & 0xf8) == 0xf0) {
            if (c > 0xf4 || i + 3 >= len || (data[i + 1] & 0xc0) != 0x80 ||
                (data[i + 2] & 0xc0) != 0x80 ||
                (data[i + 3] & 0xc0) != 0x80) {
                return 0;
            }
            {
                uint32_t v = ((uint32_t)(c & 7) << 18) |
                             ((uint32_t)(data[i + 1] & 63) << 12) |
                             ((uint32_t)(data[i + 2] & 63) << 6) |
                             (uint32_t)(data[i + 3] & 63);

                if (v < 0x10000 || v > 0x10ffff) {
                    return 0;
                }
            }
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

/* Appends the unescaped form of one quoted span. Surrogate halves and
 * out-of-range code points fail: the reference test suite requires it. */
static pbw_status
t_unescape(pbw_buf *out, const char *start, size_t len)
{
    /* start includes both quotes */
    const char *p = start + 1;
    const char *end = start + len - 1;

    while (p < end) {
        char c = *p++;

        if (c != '\\') {
            if (!pbw_buf_reserve(out, 1)) {
                return PBW_ERR_NOMEM;
            }
            out->data[out->len++] = (uint8_t)c;
            continue;
        }
        if (p >= end) {
            return PBW_ERR_INVALID;
        }
        c = *p++;
        if (c >= '0' && c <= '7') {
            unsigned v = (unsigned)(c - '0');
            int i;

            for (i = 0; i < 2 && p < end && *p >= '0' && *p <= '7'; i++) {
                v = v * 8 + (unsigned)(*p++ - '0');
            }
            /* values above 255 truncate, like the reference */
            if (!pbw_buf_reserve(out, 1)) {
                return PBW_ERR_NOMEM;
            }
            out->data[out->len++] = (uint8_t)v;
        } else if (c == 'x' || c == 'X') {
            unsigned v = 0;
            int digits = 0;

            while (digits < 2 && p < end && t_is_hex(*p)) {
                v = v * 16 + (unsigned)t_hex_val(*p++);
                digits++;
            }
            if (digits == 0) {
                return PBW_ERR_INVALID;
            }
            if (!pbw_buf_reserve(out, 1)) {
                return PBW_ERR_NOMEM;
            }
            out->data[out->len++] = (uint8_t)v;
        } else if (c == 'u' || c == 'U') {
            int want = c == 'u' ? 4 : 8;
            int i;
            uint32_t v = 0;

            for (i = 0; i < want; i++) {
                if (p >= end || !t_is_hex(*p)) {
                    return PBW_ERR_INVALID;
                }
                v = v * 16 + (uint32_t)t_hex_val(*p++);
            }
            if (c == 'U' && (start[0] != 0)) {
                /* the tokenizer already limits \U to 00[01]......; values
                 * above the planes and surrogates fail here */
            }
            if (c == 'u' && v >= 0xd800 && v <= 0xdbff && p + 5 < end &&
                p[0] == '\\' && p[1] == 'u') {
                /* a lead surrogate followed by \uXXXX: combine the pair
                 * into the scalar, like the reference; a lone surrogate
                 * stays invalid (bad UTF-8, which this parser rejects). */
                uint32_t trail = 0;
                int k;

                for (k = 0; k < 4; k++) {
                    if (!t_is_hex(p[2 + k])) {
                        break;
                    }
                    trail = trail * 16 + (uint32_t)t_hex_val(p[2 + k]);
                }
                if (k == 4 && trail >= 0xdc00 && trail <= 0xdfff) {
                    v = 0x10000u + ((v - 0xd800u) << 10) + (trail - 0xdc00u);
                    p += 6;
                }
            }
            if (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) {
                return PBW_ERR_INVALID;
            }
            {
                uint8_t tmp[4];
                size_t n = 0;

                if (v < 0x80) {
                    tmp[n++] = (uint8_t)v;
                } else if (v < 0x800) {
                    tmp[n++] = (uint8_t)(0xc0 | (v >> 6));
                    tmp[n++] = (uint8_t)(0x80 | (v & 63));
                } else if (v < 0x10000) {
                    tmp[n++] = (uint8_t)(0xe0 | (v >> 12));
                    tmp[n++] = (uint8_t)(0x80 | ((v >> 6) & 63));
                    tmp[n++] = (uint8_t)(0x80 | (v & 63));
                } else {
                    tmp[n++] = (uint8_t)(0xf0 | (v >> 18));
                    tmp[n++] = (uint8_t)(0x80 | ((v >> 12) & 63));
                    tmp[n++] = (uint8_t)(0x80 | ((v >> 6) & 63));
                    tmp[n++] = (uint8_t)(0x80 | (v & 63));
                }
                if (!pbw_buf_reserve(out, n)) {
                    return PBW_ERR_NOMEM;
                }
                memcpy(out->data + out->len, tmp, n);
                out->len += n;
            }
        } else {
            char e;

            switch (c) {
            case 'a':
                e = '\a';
                break;
            case 'b':
                e = '\b';
                break;
            case 'f':
                e = '\f';
                break;
            case 'n':
                e = '\n';
                break;
            case 'r':
                e = '\r';
                break;
            case 't':
                e = '\t';
                break;
            case 'v':
                e = '\v';
                break;
            case '\\':
                e = '\\';
                break;
            case '\'':
                e = '\'';
                break;
            case '"':
                e = '"';
                break;
            case '?':
                e = '?';
                break;
            default:
                return PBW_ERR_INVALID;
            }
            if (!pbw_buf_reserve(out, 1)) {
                return PBW_ERR_NOMEM;
            }
            out->data[out->len++] = (uint8_t)e;
        }
    }
    return PBW_OK;
}

/* Parses one unsigned magnitude with the reference tokenizer rules: hex with
 * 0x, octal with a leading 0 (8/9 are an error), decimal otherwise. */
static pbw_status
t_parse_uint(const t_number *num, uint64_t max, uint64_t *out)
{
    const char *p = num->start;
    size_t len = num->len;
    uint64_t value = 0;

    if (num->kind != T_NUM_INT) {
        return PBW_ERR_INVALID;
    }
    if (num->is_hex) {
        size_t i;

        p += 2;
        len -= 2;
        for (i = 0; i < len; i++) {
            unsigned d = (unsigned)t_hex_val(p[i]);

            if (value > max / 16 ||
                (value == max / 16 && d > max % 16)) {
                return PBW_ERR_INVALID;
            }
            value = value * 16 + d;
        }
        *out = value;
        return PBW_OK;
    }
    {
        size_t i;
        int base = num->is_oct ? 8 : 10;

        if (num->is_oct) {
            p += 1;
            len -= 1;
        }
        for (i = 0; i < len; i++) {
            unsigned d = (unsigned)(p[i] - '0');

            if (d >= (unsigned)base) {
                return PBW_ERR_INVALID;
            }
            if (value > max / (uint64_t)base ||
                (value == max / (uint64_t)base &&
                 d > max % (uint64_t)base)) {
                return PBW_ERR_INVALID;
            }
            value = value * (uint64_t)base + d;
        }
        *out = value;
        return PBW_OK;
    }
}

static pbw_status
t_parse_sint(const t_number *num, int negative, int64_t min, uint64_t max,
             int64_t *out)
{
    uint64_t limit = negative ? max + 1 : max;
    uint64_t mag;
    pbw_status status = t_parse_uint(num, limit, &mag);

    if (status != PBW_OK) {
        return status;
    }
    if (negative) {
        if (mag == max + 1) {
            *out = min;
            return PBW_OK;
        }
        *out = -(int64_t)mag;
        return PBW_OK;
    }
    *out = (int64_t)mag;
    return PBW_OK;
}

static int
t_ident_is(const char *start, size_t len, const char *word)
{
    size_t n = strlen(word);

    return len == n && memcmp(start, word, n) == 0;
}

/* Case-insensitive spelling match: the reference lowercases the identifier
 * before comparing, so `Inf`, `iNfinity` and `nAN` all parse. */
static int
t_ident_case_is(const char *start, size_t len, const char *word)
{
    size_t i;
    size_t n = strlen(word);

    if (len != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = start[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + ('a' - 'A'));
        }
        if (c != word[i]) {
            return 0;
        }
    }
    return 1;
}

typedef struct {
    text_cur cur;
    pbw_status status;
    int dot; /* the host locale's decimal point is '.' (checked once per call) */
} text_in;

/* Stores a scalar; repeated fields append. */
static pbw_status
t_store_uint(text_in *t, pbw_message *msg, const pbw_field *field,
             uint64_t bits)
{
    pbw_value *item;

    (void)t;

    if (field->label == PBW_LABEL_REPEATED) {
        item = pbw_value_new_uint_a(msg->arena, bits);
        if (item == NULL) {
            return PBW_ERR_NOMEM;
        }
        {
            pbw_value *list = pbw_message_list_for(msg, field);

            if (list == NULL || !pbw_value_list_push(list, item, msg->arena)) {
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

static pbw_status
t_parse_int_value(text_in *t, pbw_message *msg, const pbw_field *field)
{
    int negative = 0;
    t_number num;
    int64_t smin = 0;
    uint64_t umax = 0;
    int is_signed = 0;

    t_skip(&t->cur);
    if (t->cur.p < t->cur.end && *t->cur.p == '-') {
        negative = 1;
        t->cur.p++;
    }
    if (!t_parse_number(&t->cur, &num)) {
        return PBW_ERR_INVALID;
    }
    switch (field->type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
        smin = INT32_MIN;
        umax = INT32_MAX;
        is_signed = 1;
        break;
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
        smin = INT64_MIN;
        umax = INT64_MAX;
        is_signed = 1;
        break;
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32:
        umax = UINT32_MAX;
        break;
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64:
        umax = UINT64_MAX;
        break;
    case PBW_TYPE_BOOL: {
        uint64_t v;

        if (negative || t_parse_uint(&num, 1, &v) != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        return t_store_uint(t, msg, field, v);
    }
    default:
        return PBW_ERR_INVALID;
    }
    if (!is_signed && negative) {
        return PBW_ERR_INVALID;
    }
    if (is_signed) {
        int64_t v;

        if (t_parse_sint(&num, negative, smin, umax, &v) != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        /* like the binary decoder, sint values are stored decoded: the
         * zigzag only happens on the wire */
        return t_store_uint(t, msg, field, (uint64_t)v);
    }
    {
        uint64_t v;

        if (t_parse_uint(&num, umax, &v) != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        return t_store_uint(t, msg, field, v);
    }
}

static pbw_status
t_parse_float_value(text_in *t, pbw_message *msg, const pbw_field *field,
                    int is_float)
{
    double value = 0;
    int negative = 0;
    int have = 0;

    t_skip(&t->cur);
    if (t->cur.p < t->cur.end && *t->cur.p == '-') {
        negative = 1;
        t->cur.p++;
        t_skip(&t->cur);
    }
    if (t->cur.p < t->cur.end &&
        (t_is_digit(*t->cur.p) ||
         (*t->cur.p == '.' && t->cur.p + 1 < t->cur.end &&
          t_is_digit(t->cur.p[1])))) {
        t_number num;

        if (!t_parse_number(&t->cur, &num)) {
            return PBW_ERR_INVALID;
        }
        if (num.kind != T_NUM_FLOAT && (num.is_hex || num.is_oct)) {
            /* hex and octal float literals are invalid */
            return PBW_ERR_INVALID;
        }
        if (num.kind != T_NUM_FLOAT) {
            /* plain decimal integer: allowed; hex and octal are not */
            char stack[64];
            char *tmp = stack;
            char *endp;

            if (num.is_hex || num.is_oct) {
                return PBW_ERR_INVALID;
            }
            /* Numbers fit the stack buffer in practice; a longer one is unusual,
             * not invalid. */
            if (num.len >= sizeof(stack)) {
                if (num.len == SIZE_MAX || num.start == NULL) {
                    return PBW_ERR_INVALID;
                }
                tmp = (char *)malloc(num.len + 1);
                if (tmp == NULL) {
                    return PBW_ERR_NOMEM;
                }
            }
            memcpy(tmp, num.start, num.len);
            tmp[num.len] = '\0';
            value = pbw_strtod(tmp, &endp, t->dot);
            {
                int ok = endp != tmp;

                if (tmp != stack) {
                    free(tmp);
                }
                if (!ok) {
                    return PBW_ERR_INVALID;
                }
            }
        } else {
            char stack[64];
            char *tmp = stack;
            char *endp;
            size_t used;
            size_t want = num.len;

            if (num.len >= sizeof(stack)) {
                if (num.len == SIZE_MAX || num.start == NULL) {
                    return PBW_ERR_INVALID;
                }
                tmp = (char *)malloc(num.len + 1);
                if (tmp == NULL) {
                    return PBW_ERR_NOMEM;
                }
            }
            memcpy(tmp, num.start, num.len);
            /* strip a trailing f/F: strtod stops there anyway, but a bare
             * "1e" must fail instead of parsing as 1 */
            tmp[num.len] = '\0';
            value = pbw_strtod(tmp, &endp, t->dot);
            used = (size_t)(endp - tmp);
            if (want > 0 && (tmp[want - 1] == 'f' || tmp[want - 1] == 'F')) {
                want--;
            }
            if (tmp != stack) {
                free(tmp);
            }
            if (used != want) {
                return PBW_ERR_INVALID;
            }
        }
        have = 1;
    } else {
        const char *start;
        size_t len;

        if (!t_ident(&t->cur, &start, &len)) {
            return PBW_ERR_INVALID;
        }
        if (t_ident_case_is(start, len, "inf") ||
            t_ident_case_is(start, len, "infinity")) {
            value = HUGE_VAL;
            have = 1;
        } else if (t_ident_case_is(start, len, "nan")) {
            value = strtod("NAN", NULL);
            have = 1;
        } else {
            return PBW_ERR_INVALID;
        }
    }
    if (!have) {
        return PBW_ERR_INVALID;
    }
    if (negative) {
        value = -value;
    }
    if (is_float) {
        /* Through a uint32: a 4-byte copy into a uint64 fills its high half
         * on a big-endian host, where the float bits must live in the low. */
        uint32_t bits32;
        float f = (float)value;

        memcpy(&bits32, &f, sizeof(bits32));
        return t_store_uint(t, msg, field, bits32);
    } else {
        uint64_t bits = 0;

        memcpy(&bits, &value, sizeof(value));
        return t_store_uint(t, msg, field, bits);
    }
}

static pbw_status
t_parse_bool_value(text_in *t, pbw_message *msg, const pbw_field *field)
{
    t_skip(&t->cur);
    if (t->cur.p < t->cur.end &&
        (t_is_digit(*t->cur.p) || *t->cur.p == '-')) {
        return t_parse_int_value(t, msg, field);
    } else {
        const char *start;
        size_t len;

        if (!t_ident(&t->cur, &start, &len)) {
            return PBW_ERR_INVALID;
        }
        if (t_ident_is(start, len, "true") || t_ident_is(start, len, "True") ||
            t_ident_is(start, len, "t")) {
            return t_store_uint(t, msg, field, 1);
        }
        if (t_ident_is(start, len, "false") ||
            t_ident_is(start, len, "False") || t_ident_is(start, len, "f")) {
            return t_store_uint(t, msg, field, 0);
        }
        return PBW_ERR_INVALID;
    }
}

static pbw_status
t_parse_enum_value(text_in *t, pbw_message *msg, const pbw_field *field)
{
    int32_t number;

    t_skip(&t->cur);
    if (t->cur.p < t->cur.end &&
        (t_is_digit(*t->cur.p) || *t->cur.p == '-')) {
        int negative = 0;
        t_number num;
        int64_t v;

        if (*t->cur.p == '-') {
            negative = 1;
            t->cur.p++;
        }
        if (!t_parse_number(&t->cur, &num)) {
            return PBW_ERR_INVALID;
        }
        if (t_parse_sint(&num, negative, INT32_MIN, INT32_MAX, &v) != PBW_OK) {
            return PBW_ERR_INVALID;
        }
        number = (int32_t)v;
    } else {
        const char *start;
        size_t len;
        char name[128];

        if (!t_ident(&t->cur, &start, &len)) {
            return PBW_ERR_INVALID;
        }
        if (len >= sizeof(name)) {
            return PBW_ERR_INVALID;
        }
        memcpy(name, start, len);
        name[len] = '\0';
        if (field->sub == NULL ||
            !pbw_enum_find_value(field->sub, name, &number)) {
            return PBW_ERR_INVALID;
        }
        return t_store_uint(t, msg, field, (uint64_t)(int64_t)number);
    }
    if (field->sub != NULL &&
        pbw_enum_find_name(field->sub, number, NULL)) {
        return t_store_uint(t, msg, field, (uint64_t)(int64_t)number);
    }
    /* unknown numbers are kept only by open enums, like on the wire and in
     * the JSON mapping. Closed-ness can come from the enum type's dialect or
     * from an editions per-field override, so ask the shared predicate. */
    if (field->sub == NULL || pbw_enum_is_closed(field)) {
        return PBW_ERR_INVALID;
    }
    return t_store_uint(t, msg, field, (uint64_t)(int64_t)number);
}

static pbw_status
t_parse_string_value(text_in *t, pbw_message *msg, const pbw_field *field)
{
    pbw_buf out;
    pbw_status status;
    const char *start;
    size_t len;

    pbw_buf_init(&out);
    for (;;) {
        if (!t_parse_string(&t->cur, &start, &len)) {
            pbw_buf_free(&out);
            return PBW_ERR_INVALID;
        }
        status = t_unescape(&out, start, len);
        if (status != PBW_OK) {
            pbw_buf_free(&out);
            return status;
        }
        /* adjacent literals concatenate */
        {
            text_cur save = t->cur;

            t_skip(&t->cur);
            if (t->cur.p >= t->cur.end || (*t->cur.p != '"' && *t->cur.p != '\'')) {
                t->cur = save;
                break;
            }
        }
    }
    if (field->type == PBW_TYPE_STRING &&
        !t_valid_utf8(out.data == NULL ? (const uint8_t *)"" : out.data,
                      out.len)) {
        pbw_buf_free(&out);
        return PBW_ERR_INVALID;
    }
    if (field->label == PBW_LABEL_REPEATED) {
        pbw_value *item = pbw_value_new_bytes_a(
            msg->arena, out.data == NULL ? (const uint8_t *)"" : out.data,
            out.len);

        pbw_buf_free(&out);
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

        if (!pbw_value_init_bytes(
                &value, msg->arena,
                out.data == NULL ? (const uint8_t *)"" : out.data,
                out.len)) {
            pbw_buf_free(&out);
            return PBW_ERR_NOMEM;
        }
        pbw_buf_free(&out);
        pbw_message_put(msg, (size_t)(field - msg->desc->fields), &value);
        return PBW_OK;
    }
}

/* Parses one scalar (non-message) value of `field`. */
static pbw_status
t_parse_scalar_value(text_in *t, pbw_message *msg, const pbw_field *field)
{
    switch (field->type) {
    case PBW_TYPE_DOUBLE:
        return t_parse_float_value(t, msg, field, 0);
    case PBW_TYPE_FLOAT:
        return t_parse_float_value(t, msg, field, 1);
    case PBW_TYPE_BOOL:
        return t_parse_bool_value(t, msg, field);
    case PBW_TYPE_ENUM:
        return t_parse_enum_value(t, msg, field);
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
        return t_parse_string_value(t, msg, field);
    default:
        return t_parse_int_value(t, msg, field);
    }
}

/* Skips one value whose type we can only guess (reserved field names). */
static pbw_status
t_skip_value(text_in *t, uint32_t depth)
{
    t_skip(&t->cur);
    if (t->cur.p >= t->cur.end) {
        return PBW_ERR_INVALID;
    }
    if (*t->cur.p == '{' || *t->cur.p == '<') {
        char closer = *t->cur.p == '{' ? '}' : '>';
        pbw_status status;

        if (depth >= PBW_MAX_DEPTH) {
            return PBW_ERR_LIMIT;
        }
        t->cur.p++;
        /* skip nested fields without storing anything */
        for (;;) {
            t_skip(&t->cur);
            if (t->cur.p >= t->cur.end) {
                return PBW_ERR_INVALID;
            }
            if (*t->cur.p == closer) {
                t->cur.p++;
                return PBW_OK;
            }
            /* field name or [extension] */
            if (*t->cur.p == '[') {
                int d = 1;

                t->cur.p++;
                while (t->cur.p < t->cur.end && d > 0) {
                    if (*t->cur.p == '[') {
                        d++;
                    } else if (*t->cur.p == ']') {
                        d--;
                    }
                    t->cur.p++;
                }
                if (d != 0) {
                    return PBW_ERR_INVALID;
                }
            } else {
                const char *start;
                size_t len;

                if (!t_ident(&t->cur, &start, &len)) {
                    return PBW_ERR_INVALID;
                }
            }
            t_skip(&t->cur);
            if (t->cur.p < t->cur.end &&
                (*t->cur.p == '{' || *t->cur.p == '<')) {
                status = t_skip_value(t, depth + 1);
            } else {
                if (t->cur.p >= t->cur.end || *t->cur.p != ':') {
                    return PBW_ERR_INVALID;
                }
                t->cur.p++;
                status = t_skip_value(t, depth + 1);
            }
            if (status != PBW_OK) {
                return status;
            }
            t_skip(&t->cur);
            if (t->cur.p < t->cur.end &&
                (*t->cur.p == ',' || *t->cur.p == ';')) {
                t->cur.p++;
            }
        }
    }
    if (*t->cur.p == '[') {
        /* a list: skip comma-separated values */
        if (depth >= PBW_MAX_DEPTH) {
            return PBW_ERR_LIMIT;
        }
        t->cur.p++;
        t_skip(&t->cur);
        if (t->cur.p < t->cur.end && *t->cur.p == ']') {
            t->cur.p++;
            return PBW_OK;
        }
        for (;;) {
            pbw_status status = t_skip_value(t, depth + 1);

            if (status != PBW_OK) {
                return status;
            }
            t_skip(&t->cur);
            if (t->cur.p < t->cur.end && *t->cur.p == ',') {
                t->cur.p++;
                continue;
            }
            if (t->cur.p < t->cur.end && *t->cur.p == ']') {
                t->cur.p++;
                return PBW_OK;
            }
            return PBW_ERR_INVALID;
        }
    }
    {
        const char *start;
        size_t len;
        t_number num;

        if (t_parse_string(&t->cur, &start, &len)) {
            /* adjacent strings: keep skipping them as one value */
            for (;;) {
                text_cur save = t->cur;

                t_skip(&t->cur);
                if (t->cur.p >= t->cur.end ||
                    (*t->cur.p != '"' && *t->cur.p != '\'')) {
                    t->cur = save;
                    break;
                }
                if (!t_parse_string(&t->cur, &start, &len)) {
                    return PBW_ERR_INVALID;
                }
            }
            return PBW_OK;
        }
        if (t->cur.p < t->cur.end && *t->cur.p == '-') {
            t->cur.p++;
            t_skip(&t->cur);
        }
        if (t_parse_number(&t->cur, &num)) {
            return PBW_OK;
        }
        if (t_ident(&t->cur, &start, &len)) {
            return PBW_OK;
        }
        return PBW_ERR_INVALID;
    }
}

static int
t_is_url_char(char c)
{
    return t_is_ident_char(c) || c == '-' || c == '.' || c == '~' || c == '!' ||
           c == '$' || c == '&' || c == '(' || c == ')' || c == '*' ||
           c == '+' || c == ',' || c == ';' || c == '=' || c == '%' ||
           c == '/';
}

/* Collects raw bracket contents, skipping whitespace and comments. Only the
 * reference tokenizer's URL characters accumulate; anything else ends the
 * collection and fails at the `]` check, like the reference. */
static pbw_status
t_parse_brackets(text_in *t, pbw_buf *out)
{
    pbw_buf_init(out);
    t_skip(&t->cur);
    if (t->cur.p >= t->cur.end || *t->cur.p != '[') {
        return PBW_ERR_INVALID;
    }
    t->cur.p++;
    for (;;) {
        t_skip(&t->cur);
        if (t->cur.p >= t->cur.end) {
            pbw_buf_free(out);
            return PBW_ERR_INVALID;
        }
        if (*t->cur.p == ']') {
            t->cur.p++;
            if (!pbw_buf_reserve(out, 1)) {
                pbw_buf_free(out);
                return PBW_ERR_NOMEM;
            }
            out->data[out->len] = '\0';
            return PBW_OK;
        }
        if (!t_is_url_char(*t->cur.p)) {
            pbw_buf_free(out);
            return PBW_ERR_INVALID;
        }
        if (!pbw_buf_reserve(out, 1)) {
            pbw_buf_free(out);
            return PBW_ERR_NOMEM;
        }
        out->data[out->len++] = (uint8_t)*t->cur.p++;
    }
}

/* Dotted identifiers only ("a.b.C"), no slashes. */
static int
t_valid_type_name(const char *s)
{
    const char *p = s;

    if (*p == '\0') {
        return 0;
    }
    for (;;) {
        if (!t_is_ident_start(*p)) {
            return 0;
        }
        while (t_is_ident_char(*p)) {
            p++;
        }
        if (*p == '\0') {
            return 1;
        }
        if (*p != '.') {
            return 0;
        }
        p++;
    }
}

static pbw_status t_parse_message_body(text_in *t, pbw_message *msg,
                                       uint32_t depth, char closer);
static pbw_status t_parse_list(text_in *t, pbw_message *msg,
                               const pbw_field *field, uint32_t depth);

/* Any expansion when the parser already consumed nothing: reads [url]{...}
 * and fills the Any message `msg` itself (no field: the message IS the Any). */
static pbw_status
t_parse_any_inner(text_in *t, pbw_message *msg, uint32_t depth)
{
    pbw_buf raw;
    pbw_status status;
    const char *slash;
    const char *type_name;
    const pbw_descriptor *desc;
    pbw_message *inner = NULL;
    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    size_t i;

    status = t_parse_brackets(t, &raw);
    if (status != PBW_OK) {
        return status;
    }
    slash = NULL;
    for (i = 0; i < raw.len; i++) {
        if (raw.data[i] == '/') {
            slash = (const char *)raw.data + i;
        }
    }
    if (slash == NULL || slash == (const char *)raw.data ||
        ((const char *)raw.data)[0] == '/') {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    for (i = 0; (const char *)raw.data + i < slash; i++) {
        if (raw.data[i] == '%') {
            if (i + 2 >= (size_t)(slash - (const char *)raw.data) ||
                !t_is_hex((char)raw.data[i + 1]) ||
                !t_is_hex((char)raw.data[i + 2])) {
                pbw_buf_free(&raw);
                return PBW_ERR_INVALID;
            }
            i += 2;
        }
    }
    type_name = slash + 1;
    if (!t_valid_type_name(type_name)) {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    if (msg->desc->schema == NULL ||
        (desc = pbw_schema_find(msg->desc->schema, type_name)) == NULL ||
        desc->is_enum) {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    t_skip(&t->cur);
    if (t->cur.p < t->cur.end && *t->cur.p == ':') {
        t->cur.p++;
    }
    t_skip(&t->cur);
    if (t->cur.p >= t->cur.end ||
        (*t->cur.p != '{' && *t->cur.p != '<')) {
        pbw_buf_free(&raw);
        return PBW_ERR_INVALID;
    }
    inner = pbw_message_new(desc);
    if (inner == NULL) {
        pbw_buf_free(&raw);
        return PBW_ERR_NOMEM;
    }
    {
        char closer = *t->cur.p == '{' ? '}' : '>';

        t->cur.p++;
        status = t_parse_message_body(t, inner, depth + 1, closer);
    }
    if (status != PBW_OK) {
        pbw_message_free(inner);
        pbw_buf_free(&raw);
        return status;
    }
    status = pbw_encode(inner, &bytes, &bytes_len);
    pbw_message_free(inner);
    if (status != PBW_OK) {
        pbw_buf_free(&raw);
        return status;
    }
    {
        const pbw_field *url_f = pbw_descriptor_find_field(msg->desc, 1);
        const pbw_field *val_f = pbw_descriptor_find_field(msg->desc, 2);
        pbw_value *url_v;
        pbw_value *val_v;

        if (url_f == NULL || val_f == NULL) {
            pbw_buf_free(&raw);
            pbw_free(bytes);
            return PBW_ERR_INVALID;
        }
        url_v = pbw_value_new_string((const char *)raw.data);
        val_v = pbw_value_new_bytes(bytes, bytes_len);
        pbw_buf_free(&raw);
        pbw_free(bytes);
        if (url_v == NULL || val_v == NULL) {
            pbw_value_free(url_v);
            pbw_value_free(val_v);
            return PBW_ERR_NOMEM;
        }
        /* set() owns the value on success only, so each one is released on
         * its own failure path: freeing url_v after a successful set would
         * touch memory the message already owns. */
        status = pbw_message_set(msg, 1, url_v);
        if (status != PBW_OK) {
            pbw_value_free(url_v);
            pbw_value_free(val_v);
        } else {
            status = pbw_message_set(msg, 2, val_v);
            if (status != PBW_OK) {
                pbw_value_free(val_v);
            }
        }
        if (status != PBW_OK) {
            /* like the message body above: malformed content is
             * INVALID, only resource failures propagate */
            return status == PBW_ERR_LIMIT || status == PBW_ERR_NOMEM
                       ? status
                       : PBW_ERR_INVALID;
        }
        return PBW_OK;
    }
}

static pbw_status
t_parse_message_body(text_in *t, pbw_message *msg, uint32_t depth,
                     char closer);

/* Parses a message-typed value: `{...}` or `<...>`, merging into an existing
 * singular submessage like the binary decoder does. */
static pbw_status
t_parse_message_value(text_in *t, pbw_message *msg, const pbw_field *field,
                      uint32_t depth)
{
    char closer;
    pbw_message *slot;

    if (depth >= PBW_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    t_skip(&t->cur);
    if (t->cur.p < t->cur.end && *t->cur.p == ':') {
        t->cur.p++;
        t_skip(&t->cur);
    }
    /* Like the reference, a repeated message or group also accepts the
     * list form (`subs [{...}, ...]`); a singular one never does. */
    if (field->label == PBW_LABEL_REPEATED &&
        t->cur.p < t->cur.end && *t->cur.p == '[') {
        return t_parse_list(t, msg, field, depth);
    }
    if (t->cur.p >= t->cur.end ||
        (*t->cur.p != '{' && *t->cur.p != '<')) {
        return PBW_ERR_INVALID;
    }
    closer = *t->cur.p == '{' ? '}' : '>';
    t->cur.p++;
    if (field->label == PBW_LABEL_REPEATED) {
        slot = pbw_message_new(field->sub);
        if (slot == NULL) {
            return PBW_ERR_NOMEM;
        }
        {
            pbw_status status = t_parse_message_body(t, slot, depth + 1,
                                                     closer);

            if (status != PBW_OK) {
                pbw_message_free(slot);
                return status;
            }
        }
        {
            pbw_value *item = pbw_value_new_message(slot);

            if (item == NULL) {
                pbw_message_free(slot);
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
    }
    slot = pbw_message_set_message(msg, field->number);
    if (slot == NULL) {
        return PBW_ERR_INVALID;
    }
    return t_parse_message_body(t, slot, depth + 1, closer);
}

/* Parses a `[a, b]` list for a repeated scalar field. */
static pbw_status
t_parse_list(text_in *t, pbw_message *msg, const pbw_field *field,
             uint32_t depth)
{
    t_skip(&t->cur);
    if (t->cur.p >= t->cur.end || *t->cur.p != '[') {
        return PBW_ERR_INVALID;
    }
    t->cur.p++;
    t_skip(&t->cur);
    if (t->cur.p < t->cur.end && *t->cur.p == ']') {
        t->cur.p++;
        return PBW_OK; /* empty list */
    }
    for (;;) {
        pbw_status status;

        if (field->type == PBW_TYPE_MESSAGE || field->type == PBW_TYPE_GROUP) {
            /* embedded messages in a list parse like standalone values */
            t_skip(&t->cur);
            if (t->cur.p >= t->cur.end ||
                (*t->cur.p != '{' && *t->cur.p != '<')) {
                return PBW_ERR_INVALID;
            }
            {
                char closer = *t->cur.p == '{' ? '}' : '>';

                t->cur.p++;
                {
                    pbw_message *slot = pbw_message_new(field->sub);

                    if (slot == NULL) {
                        return PBW_ERR_NOMEM;
                    }
                    status = t_parse_message_body(t, slot, depth + 1,
                                                  closer);
                    if (status != PBW_OK) {
                        pbw_message_free(slot);
                        return status;
                    }
                    {
                        pbw_value *item = pbw_value_new_message(slot);

                        if (item == NULL) {
                            pbw_message_free(slot);
                            return PBW_ERR_NOMEM;
                        }
                        {
                            pbw_value *list =
                                pbw_message_list_for(msg, field);

                            if (list == NULL ||
                                !pbw_value_list_push(list, item,
                                                     msg->arena)) {
                                pbw_value_free(item);
                                return PBW_ERR_NOMEM;
                            }
                        }
                    }
                }
            }
        } else {
            /* adjacent string literals already concatenated into one
             * value inside the scalar parser */
            status = t_parse_scalar_value(t, msg, field);
            if (status != PBW_OK) {
                return status;
            }
        }
        t_skip(&t->cur);
        if (t->cur.p < t->cur.end && *t->cur.p == ',') {
            t->cur.p++;
            t_skip(&t->cur);
            if (t->cur.p < t->cur.end && *t->cur.p == ']') {
                return PBW_ERR_INVALID; /* trailing comma */
            }
            continue;
        }
        if (t->cur.p < t->cur.end && *t->cur.p == ']') {
            t->cur.p++;
            return PBW_OK;
        }
        return PBW_ERR_INVALID;
    }
}

static pbw_status
t_parse_message_body(text_in *t, pbw_message *msg, uint32_t depth,
                     char closer)
{
    if (depth > PBW_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    for (;;) {
        const char *name = NULL;
        size_t name_len = 0;
        const pbw_field *field = NULL;
        int is_bracket = 0;
        pbw_status status;

        t_skip(&t->cur);
        if (t->cur.p >= t->cur.end) {
            return closer == 0 ? PBW_OK : PBW_ERR_INVALID;
        }
        if (closer != 0 && *t->cur.p == closer) {
            t->cur.p++;
            return PBW_OK;
        }
        if (*t->cur.p == '[') {
            is_bracket = 1;
        } else if (!t_ident(&t->cur, &name, &name_len)) {
            return PBW_ERR_INVALID;
        }
        if (is_bracket) {
            /* Any expansion only applies inside google.protobuf.Any;
             * everywhere else brackets hold an extension name. */
            if (is_any_message(msg->desc)) {
                const pbw_field *url_f =
                    pbw_descriptor_find_field(msg->desc, 1);
                /* like the reference, a second expansion is an error */
                if (url_f != NULL &&
                    pbw_msg_present(msg,
                                    (size_t)(url_f - msg->desc->fields))) {
                    return PBW_ERR_INVALID;
                }
                status = t_parse_any_inner(t, msg, depth);
                if (status != PBW_OK) {
                    return status;
                }
                t_skip(&t->cur);
                if (t->cur.p < t->cur.end &&
                    (*t->cur.p == ',' || *t->cur.p == ';')) {
                    t->cur.p++;
                }
                continue;
            }
            {
                pbw_buf raw;
                const pbw_field *ext;

                status = t_parse_brackets(t, &raw);
                if (status != PBW_OK) {
                    return status;
                }
                {
                    size_t i;

                    for (i = 0; i < raw.len; i++) {
                        if (raw.data[i] == '/') {
                            pbw_buf_free(&raw);
                            return PBW_ERR_INVALID;
                        }
                    }
                }
                if (!t_valid_type_name((const char *)raw.data)) {
                    pbw_buf_free(&raw);
                    return PBW_ERR_INVALID;
                }
                ext = msg->desc->schema == NULL
                          ? NULL
                          : pbw_schema_find_extension(
                                msg->desc->schema, (const char *)raw.data);
                pbw_buf_free(&raw);
                if (ext == NULL) {
                    return PBW_ERR_INVALID;
                }
                /* the extension must belong to this message */
                {
                    const pbw_field *own =
                        pbw_descriptor_find_field(msg->desc, ext->number);

                    if (own != ext) {
                        return PBW_ERR_INVALID;
                    }
                }
                field = ext;
            }
        } else {
            char tmp[256];

            if (name_len >= sizeof(tmp)) {
                return PBW_ERR_INVALID;
            }
            memcpy(tmp, name, name_len);
            tmp[name_len] = '\0';
            field = pbw_descriptor_find_field_name(msg->desc, tmp);
            /* extensions are only addressable as [full.name] */
            if (field != NULL && field->is_extension) {
                field = NULL;
            }
            if (field == NULL) {
                /* groups also accept their lowercase field name when it
                 * matches the lowercased type name (the protoc
                 * convention), or their type name outright (the printer
                 * emits it, so the parse must round-trip whatever the
                 * field is called) */
                size_t i;
                char lower[256];

                for (i = 0; i < name_len; i++) {
                    char c = tmp[i];

                    lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
                }
                lower[name_len] = '\0';
                field = pbw_descriptor_find_field_name(msg->desc, lower);
                if (field != NULL &&
                    (field->is_extension ||
                     field->type != PBW_TYPE_GROUP || field->sub == NULL ||
                     strcmp(short_name(field->sub->name), tmp) != 0)) {
                    field = NULL;
                }
                for (i = 0; field == NULL && i < msg->desc->field_count;
                     i++) {
                    const pbw_field *g = &msg->desc->fields[i];
                    const char *short_sub;

                    if (g->is_extension || g->type != PBW_TYPE_GROUP ||
                        g->sub == NULL) {
                        continue;
                    }
                    short_sub = short_name(g->sub->name);
                    if (strlen(short_sub) == name_len &&
                        memcmp(short_sub, name, name_len) == 0) {
                        field = g;
                        break;
                    }
                }
                if (field == NULL) {
                    if (pbw_descriptor_is_reserved(msg->desc, tmp)) {
                        /* reserved names are skipped by guessing the value */
                        t_skip(&t->cur);
                        if (t->cur.p < t->cur.end &&
                            (*t->cur.p == '{' || *t->cur.p == '<')) {
                            status = t_skip_value(t, depth);
                        } else {
                            if (t->cur.p >= t->cur.end || *t->cur.p != ':') {
                                return PBW_ERR_INVALID;
                            }
                            t->cur.p++;
                            status = t_skip_value(t, depth);
                        }
                        if (status != PBW_OK) {
                            return status;
                        }
                        t_skip(&t->cur);
                        if (t->cur.p < t->cur.end &&
                            (*t->cur.p == ',' || *t->cur.p == ';')) {
                            t->cur.p++;
                        }
                        continue;
                    }
                    return PBW_ERR_INVALID;
                }
            }
        }
        /* like the reference parser, singular fields and oneof members may
         * only be specified once (no last-wins, no message merge) */
        if (field->label != PBW_LABEL_REPEATED) {
            size_t index = (size_t)(field - msg->desc->fields);

            if (pbw_msg_present(msg, index)) {
                return PBW_ERR_INVALID;
            }
            if (field->oneof_index != 0 &&
                msg->oneof_case[field->oneof_index] != 0) {
                return PBW_ERR_INVALID;
            }
        }
        if (field->type == PBW_TYPE_MESSAGE || field->type == PBW_TYPE_GROUP) {
            status = t_parse_message_value(t, msg, field, depth);
        } else {
            /* scalar: a colon is required */
            t_skip(&t->cur);
            if (t->cur.p >= t->cur.end || *t->cur.p != ':') {
                return PBW_ERR_INVALID;
            }
            t->cur.p++;
            if (field->label == PBW_LABEL_REPEATED) {
                text_cur save = t->cur;

                t_skip(&t->cur);
                if (t->cur.p < t->cur.end && *t->cur.p == '[') {
                    t->cur = save;
                    status = t_parse_list(t, msg, field, depth);
                } else {
                    t->cur = save;
                    status = t_parse_scalar_value(t, msg, field);
                }
            } else {
                status = t_parse_scalar_value(t, msg, field);
            }
        }
        if (status != PBW_OK) {
            return status;
        }
        t_skip(&t->cur);
        if (t->cur.p < t->cur.end && (*t->cur.p == ',' || *t->cur.p == ';')) {
            t->cur.p++;
        }
    }
}

pbw_status
pbw_text_parse(const pbw_descriptor *desc, const char *text, size_t len,
               pbw_message **out)
{
    text_in t;
    pbw_message *msg;
    pbw_status status;

    if (out != NULL) {
        *out = NULL;
    }
    if (desc == NULL || out == NULL || desc->is_enum) {
        return PBW_ERR_INVALID;
    }
    if (text == NULL && len > 0) {
        return PBW_ERR_INVALID;
    }
    if (desc->schema != NULL && !desc->schema->linked) {
        return PBW_ERR_INVALID;
    }
    msg = pbw_message_new(desc);
    if (msg == NULL) {
        return PBW_ERR_NOMEM;
    }
    t.cur.p = text == NULL ? "" : text;
    t.cur.end = t.cur.p + len;
    t.status = PBW_OK;
    t.dot = pbw_locale_dot();
    status = t_parse_message_body(&t, msg, 0, 0);
    if (status != PBW_OK) {
        pbw_message_free(msg);
        return status;
    }
    /* a missing proto2 `required` field fails, like the binary decoder */
    if (desc->has_required && !pbw_message_is_initialized(msg)) {
        pbw_message_free(msg);
        return PBW_ERR_REQUIRED;
    }
    *out = msg;
    return PBW_OK;
}
