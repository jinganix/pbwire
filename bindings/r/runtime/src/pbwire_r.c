/* R binding: named lists <-> wire bytes over the pbwire C core.
 *
 * A FileDescriptorSet (protoc --descriptor_set_out) loaded at run time is the
 * schema, so field numbers, types and presence rules are the descriptor's,
 * never a hand-written copy. No plugin, no generated code, no second wire
 * implementation.
 *
 * Messages are named R lists, which is what RProtoBuf users already pass
 * around:
 *
 *   schema <- pbwire_loadfile("sample.desc")
 *   wire <- pbwire_encode(schema, "pbwire.lua.Sample", list(name = "alice"))
 *   back <- pbwire_decode(schema, "pbwire.lua.Sample", wire)
 *
 * Hot loops use a builder instead (build once, encode per send):
 *
 *   m <- pbwire_message(schema, "pbwire.lua.Sample")
 *   pbwire_msg_set(m, list(name = "alice"))
 *   w1 <- pbwire_msg_encode(m)
 *
 * The schema owns one pbw_ctx (arena + encode buffer, reset per call), so
 * steady use stops calling malloc: list encode/decode converts through the
 * arena, builders own a plain tree each. A builder preserves its schema
 * SEXP, so collecting the schema never dangles it. R is single-threaded,
 * so no reentrant call can reset the arena mid-use.
 *
 * Value mapping: signed integers and enums <-> integer vectors, 64-bit and
 * unsigned integers <-> numeric (double), bool <-> logical, float/double <->
 * numeric, string <-> string, bytes <-> raw, message <-> named list,
 * repeated <-> atomic vector (scalars) or list (messages, bytes). An absent
 * singular field decodes as a missing list element (NULL); an absent
 * repeated field decodes as a zero-length vector/list. Groups map like
 * messages (START_GROUP framing on the wire); maps
 * are lists of list(key, value) entries.
 *
 * shortcut: doubles hold integers exactly only to 2^53, so int64/uint64
 * past that round-trip approximately (uint64 max reads as 2^64) and
 * out-of-range inputs clamp; INT32_MIN reads back as NA (it is NA_INTEGER).
 */
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

#include <pbwire.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* == PBW_MAX_DEPTH (include/pbwire.h): nesting cap the decoder enforces. */
#define PBWR_MAX_DEPTH 256

typedef struct {
    pbw_schema *schema;
    /* One arena + encode buffer for every call. */
    pbw_ctx *ctx;
    /* Per-descriptor interned field names for the decode->named-list walk,
     * resolved once per message type. */
    struct pbwr_fields *fields;
    /* Last one used: a decode loop usually repeats the same message type, so
     * this skips the list walk. */
    struct pbwr_fields *last;
} pbwr_schema;

/* A builder owns a plain malloc tree. It preserves its schema SEXP
 * (released by the finalizer): pbw_message_free() reads the descriptor, so
 * the schema must outlive every builder. */
typedef struct {
    const pbw_descriptor *desc;
    pbw_message *tree;
    SEXP schema_sexp;
} pbwr_message;

/* A message type's interned field names, in descriptor order: the
 * decode->list walk stores these instead of paying a mkChar hash lookup per
 * field per message. */
typedef struct pbwr_fields {
    const pbw_descriptor *desc;
    size_t nfields;
    SEXP names; /* STRSXP, R_PreserveObject'd */
    struct pbwr_fields *next;
} pbwr_fields;

static SEXP pbwr_schema_tag(void)
{
    static SEXP tag = NULL;

    if (tag == NULL) {
        tag = install("PbwireSchema");
        R_PreserveObject(tag);
    }
    return tag;
}

static SEXP pbwr_message_tag(void)
{
    static SEXP tag = NULL;

    if (tag == NULL) {
        tag = install("PbwireMessage");
        R_PreserveObject(tag);
    }
    return tag;
}

static pbwr_schema *
pbwr_check_schema(SEXP s)
{
    pbwr_schema *p;

    if (TYPEOF(s) != EXTPTRSXP || R_ExternalPtrTag(s) != pbwr_schema_tag()) {
        Rf_error("pbwire: not a pbwire schema");
    }
    p = (pbwr_schema *)R_ExternalPtrAddr(s);
    if (p == NULL || p->schema == NULL) {
        Rf_error("pbwire: schema is closed");
    }
    return p;
}

static pbwr_message *
pbwr_check_message(SEXP s)
{
    pbwr_message *m;

    if (TYPEOF(s) != EXTPTRSXP || R_ExternalPtrTag(s) != pbwr_message_tag()) {
        Rf_error("pbwire: not a pbwire message");
    }
    m = (pbwr_message *)R_ExternalPtrAddr(s);
    if (m == NULL || m->tree == NULL) {
        Rf_error("pbwire: message is closed");
    }
    return m;
}

static const pbw_descriptor *
pbwr_find(pbw_schema *schema, const char *name)
{
    const pbw_descriptor *desc = pbw_schema_find(schema, name);

    if (desc == NULL || pbw_descriptor_is_enum(desc)) {
        Rf_error("pbwire: no message %s in the schema", name);
    }
    return desc;
}

static void
pbwr_fail(pbw_status status, const char *what, const char *name)
{
    Rf_error("pbwire: %s %s: %s", what, name, pbw_strerror(status));
}

static pbw_value *
pbwr_uint_bits(pbw_ctx *ctx, uint64_t bits)
{
    if (ctx != NULL) {
        return pbw_value_new_uint_ctx(ctx, bits);
    }
    return pbw_value_new_uint(bits);
}

static pbw_value *
pbwr_bytes_copy(pbw_ctx *ctx, const void *data, size_t len)
{
    if (ctx != NULL) {
        return pbw_value_new_bytes_ctx(ctx, data, len);
    }
    return pbw_value_new_bytes(data, len);
}

/* Plain number from an integer/numeric/logical vector element. */
static double
pbwr_as_double(SEXP v, R_xlen_t i, const char *fname)
{
    switch (TYPEOF(v)) {
    case REALSXP:
        if (ISNAN(REAL(v)[i])) {
            Rf_error("pbwire: field %s expects a number", fname);
        }
        return REAL(v)[i];
    case INTSXP:
        if (INTEGER(v)[i] == NA_INTEGER) {
            Rf_error("pbwire: field %s expects a number", fname);
        }
        return (double)INTEGER(v)[i];
    case LGLSXP:
        if (LOGICAL(v)[i] == NA_LOGICAL) {
            Rf_error("pbwire: field %s expects a number", fname);
        }
        return (double)LOGICAL(v)[i];
    default:
        break;
    }
    Rf_error("pbwire: field %s expects a number", fname);
    return 0;
}

/* Doubles past 2^64 are the only way R spells uint64 max (see the file
 * comment), so clamp there. Negative integral doubles in the int64 range
 * spell two's complement bit patterns (`-1` is 2^64-1, the Lua bench
 * idiom); anything else negative is out of range. */
static uint64_t
pbwr_to_u64(double d, const char *fname)
{
    if (d < 0.0) {
        if (d >= -9223372036854775808.0) {
            int64_t i = (int64_t)d;

            if ((double)i == d) {
                return (uint64_t)i;
            }
        }
        Rf_error("pbwire: field %s is out of range", fname);
    }
    if (d >= 18446744073709551616.0) { /* 2^64 */
        return UINT64_MAX;
    }
    return (uint64_t)d;
}

static int64_t
pbwr_to_i64(double d)
{
    if (d >= 9223372036854775808.0) { /* 2^63 */
        return INT64_MAX;
    }
    if (d < -9223372036854775808.0) {
        return INT64_MIN;
    }
    return (int64_t)d;
}

/* One scalar value: v holds a single value at index i, except RAWSXP which
 * is always the whole bytes value. ctx != NULL (list API): arena, no
 * per-value malloc; ctx == NULL (builder): plain malloc values owned by
 * the tree. */
static pbw_value *
pbwr_scalar_value(SEXP v, R_xlen_t i, const pbw_field *field, pbw_ctx *ctx)
{
    pbw_type type = pbw_field_type(field);
    const char *fname = pbw_field_name(field);

    switch (type) {
    case PBW_TYPE_STRING: {
        SEXP s;
        const char *utf8;

        if (TYPEOF(v) != STRSXP || i >= Rf_xlength(v)) {
            Rf_error("pbwire: field %s expects a string", fname);
        }
        s = STRING_ELT(v, i);
        if (s == NA_STRING) {
            Rf_error("pbwire: field %s: NA is not a valid string", fname);
        }
        /* protobuf `string` is UTF-8 on the wire: translate an R string
         * marked latin1 (or native in a non-UTF-8 locale) instead of
         * shipping its bytes as-is, which no peer could read as UTF-8. */
        utf8 = Rf_translateCharUTF8(s);
        return pbwr_bytes_copy(ctx, utf8, strlen(utf8));
    }
    case PBW_TYPE_BYTES: {
        if (TYPEOF(v) == RAWSXP) {
            return pbwr_bytes_copy(ctx, RAW(v), (size_t)Rf_xlength(v));
        }
        if (TYPEOF(v) == STRSXP && Rf_xlength(v) > 0) {
            SEXP s = STRING_ELT(v, 0);

            if (s == NA_STRING) {
                Rf_error("pbwire: field %s: NA is not valid bytes", fname);
            }
            return pbwr_bytes_copy(ctx, CHAR(s), (size_t)LENGTH(s));
        }
        Rf_error("pbwire: field %s expects a raw vector or string", fname);
        return NULL;
    }
    case PBW_TYPE_FLOAT: {
        float f = (float)pbwr_as_double(v, i, fname);
        uint32_t bits = 0;

        memcpy(&bits, &f, sizeof(bits));
        return pbwr_uint_bits(ctx, bits);
    }
    case PBW_TYPE_DOUBLE: {
        double d = pbwr_as_double(v, i, fname);
        uint64_t bits = 0;

        memcpy(&bits, &d, sizeof(bits));
        return pbwr_uint_bits(ctx, bits);
    }
    case PBW_TYPE_BOOL: {
        int b;

        if (TYPEOF(v) != LGLSXP || i >= Rf_xlength(v)) {
            Rf_error("pbwire: field %s expects a logical", fname);
        }
        b = LOGICAL(v)[i];
        if (b == NA_LOGICAL) {
            Rf_error("pbwire: field %s: NA is not a valid bool", fname);
        }
        return pbwr_uint_bits(ctx, b ? 1u : 0u);
    }
    case PBW_TYPE_ENUM:
        if (TYPEOF(v) == STRSXP && Rf_xlength(v) > 0) {
            /* Numbers round-trip as numbers; names ("COLOR_RED") resolve
             * through the enum. */
            const pbw_descriptor *ed = pbw_field_message_type(field);
            SEXP s = STRING_ELT(v, 0);
            int32_t number = 0;

            if (s == NA_STRING || ed == NULL ||
                !pbw_enum_find_value(ed, CHAR(s), &number)) {
                Rf_error("pbwire: field %s: unknown enum name", fname);
            }
            return pbwr_uint_bits(ctx, (uint64_t)(int64_t)number);
        }
        /* fall through */
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32: {
        int64_t n = pbwr_to_i64(pbwr_as_double(v, i, fname));

        if (n > INT32_MAX) {
            n = INT32_MAX;
        } else if (n < INT32_MIN) {
            n = INT32_MIN;
        }
        return pbwr_uint_bits(ctx, (uint64_t)(int64_t)(int32_t)n);
    }
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
        return pbwr_uint_bits(
            ctx, (uint64_t)pbwr_to_i64(pbwr_as_double(v, i, fname)));
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32: {
        uint64_t n = pbwr_to_u64(pbwr_as_double(v, i, fname), fname);

        /* The wire field is 32 bits wide: clamp like int32 rather than
         * silently truncating 2^32 to 0. */
        return pbwr_uint_bits(ctx, n > UINT32_MAX ? UINT32_MAX : n);
    }
    default: /* 64-bit unsigned and fixed: raw bit pattern */
        return pbwr_uint_bits(ctx,
                              pbwr_to_u64(pbwr_as_double(v, i, fname), fname));
    }
}

/* R's CHARSXP length is an int: a >2 GiB payload would wrap negative. */
static SEXP
pbwr_charsxp(const char *text, size_t len)
{
    if (len > (size_t)INT32_MAX) {
        Rf_error("pbwire: text payload exceeds R's string limit");
    }
    return Rf_mkCharLenCE(text, (int)len, CE_UTF8);
}

static void pbwr_fill(SEXP list, pbw_ctx *ctx, const pbw_descriptor *desc,
                      pbw_message *msg, unsigned depth);

/* Repeated items share one setter: list elements arrive whole (i = 0),
 * atomic vector elements arrive by index. */
static void
pbwr_append(pbw_message *msg, const pbw_field *field, SEXP v, R_xlen_t i,
            pbw_ctx *ctx, unsigned depth)
{
    if (pbw_field_type(field) == PBW_TYPE_MESSAGE ||
        pbw_field_type(field) == PBW_TYPE_GROUP) {
        const pbw_descriptor *sub = pbw_field_message_type(field);
        pbw_message *child;

        if (sub == NULL) {
            Rf_error("pbwire: unresolved message type for field %s",
                     pbw_field_name(field));
        }
        if (TYPEOF(v) != VECSXP) {
            Rf_error("pbwire: field %s expects a named list",
                     pbw_field_name(field));
        }
        child = pbw_message_set_message(msg, pbw_field_number(field));
        if (child == NULL) {
            Rf_error("pbwire: cannot set field %s", pbw_field_name(field));
        }
        pbwr_fill(v, ctx, sub, child, depth + 1);
        return;
    }
    {
        /* Ownership transfers only on success. */
        pbw_value *value = pbwr_scalar_value(v, i, field, ctx);
        pbw_status status;

        if (value == NULL) {
            Rf_error("pbwire: out of memory");
        }
        status = pbw_message_set_field(msg, field, value);
        if (status != PBW_OK) {
            pbw_value_free(value);
            Rf_error("pbwire: field %s: %s", pbw_field_name(field),
                     pbw_strerror(status));
        }
    }
}

/* One lua_next-style pass fills and validates: the core emits fields in
 * descriptor order no matter the insertion order, so equal lists encode to
 * equal bytes without a second pass. */
static void
pbwr_fill(SEXP list, pbw_ctx *ctx, const pbw_descriptor *desc,
          pbw_message *msg, unsigned depth)
{
    SEXP names;
    R_xlen_t n, i;

    if (depth > PBWR_MAX_DEPTH) {
        Rf_error("pbwire: nesting limit exceeded");
    }
    if (TYPEOF(list) != VECSXP) {
        Rf_error("pbwire: a message is a named list");
    }
    names = getAttrib(list, R_NamesSymbol);
    n = Rf_xlength(list);
    if (n > 0 && (names == R_NilValue || Rf_xlength(names) != n)) {
        Rf_error("pbwire: message lists need names for every field");
    }
    for (i = 0; i < n; i++) {
        SEXP key = STRING_ELT(names, i);
        const pbw_field *field;
        SEXP v;

        if (key == NA_STRING || CHAR(key)[0] == '\0') {
            Rf_error("pbwire: message lists need names for every field");
        }
        field = pbw_descriptor_find_field_name(desc, CHAR(key));
        if (field == NULL) {
            Rf_error("pbwire: %s has no field '%s'",
                     pbw_descriptor_name(desc), CHAR(key));
        }
        v = VECTOR_ELT(list, i);
        if (v == R_NilValue) {
            continue; /* NULL = absent, like an unset optional. */
        }
        if (pbw_field_label(field) != PBW_LABEL_REPEATED) {
            if (pbw_field_type(field) == PBW_TYPE_MESSAGE ||
                pbw_field_type(field) == PBW_TYPE_GROUP) {
                const pbw_descriptor *sub =
                    pbw_field_message_type(field);
                pbw_message *child;

                if (sub == NULL) {
                    Rf_error("pbwire: unresolved message type for field %s",
                             pbw_field_name(field));
                }
                if (TYPEOF(v) != VECSXP) {
                    Rf_error("pbwire: field %s expects a named list",
                             pbw_field_name(field));
                }
                /* set() replaces: drop the previous submessage first
                 * (a no-op on a fresh tree). */
                pbw_message_clear(msg, pbw_field_number(field));
                child =
                    pbw_message_set_message(msg, pbw_field_number(field));
                if (child == NULL) {
                    Rf_error("pbwire: cannot set field %s",
                             pbw_field_name(field));
                }
                pbwr_fill(v, ctx, sub, child, depth + 1);
                continue;
            }
            pbwr_append(msg, field, v, 0, ctx, depth);
            continue;
        }
        /* Assigning a repeated field replaces its contents (a no-op
         * clear on a fresh tree). */
        pbw_message_clear(msg, pbw_field_number(field));
        if (TYPEOF(v) == VECSXP) {
            R_xlen_t m = Rf_xlength(v);
            R_xlen_t k;

            for (k = 0; k < m; k++) {
                SEXP item = VECTOR_ELT(v, k);

                if (item == R_NilValue) {
                    Rf_error("pbwire: field %s: NULL is not a value",
                             pbw_field_name(field));
                }
                pbwr_append(msg, field, item, 0, ctx, depth);
            }
        } else if (TYPEOF(v) == INTSXP || TYPEOF(v) == REALSXP ||
                   TYPEOF(v) == LGLSXP || TYPEOF(v) == STRSXP) {
            R_xlen_t m = Rf_xlength(v);
            R_xlen_t k;

            for (k = 0; k < m; k++) {
                pbwr_append(msg, field, v, k, ctx, depth);
            }
        } else {
            Rf_error("pbwire: field %s expects a vector or list",
                     pbw_field_name(field));
        }
    }
}

/* ------------------------------------------------------------ C -> R */

static SEXP
pbwr_scalar_sexp(pbw_type type, const pbw_value *value)
{
    switch (type) {
    case PBW_TYPE_STRING: {
        size_t len = 0;
        const void *data = pbw_value_get_bytes(value, &len);
        SEXP s;

        PROTECT(s = allocVector(STRSXP, 1));
        /* Marked UTF-8: protobuf strings are UTF-8, and an unmarked CHARSXP
         * reads as "unknown" in a non-UTF-8 locale (no `==` with literals). */
        SET_STRING_ELT(s, 0, pbwr_charsxp((const char *)data, len));
        UNPROTECT(1);
        return s;
    }
    case PBW_TYPE_BYTES: {
        size_t len = 0;
        const void *data = pbw_value_get_bytes(value, &len);
        SEXP s = allocVector(RAWSXP, (R_xlen_t)len);

        if (len > 0) {
            memcpy(RAW(s), data, len);
        }
        return s;
    }
    case PBW_TYPE_FLOAT:
        return ScalarReal((double)pbw_value_get_float(value));
    case PBW_TYPE_DOUBLE:
        return ScalarReal(pbw_value_get_double(value));
    case PBW_TYPE_BOOL:
        return ScalarLogical(pbw_value_get_bool(value) ? TRUE : FALSE);
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_ENUM:
        return ScalarInteger(pbw_value_get_int32(value));
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
        return ScalarReal((double)pbw_value_get_int(value));
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32:
        /* Past INT_MAX, so numeric: integers cannot spell it. */
        return ScalarReal((double)pbw_value_get_uint32(value));
    default:
        return ScalarReal((double)pbw_value_get_uint(value));
    }
}

static SEXP pbwr_message_sexp(const pbw_message *msg, pbwr_schema *s,
                              unsigned depth);

/* The interned key for field f: from the cache when it was built, from
 * mkChar otherwise (allocation failure, or a descriptor with no cache). */
static SEXP
pbwr_name(pbwr_fields *cache, size_t index, const pbw_field *field)
{
    if (cache != NULL && index < cache->nfields) {
        return STRING_ELT(cache->names, (R_xlen_t)index);
    }
    return mkChar(pbw_field_name(field));
}

/* Cache of a message type's prepared fields, built on first decode. NULL
 * when memory is short: the walk then falls back to mkChar per field. */
static pbwr_fields *
pbwr_fields_get(pbwr_schema *s, const pbw_descriptor *desc)
{
    pbwr_fields *c;
    size_t nfields, i;

    if (s->last != NULL && s->last->desc == desc) {
        return s->last;
    }
    for (c = s->fields; c != NULL; c = c->next) {
        if (c->desc == desc) {
            s->last = c;
            return c;
        }
    }
    nfields = pbw_descriptor_field_count(desc);
    c = (pbwr_fields *)calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->names = PROTECT(allocVector(STRSXP, (R_xlen_t)nfields));
    for (i = 0; i < nfields; i++) {
        SET_STRING_ELT(
            c->names, (R_xlen_t)i,
            mkChar(pbw_field_name(pbw_descriptor_field_at(desc, i))));
    }
    c->desc = desc;
    c->nfields = nfields;
    R_PreserveObject(c->names);
    UNPROTECT(1);
    c->next = s->fields;
    s->fields = c;
    s->last = c;
    return c;
}

/* Repeated scalars decode as atomic vectors (what R code already maps
 * over); messages and bytes decode as lists. */
static SEXP
pbwr_repeated_sexp(pbwr_schema *s, const pbw_field *field,
                   const pbw_value *list, unsigned depth)
{
    size_t count = (list != NULL) ? pbw_value_count(list) : 0;
    pbw_type type = pbw_field_type(field);
    SEXP out;
    size_t k;

    if (list == NULL) {
        /* Absent reads as an empty list (test/README contract). */
        PROTECT(out = allocVector(VECSXP, 0));
        UNPROTECT(1);
        return out;
    }

    if (type == PBW_TYPE_MESSAGE || type == PBW_TYPE_GROUP) {
        PROTECT(out = allocVector(VECSXP, (R_xlen_t)count));
        for (k = 0; k < count; k++) {
            SEXP item =
                pbwr_message_sexp(pbw_value_get_message(
                                      pbw_value_at(list, k)),
                                  s, depth + 1);

            PROTECT(item);
            SET_VECTOR_ELT(out, (R_xlen_t)k, item);
            UNPROTECT(1);
        }
        UNPROTECT(1);
        return out;
    }
    if (type == PBW_TYPE_BYTES) {
        PROTECT(out = allocVector(VECSXP, (R_xlen_t)count));
        for (k = 0; k < count; k++) {
            SEXP item = pbwr_scalar_sexp(type, pbw_value_at(list, k));

            PROTECT(item);
            SET_VECTOR_ELT(out, (R_xlen_t)k, item);
            UNPROTECT(1);
        }
        UNPROTECT(1);
        return out;
    }
    switch (type) {
    case PBW_TYPE_STRING:
        PROTECT(out = allocVector(STRSXP, (R_xlen_t)count));
        break;
    case PBW_TYPE_BOOL:
        PROTECT(out = allocVector(LGLSXP, (R_xlen_t)count));
        break;
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_ENUM:
        PROTECT(out = allocVector(INTSXP, (R_xlen_t)count));
        break;
    default:
        PROTECT(out = allocVector(REALSXP, (R_xlen_t)count));
        break;
    }
    for (k = 0; k < count; k++) {
        const pbw_value *item = pbw_value_at(list, k);
        R_xlen_t j = (R_xlen_t)k;

        switch (type) {
        case PBW_TYPE_STRING: {
            size_t len = 0;
            const void *data = pbw_value_get_bytes(item, &len);

            SET_STRING_ELT(out, j, pbwr_charsxp((const char *)data, len));
            break;
        }
        case PBW_TYPE_BOOL:
            LOGICAL(out)[j] = pbw_value_get_bool(item) ? TRUE : FALSE;
            break;
        case PBW_TYPE_INT32:
        case PBW_TYPE_SINT32:
        case PBW_TYPE_SFIXED32:
        case PBW_TYPE_ENUM:
            INTEGER(out)[j] = pbw_value_get_int32(item);
            break;
        case PBW_TYPE_FLOAT:
            REAL(out)[j] = (double)pbw_value_get_float(item);
            break;
        case PBW_TYPE_DOUBLE:
            REAL(out)[j] = pbw_value_get_double(item);
            break;
        case PBW_TYPE_INT64:
        case PBW_TYPE_SINT64:
        case PBW_TYPE_SFIXED64:
            REAL(out)[j] = (double)pbw_value_get_int(item);
            break;
        case PBW_TYPE_UINT32:
        case PBW_TYPE_FIXED32:
            REAL(out)[j] = (double)pbw_value_get_uint32(item);
            break;
        default:
            REAL(out)[j] = (double)pbw_value_get_uint(item);
            break;
        }
    }
    UNPROTECT(1);
    return out;
}

/* pbwr_message_sexp raises through R's longjmp, which skips the caller's
 * pbw_message_free. Walk the tree first so a too-deep message (peer
 * controlled) is rejected while the caller still owns and can free it. */
static int
pbwr_depth_ok(const pbw_message *msg, unsigned depth)
{
    const pbw_descriptor *desc = pbw_message_descriptor(msg);
    size_t nfields = pbw_descriptor_field_count(desc);
    size_t f;

    if (msg == NULL || desc == NULL) {
        return 0;
    }
    if (depth > PBWR_MAX_DEPTH) {
        return 0;
    }
    for (f = 0; f < nfields; f++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, f);
        pbw_value *value = pbw_message_get_field(msg, field);

        if (value == NULL) {
            continue;
        }
        if (pbw_value_get_kind(value) == PBW_VALUE_LIST) {
            size_t i, n = pbw_value_count(value);

            for (i = 0; i < n; i++) {
                pbw_value *item = pbw_value_at(value, i);

                if (item != NULL &&
                    pbw_value_get_kind(item) == PBW_VALUE_MESSAGE &&
                    !pbwr_depth_ok(pbw_value_get_message(item), depth + 1)) {
                    return 0;
                }
            }
        } else if (pbw_value_get_kind(value) == PBW_VALUE_MESSAGE &&
                   !pbwr_depth_ok(pbw_value_get_message(value), depth + 1)) {
            return 0;
        }
    }
    return 1;
}

static SEXP
pbwr_message_sexp(const pbw_message *msg, pbwr_schema *s, unsigned depth)
{
    const pbw_descriptor *desc = pbw_message_descriptor(msg);
    size_t nfields = pbw_descriptor_field_count(desc);
    /* The cache is per descriptor: a nested message needs its own. */
    pbwr_fields *cache = s != NULL ? pbwr_fields_get(s, desc) : NULL;
    /* Present fields first, then one empty vector/list per absent
     * repeated field, so R code can always length()/lapply() it. */
    size_t total = 0;
    size_t f, i = 0;
    SEXP out, names;

    if (depth > PBWR_MAX_DEPTH) {
        Rf_error("pbwire: nesting limit exceeded");
    }
    /* One walk of the descriptor, not of the present fields through
     * pbw_message_field_at(): that call scans the field table from the start,
     * so counting that way was quadratic per message. pbw_message_get_field()
     * is one index and one presence bit. */
    for (f = 0; f < nfields; f++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, f);

        total += (size_t)(pbw_message_get_field(msg, field) != NULL ||
                          pbw_field_label(field) == PBW_LABEL_REPEATED);
    }
    PROTECT(out = allocVector(VECSXP, (R_xlen_t)total));
    PROTECT(names = allocVector(STRSXP, (R_xlen_t)total));
    for (f = 0; f < nfields; f++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, f);
        pbw_value *value = pbw_message_get_field(msg, field);
        SEXP item;

        if (value == NULL) {
            continue;
        }
        if (pbw_value_get_kind(value) == PBW_VALUE_LIST) {
            item = pbwr_repeated_sexp(s, field, value, depth);
        } else if (pbw_value_get_kind(value) == PBW_VALUE_MESSAGE) {
            item = pbwr_message_sexp(pbw_value_get_message(value), s,
                                     depth + 1);
        } else {
            item = pbwr_scalar_sexp(pbw_field_type(field), value);
        }
        PROTECT(item);
        SET_VECTOR_ELT(out, (R_xlen_t)i, item);
        SET_STRING_ELT(names, (R_xlen_t)i, pbwr_name(cache, f, field));
        UNPROTECT(1);
        i++;
    }
    for (f = 0; f < nfields; f++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, f);
        SEXP item;

        if (pbw_field_label(field) != PBW_LABEL_REPEATED ||
            pbw_message_get_field(msg, field) != NULL) {
            continue;
        }
        item = pbwr_repeated_sexp(s, field, NULL, depth);
        PROTECT(item);
        SET_VECTOR_ELT(out, (R_xlen_t)i, item);
        SET_STRING_ELT(names, (R_xlen_t)i, pbwr_name(cache, f, field));
        UNPROTECT(1);
        i++;
    }
    setAttrib(out, R_NamesSymbol, names);
    UNPROTECT(2);
    return out;
}

/* ------------------------------------------------------------ entry points */

static void
pbwr_schema_finalizer(SEXP s)
{
    pbwr_schema *p = (pbwr_schema *)R_ExternalPtrAddr(s);

    if (p != NULL) {
        pbwr_fields *c = p->fields;

        while (c != NULL) {
            pbwr_fields *next = c->next;

            R_ReleaseObject(c->names);
            free(c);
            c = next;
        }
        if (p->ctx != NULL) {
            pbw_ctx_free(p->ctx);
        }
        if (p->schema != NULL) {
            pbw_schema_free(p->schema);
        }
        free(p);
        R_ClearExternalPtr(s);
    }
}

static void
pbwr_message_finalizer(SEXP s)
{
    pbwr_message *m = (pbwr_message *)R_ExternalPtrAddr(s);

    if (m != NULL) {
        if (m->tree != NULL) {
            pbw_message_free(m->tree);
        }
        if (m->schema_sexp != R_NilValue) {
            R_ReleaseObject(m->schema_sexp);
        }
        free(m);
        R_ClearExternalPtr(s);
    }
}

/* Raw descriptor-set bytes (or a single string holding them) -> schema. */
SEXP
pbwr_load(SEXP data)
{
    const void *bytes;
    size_t len;
    SEXP raw = data;
    pbw_schema *schema = NULL;
    pbw_status status;
    pbwr_schema *p;
    SEXP s;

    if (TYPEOF(raw) == STRSXP) {
        if (Rf_xlength(raw) < 1 || STRING_ELT(raw, 0) == NA_STRING) {
            Rf_error("pbwire: descriptor set is empty");
        }
        bytes = CHAR(STRING_ELT(raw, 0));
        len = (size_t)LENGTH(STRING_ELT(raw, 0));
    } else if (TYPEOF(raw) == RAWSXP) {
        bytes = RAW(raw);
        len = (size_t)Rf_xlength(raw);
    } else {
        Rf_error("pbwire: descriptor set must be a raw vector");
        return R_NilValue;
    }
    status = pbw_descriptor_set_load(bytes, len, &schema);
    if (status != PBW_OK) {
        /* The reason lives inside the schema: copy it before freeing it
         * (Rf_error longjmps after the free, so a pointer would be stale). */
        char reason[256];
        const char *why = schema != NULL ? pbw_schema_error(schema) : NULL;

        snprintf(reason, sizeof(reason), "%s", why != NULL ? why : "");
        pbw_schema_free(schema);
        Rf_error("pbwire: descriptor set rejected: %s",
                 reason[0] != '\0' ? reason : pbw_strerror(status));
    }
    /* calloc: fields/last are only touched later, and the finalizer walks
     * them, so they must start NULL. */
    p = (pbwr_schema *)calloc(1, sizeof(*p));
    if (p == NULL) {
        pbw_schema_free(schema);
        Rf_error("pbwire: out of memory");
    }
    p->schema = schema;
    p->ctx = pbw_ctx_new();
    if (p->ctx == NULL) {
        pbw_schema_free(schema);
        free(p);
        Rf_error("pbwire: out of memory");
    }
    PROTECT(s = R_MakeExternalPtr(p, pbwr_schema_tag(), R_NilValue));
    R_RegisterCFinalizerEx(s, pbwr_schema_finalizer, FALSE);
    UNPROTECT(1);
    return s;
}

static const char *
pbwr_type_name(SEXP type)
{
    SEXP s;

    if (TYPEOF(type) != STRSXP || Rf_xlength(type) < 1 ||
        (s = STRING_ELT(type, 0)) == NA_STRING) {
        Rf_error("pbwire: message type must be a string");
    }
    return CHAR(s);
}

/* Wire bytes in: raw, or a single string holding them. */
static void
pbwr_wire_in(SEXP wire, const void **bytes, size_t *len)
{
    if (TYPEOF(wire) == RAWSXP) {
        *bytes = RAW(wire);
        *len = (size_t)Rf_xlength(wire);
    } else if (TYPEOF(wire) == STRSXP && Rf_xlength(wire) > 0 &&
               STRING_ELT(wire, 0) != NA_STRING) {
        *bytes = CHAR(STRING_ELT(wire, 0));
        *len = (size_t)LENGTH(STRING_ELT(wire, 0));
    } else {
        Rf_error("pbwire: wire input must be a raw vector");
    }
}

SEXP
pbwr_encode(SEXP schema_sexp, SEXP type, SEXP list)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    /* Reset first: the arena holds the previous call's tree, the buffer
     * the previous encoding. One reset covers both. */
    pbw_message *msg;
    const uint8_t *out = NULL;
    size_t len = 0;
    pbw_status status;
    SEXP ans;

    pbw_ctx_reset(s->ctx);
    msg = pbw_message_new_ctx(s->ctx, desc);
    if (msg == NULL) {
        Rf_error("pbwire: out of memory");
    }
    pbwr_fill(list, s->ctx, desc, msg, 0);
    /* Reuses the context's buffer: steady use stops calling malloc. */
    status = pbw_encode_ctx(s->ctx, msg, &out, &len);
    if (status != PBW_OK) {
        pbwr_fail(status, "encode", name);
    }
    ans = allocVector(RAWSXP, (R_xlen_t)len);
    if (len > 0) {
        memcpy(RAW(ans), out, len);
    }
    return ans;
}

/* Arena decode: one bump allocator, no per-value malloc. The tree is
 * read-only and dies with the next reset; conversion only reads. */
SEXP
pbwr_decode(SEXP schema_sexp, SEXP type, SEXP wire)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    const void *bytes;
    size_t len;
    pbw_message *msg = NULL;
    pbw_status status;

    pbwr_wire_in(wire, &bytes, &len);
    pbw_ctx_reset(s->ctx);
    status = pbw_decode_ctx(s->ctx, desc, bytes, len, &msg);
    if (status != PBW_OK) {
        pbwr_fail(status, "decode", name);
    }
    return pbwr_message_sexp(msg, s, 0);
}

/* Gateway passthrough (and the conformance binary path): decode and
 * re-encode through the core, so unknown fields survive. */
SEXP
pbwr_roundtrip(SEXP schema_sexp, SEXP type, SEXP wire)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    const void *bytes;
    size_t len;
    pbw_message *msg = NULL;
    const uint8_t *out = NULL;
    size_t outlen = 0;
    pbw_status status;
    SEXP ans;

    pbwr_wire_in(wire, &bytes, &len);
    pbw_ctx_reset(s->ctx);
    status = pbw_decode_ctx(s->ctx, desc, bytes, len, &msg);
    if (status != PBW_OK) {
        pbwr_fail(status, "decode", name);
    }
    status = pbw_encode_ctx(s->ctx, msg, &out, &outlen);
    if (status != PBW_OK) {
        pbwr_fail(status, "encode", name);
    }
    ans = allocVector(RAWSXP, (R_xlen_t)outlen);
    if (outlen > 0) {
        memcpy(RAW(ans), out, outlen);
    }
    return ans;
}

/* Text in: an R string (NA and empty vectors fail, like a bad type). The
 * parser and the printer both speak UTF-8, so a string marked latin1 is
 * translated instead of parsed byte for byte. */
static const char *
pbwr_text_in(SEXP v, size_t *len, const char *what)
{
    SEXP s;
    const char *utf8;

    if (TYPEOF(v) != STRSXP || Rf_xlength(v) < 1) {
        Rf_error("pbwire: %s expects a string", what);
    }
    s = STRING_ELT(v, 0);
    if (s == NA_STRING) {
        Rf_error("pbwire: %s: NA is not valid text", what);
    }
    utf8 = Rf_translateCharUTF8(s);
    *len = strlen(utf8);
    return utf8;
}

/* Text out: a one-element string vector (NA-safe by construction). Marked
 * UTF-8 so a non-UTF-8 locale still reads it as text, not as unknown bytes. */
static SEXP
pbwr_text_out(const char *text, size_t len)
{
    SEXP ans;

    PROTECT(ans = allocVector(STRSXP, 1));
    SET_STRING_ELT(ans, 0, pbwr_charsxp(text, len));
    UNPROTECT(1);
    return ans;
}

/* JSON mapping text of a named list. */
static SEXP
pbwr_to_json(SEXP schema_sexp, SEXP type, SEXP list)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    pbw_message *msg;
    char *text = NULL;
    size_t len = 0;
    pbw_status status;
    SEXP ans;

    pbw_ctx_reset(s->ctx);
    msg = pbw_message_new_ctx(s->ctx, desc);
    if (msg == NULL) {
        Rf_error("pbwire: out of memory");
    }
    pbwr_fill(list, s->ctx, desc, msg, 0);
    status = pbw_json_print(msg, &text, &len);
    if (status != PBW_OK) {
        pbwr_fail(status, "to_json", name);
    }
    ans = pbwr_text_out(text, len);
    pbw_free(text);
    return ans;
}

/* Parses JSON mapping text into a named list; a fourth TRUE argument skips
 * unknown field names instead of failing. */
static SEXP
pbwr_from_json(SEXP schema_sexp, SEXP type, SEXP text, SEXP ignore)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    size_t len = 0;
    const char *data = pbwr_text_in(text, &len, "from_json");
    pbw_message *msg = NULL;
    pbw_status status;
    pbw_json_parse_options opts;
    SEXP ans;

    opts.ignore_unknown_fields =
        (TYPEOF(ignore) == LGLSXP && Rf_xlength(ignore) > 0 &&
         LOGICAL(ignore)[0] == TRUE);
    status = pbw_json_parse_opts(desc, data, len, &opts, &msg);
    if (status != PBW_OK) {
        pbwr_fail(status, "from_json", name);
    }
    if (!pbwr_depth_ok(msg, 0)) {
        pbw_message_free(msg);
        pbwr_fail(PBW_ERR_LIMIT, "from_json", name);
    }
    ans = pbwr_message_sexp(msg, s, 0);
    pbw_message_free(msg);
    return ans;
}

/* Text format of a named list. */
static SEXP
pbwr_to_text(SEXP schema_sexp, SEXP type, SEXP list)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    pbw_message *msg;
    char *text = NULL;
    size_t len = 0;
    pbw_status status;
    SEXP ans;

    pbw_ctx_reset(s->ctx);
    msg = pbw_message_new_ctx(s->ctx, desc);
    if (msg == NULL) {
        Rf_error("pbwire: out of memory");
    }
    pbwr_fill(list, s->ctx, desc, msg, 0);
    status = pbw_text_print(msg, &text, &len);
    if (status != PBW_OK) {
        pbwr_fail(status, "to_text", name);
    }
    ans = pbwr_text_out(text, len);
    pbw_free(text);
    return ans;
}

/* Parses text format into a named list. */
static SEXP
pbwr_from_text(SEXP schema_sexp, SEXP type, SEXP text)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    size_t len = 0;
    const char *data = pbwr_text_in(text, &len, "from_text");
    pbw_message *msg = NULL;
    pbw_status status;
    SEXP ans;

    status = pbw_text_parse(desc, data, len, &msg);
    if (status != PBW_OK) {
        pbwr_fail(status, "from_text", name);
    }
    /* Same guard as from_json(): raise while the caller still owns the tree
     * (pbwr_message_sexp longjmps on a too-deep message). */
    if (!pbwr_depth_ok(msg, 0)) {
        pbw_message_free(msg);
        pbwr_fail(PBW_ERR_LIMIT, "from_text", name);
    }
    ans = pbwr_message_sexp(msg, s, 0);
    pbw_message_free(msg);
    return ans;
}

/* Canonical encoding: map entries sorted by key, like the reference. */
static SEXP
pbwr_encode_deterministic(SEXP schema_sexp, SEXP type, SEXP list)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    pbw_message *msg;
    uint8_t *out = NULL;
    size_t len = 0;
    pbw_status status;
    SEXP ans;

    pbw_ctx_reset(s->ctx);
    msg = pbw_message_new_ctx(s->ctx, desc);
    if (msg == NULL) {
        Rf_error("pbwire: out of memory");
    }
    pbwr_fill(list, s->ctx, desc, msg, 0);
    status = pbw_encode_deterministic(msg, &out, &len);
    if (status != PBW_OK) {
        pbwr_fail(status, "encode_deterministic", name);
    }
    ans = allocVector(RAWSXP, (R_xlen_t)len);
    if (len > 0) {
        memcpy(RAW(ans), out, len);
    }
    pbw_free(out);
    return ans;
}

/* One length-delimited frame (varint length + message). */
static SEXP
pbwr_encode_delimited(SEXP schema_sexp, SEXP type, SEXP list)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    pbw_message *msg;
    uint8_t *out = NULL;
    size_t len = 0;
    pbw_status status;
    SEXP ans;

    pbw_ctx_reset(s->ctx);
    msg = pbw_message_new_ctx(s->ctx, desc);
    if (msg == NULL) {
        Rf_error("pbwire: out of memory");
    }
    pbwr_fill(list, s->ctx, desc, msg, 0);
    status = pbw_encode_delimited(msg, &out, &len);
    if (status != PBW_OK) {
        pbwr_fail(status, "encode_delimited", name);
    }
    ans = allocVector(RAWSXP, (R_xlen_t)len);
    if (len > 0) {
        memcpy(RAW(ans), out, len);
    }
    pbw_free(out);
    return ans;
}

/* One frame from wire input: list(message, read), so concatenated frames
 * decode in a loop. */
static SEXP
pbwr_decode_delimited(SEXP schema_sexp, SEXP type, SEXP wire)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    const void *bytes;
    size_t len;
    pbw_message *msg = NULL;
    size_t consumed = 0;
    pbw_status status;
    SEXP ans, names, item, read;

    pbwr_wire_in(wire, &bytes, &len);
    status = pbw_decode_delimited(desc, bytes, len, &msg, &consumed);
    if (status != PBW_OK) {
        pbwr_fail(status, "decode_delimited", name);
    }
    if (!pbwr_depth_ok(msg, 0)) {
        pbw_message_free(msg);
        pbwr_fail(PBW_ERR_LIMIT, "decode_delimited", name);
    }
    PROTECT(ans = allocVector(VECSXP, 2));
    PROTECT(names = allocVector(STRSXP, 2));
    item = pbwr_message_sexp(msg, s, 0);
    PROTECT(item);
    pbw_message_free(msg);
    read = allocVector(INTSXP, 1);
    PROTECT(read);
    if (consumed > (size_t)INT32_MAX) {
        UNPROTECT(4);
        Rf_error("pbwire: frame too large");
    }
    INTEGER(read)[0] = (int)consumed;
    SET_VECTOR_ELT(ans, 0, item);
    SET_VECTOR_ELT(ans, 1, read);
    SET_STRING_ELT(names, 0, mkChar("message"));
    SET_STRING_ELT(names, 1, mkChar("read"));
    setAttrib(ans, R_NamesSymbol, names);
    UNPROTECT(4);
    return ans;
}

static SEXP pbwr_push_builder(SEXP schema_sexp, const pbw_descriptor *desc,
                              pbw_message *tree, pbwr_message *m);

/* A builder: an empty plain tree of the type, for hot loops that encode
 * far more often than they rebuild. Build once with set(), then encode()
 * is one core walk plus one buffer malloc. */
SEXP
pbwr_message_new(SEXP schema_sexp, SEXP type)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    pbwr_message *m = (pbwr_message *)malloc(sizeof(*m));

    if (m == NULL) {
        Rf_error("pbwire: out of memory");
    }
    m->desc = desc;
    m->tree = pbw_message_new(desc);
    if (m->tree == NULL) {
        free(m);
        Rf_error("pbwire: out of memory");
    }
    return pbwr_push_builder(schema_sexp, desc, m->tree, m);
}

/* Wraps a plain tree (freshly parsed or built) in a builder. Takes over
 * m (frees it on failure); the schema is preserved until the finalizer. */
static SEXP
pbwr_push_builder(SEXP schema_sexp, const pbw_descriptor *desc,
                  pbw_message *tree, pbwr_message *m)
{
    SEXP ans;

    if (m == NULL) {
        m = (pbwr_message *)malloc(sizeof(*m));
        if (m == NULL) {
            pbw_message_free(tree);
            Rf_error("pbwire: out of memory");
        }
    }
    m->desc = desc;
    m->tree = tree;
    /* Anchor the schema: the tree reads its descriptor until the
     * finalizer runs. */
    m->schema_sexp = schema_sexp;
    R_PreserveObject(schema_sexp);
    PROTECT(ans = R_MakeExternalPtr(m, pbwr_message_tag(), R_NilValue));
    R_RegisterCFinalizerEx(ans, pbwr_message_finalizer, FALSE);
    UNPROTECT(1);
    return ans;
}

/* Parses input straight into a builder tree, skipping lists: exact for
 * 64-bit integers past 2^53, which lists carry as doubles. */
static SEXP
pbwr_parse_json(SEXP schema_sexp, SEXP type, SEXP text, SEXP ignore)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    size_t len = 0;
    const char *data = pbwr_text_in(text, &len, "parse_json");
    pbw_message *msg = NULL;
    pbw_status status;
    pbw_json_parse_options opts;

    opts.ignore_unknown_fields =
        (TYPEOF(ignore) == LGLSXP && Rf_xlength(ignore) > 0 &&
         LOGICAL(ignore)[0] == TRUE);
    status = pbw_json_parse_opts(desc, data, len, &opts, &msg);
    if (status != PBW_OK) {
        pbwr_fail(status, "parse_json", name);
    }
    return pbwr_push_builder(schema_sexp, desc, msg, NULL);
}

static SEXP
pbwr_parse_text(SEXP schema_sexp, SEXP type, SEXP text)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    size_t len = 0;
    const char *data = pbwr_text_in(text, &len, "parse_text");
    pbw_message *msg = NULL;
    pbw_status status;

    status = pbw_text_parse(desc, data, len, &msg);
    if (status != PBW_OK) {
        pbwr_fail(status, "parse_text", name);
    }
    return pbwr_push_builder(schema_sexp, desc, msg, NULL);
}

static SEXP
pbwr_parse_binary(SEXP schema_sexp, SEXP type, SEXP wire)
{
    pbwr_schema *s = pbwr_check_schema(schema_sexp);
    const char *name = pbwr_type_name(type);
    const pbw_descriptor *desc = pbwr_find(s->schema, name);
    const void *bytes;
    size_t len;
    pbw_message *msg = NULL;
    pbw_status status;

    pbwr_wire_in(wire, &bytes, &len);
    status = pbw_decode(desc, bytes, len, &msg);
    if (status != PBW_OK) {
        pbwr_fail(status, "parse_binary", name);
    }
    return pbwr_push_builder(schema_sexp, desc, msg, NULL);
}

/* Prints a builder's own tree: exact, no list in between. */
static SEXP
pbwr_msg_to_json(SEXP msg_sexp)
{
    pbwr_message *m = pbwr_check_message(msg_sexp);
    char *text = NULL;
    size_t len = 0;
    pbw_status status;
    SEXP ans;

    status = pbw_json_print(m->tree, &text, &len);
    if (status != PBW_OK) {
        pbwr_fail(status, "to_json", pbw_descriptor_name(m->desc));
    }
    ans = pbwr_text_out(text, len);
    pbw_free(text);
    return ans;
}

/* Prints a builder's own tree; a second FALSE argument drops unknown
 * fields (the runner's print_unknown_fields flag), TRUE prints them. */
static SEXP
pbwr_msg_to_text(SEXP msg_sexp, SEXP print_unknown)
{
    pbwr_message *m = pbwr_check_message(msg_sexp);
    int print = TYPEOF(print_unknown) != LGLSXP ||
                Rf_xlength(print_unknown) < 1 ||
                LOGICAL(print_unknown)[0] != FALSE;
    char *text = NULL;
    size_t len = 0;
    pbw_status status;
    SEXP ans;

    status = pbw_text_print_opts(m->tree, print, &text, &len);
    if (status != PBW_OK) {
        pbwr_fail(status, "to_text", pbw_descriptor_name(m->desc));
    }
    ans = pbwr_text_out(text, len);
    pbw_free(text);
    return ans;
}

SEXP
pbwr_message_set(SEXP msg_sexp, SEXP list)
{
    pbwr_message *m = pbwr_check_message(msg_sexp);

    /* Plain tree: NULL ctx builds malloc values owned by it. */
    pbwr_fill(list, NULL, m->desc, m->tree, 0);
    return msg_sexp;
}

SEXP
pbwr_message_encode(SEXP msg_sexp)
{
    pbwr_message *m = pbwr_check_message(msg_sexp);
    uint8_t *out = NULL;
    size_t len = 0;
    pbw_status status = pbw_encode(m->tree, &out, &len);
    SEXP ans;

    if (status != PBW_OK) {
        pbwr_fail(status, "encode", pbw_descriptor_name(m->desc));
    }
    ans = allocVector(RAWSXP, (R_xlen_t)len);
    if (len > 0) {
        memcpy(RAW(ans), out, len);
    }
    pbw_free(out);
    return ans;
}

static const R_CallMethodDef pbwr_calls[] = {
    {"pbwr_load", (DL_FUNC)&pbwr_load, 1},
    {"pbwr_encode", (DL_FUNC)&pbwr_encode, 3},
    {"pbwr_decode", (DL_FUNC)&pbwr_decode, 3},
    {"pbwr_roundtrip", (DL_FUNC)&pbwr_roundtrip, 3},
    {"pbwr_to_json", (DL_FUNC)&pbwr_to_json, 3},
    {"pbwr_from_json", (DL_FUNC)&pbwr_from_json, 4},
    {"pbwr_to_text", (DL_FUNC)&pbwr_to_text, 3},
    {"pbwr_from_text", (DL_FUNC)&pbwr_from_text, 3},
    {"pbwr_encode_deterministic", (DL_FUNC)&pbwr_encode_deterministic, 3},
    {"pbwr_encode_delimited", (DL_FUNC)&pbwr_encode_delimited, 3},
    {"pbwr_decode_delimited", (DL_FUNC)&pbwr_decode_delimited, 3},
    {"pbwr_parse_json", (DL_FUNC)&pbwr_parse_json, 4},
    {"pbwr_parse_text", (DL_FUNC)&pbwr_parse_text, 3},
    {"pbwr_parse_binary", (DL_FUNC)&pbwr_parse_binary, 3},
    {"pbwr_msg_to_json", (DL_FUNC)&pbwr_msg_to_json, 1},
    {"pbwr_msg_to_text", (DL_FUNC)&pbwr_msg_to_text, 2},
    {"pbwr_message_new", (DL_FUNC)&pbwr_message_new, 2},
    {"pbwr_message_set", (DL_FUNC)&pbwr_message_set, 2},
    {"pbwr_message_encode", (DL_FUNC)&pbwr_message_encode, 1},
    {NULL, NULL, 0},
};

void
R_init_pbwire(DllInfo *dll)
{
    R_registerRoutines(dll, NULL, pbwr_calls, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
}
