/* Ruby C extension: the Ruby binding.
 *
 * The schema is a FileDescriptorSet (protoc --descriptor_set_out) loaded at run
 * time, so field numbers, types and presence rules are the descriptor's, never a
 * hand-written copy. Two message representations exist, both Ruby objects of the
 * same class:
 *
 *   builder   schema.message(name), mutable: set() stores into a pbw_message
 *             tree, encode() walks it with the C core (pbw_encode_to writes
 *             straight into the returned String), get() reads it back.
 *
 *   wire view decode(bytes), read-only: one validating tag scan records, per
 *             wire field, where its value lives. get() materializes only the
 *             field asked for (a scalar, a String, a sub-message view, or an
 *             Array for a repeated field), and encode() hands back the exact
 *             bytes it was parsed from, so unknown fields, field order and
 *             non-canonical encodings survive a decode/encode round trip.
 *
 * The wire view is what keeps this binding near the C core instead of behind a
 * per-field bridge: decoding 3000 repeated items costs one scan, not 3000
 * message trees, and encoding a builder costs one pass into the caller's
 * string.
 *
 * Field facts (number, kind, wire type, packed/required/oneof, sub type, name
 * lookup) are resolved once per message type and cached per schema, so the hot
 * loops never touch the descriptor again.
 *
 * Memory: a Schema and its messages share a refcounted holder, so whichever
 * side is released last frees the C schema. Nothing relies on finalizer order.
 */
#include <ruby.h>
#include <ruby/encoding.h>

#include <pbwire.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Same cap as the core's PBW_MAX_DEPTH (src/internal.h, not public). */
#define RB_PBW_MAX_DEPTH 256
#define SLICE_INLINE 32
#define SLICE_DEAD 0xffffffffu

static VALUE mPbwire;
static VALUE eError;
static VALUE cSchema;
static VALUE cMessage;
static VALUE cType;

/* --------------------------------------------------------------- schema */

/* The C schema plus the number of Ruby objects pointing at it. The holder
 * outlives whichever side goes first, because pbw_message_free() reads the
 * descriptor and Ruby gives no order for GC. */
typedef struct {
    pbw_schema *schema; /* NULL once freed */
    long refs;
} pbw_holder;

typedef struct {
    pbw_holder *holder;
    VALUE types; /* Hash: message name -> Type, built lazily */
    int closed;
} schema_t;

/* One field of one message type, resolved once. */
typedef struct {
    uint32_t number;
    uint32_t index; /* position in the descriptor's field array */
    uint8_t type;   /* pbw_type */
    uint8_t wire;   /* expected wire type */
    uint8_t repeated;
    uint8_t packed;
    uint8_t required;
    uint8_t oneof; /* 1-based oneof group, 0 = none */
    uint8_t unsupported;
    VALUE name; /* frozen UTF-8 String */
    VALUE sub;  /* Type of a MESSAGE field, else Qnil */
} field_t;

typedef struct {
    pbw_holder *holder; /* not owned: the Schema or a message keeps it */
    const pbw_descriptor *desc;
    size_t count;
    field_t *fields;          /* sorted by field number */
    uint32_t *by_number;      /* dense field number -> index, or NULL */
    uint32_t by_number_size;  /* entries in by_number */
    VALUE by_name;            /* Hash: field name -> Integer index */
    uint32_t oneofs;
    int has_required;
} type_t;

/* One wire field of a decoded message. */
typedef struct {
    size_t off; /* payload offset (delimited) or value offset */
    size_t len; /* payload length (delimited), 0 otherwise */
    uint32_t fidx;
    uint32_t n; /* elements: 1 for an unpacked occurrence */
    uint8_t delimited;
} slice_t;

enum {
    MSG_CLOSED = 0,
    MSG_BUILDER = 1, /* mutable pbw_message tree */
    MSG_WIRE = 2     /* read-only view over decoded bytes */
};

typedef struct {
    int kind;
    VALUE type; /* Type object, also for a builder */
    /* builder */
    pbw_message *tree;
    pbw_holder *holder;
    VALUE parent; /* borrowed sub-message: the builder that owns tree */
    int owned;
    size_t enc_cap;
    /* wire view */
    VALUE buffer; /* frozen String holding the bytes */
    const uint8_t *data;
    size_t off;
    size_t len;
    uint32_t depth;
    slice_t *slices;
    uint32_t nslices;
    uint32_t cap;
    slice_t inline_slices[SLICE_INLINE];
} message_t;

static void schema_mark(void *ptr);
static void schema_free(void *ptr);
static void type_mark(void *ptr);
static void type_free(void *ptr);
static void message_mark(void *ptr);
static void message_free(void *ptr);

static const rb_data_type_t schema_data_type = {
    "pbwire/schema",
    {schema_mark, schema_free, NULL},
    0,
    0,
    RUBY_TYPED_FREE_IMMEDIATELY,
};

static const rb_data_type_t type_data_type = {
    "pbwire/type",
    {type_mark, type_free, NULL},
    0,
    0,
    RUBY_TYPED_FREE_IMMEDIATELY,
};

static const rb_data_type_t message_data_type = {
    "pbwire/message",
    {message_mark, message_free, NULL},
    0,
    0,
    RUBY_TYPED_FREE_IMMEDIATELY,
};

/* Allocate through the class allocator, so every field starts at its documented
 * empty value (Qnil, not zero). TypedData_Make_Struct zero-fills instead. */
static VALUE
typed_object_alloc(VALUE klass, const rb_data_type_t *data_type, void **out)
{
    VALUE obj = rb_obj_alloc(klass);

    TypedData_Get_Struct(obj, void, data_type, *out);
    return obj;
}

static pbw_holder *
holder_new(pbw_schema *schema)
{
    pbw_holder *holder = ALLOC(pbw_holder);

    holder->schema = schema;
    holder->refs = 1;
    return holder;
}

static void
holder_ref(pbw_holder *holder)
{
    if (holder != NULL) {
        holder->refs++;
    }
}

static void
holder_release(pbw_holder *holder)
{
    if (holder == NULL) {
        return;
    }
    if (--holder->refs == 0) {
        if (holder->schema != NULL) {
            pbw_schema_free(holder->schema);
        }
        xfree(holder);
    }
}

/* Schema#close: free the C schema now when nobody else points at it, else let
 * the last message release it. */
static void
holder_close(pbw_holder *holder)
{
    if (holder->schema != NULL && holder->refs == 1) {
        pbw_schema_free(holder->schema);
        holder->schema = NULL;
    }
}

static void
schema_mark(void *ptr)
{
    schema_t *schema = (schema_t *)ptr;

    if (schema != NULL) {
        rb_gc_mark(schema->types);
    }
}

static void
schema_free(void *ptr)
{
    schema_t *schema = (schema_t *)ptr;

    if (schema == NULL) {
        return;
    }
    holder_release(schema->holder);
    xfree(schema);
}

static void
type_mark(void *ptr)
{
    type_t *type = (type_t *)ptr;
    size_t i;

    if (type == NULL) {
        return;
    }
    rb_gc_mark(type->by_name);
    for (i = 0; i < type->count; i++) {
        rb_gc_mark(type->fields[i].name);
        rb_gc_mark(type->fields[i].sub);
    }
}

static void
type_free(void *ptr)
{
    type_t *type = (type_t *)ptr;

    if (type == NULL) {
        return;
    }
    xfree(type->fields);
    xfree(type->by_number);
    xfree(type);
}

static void
message_mark(void *ptr)
{
    message_t *message = (message_t *)ptr;

    if (message == NULL) {
        return;
    }
    rb_gc_mark(message->type);
    rb_gc_mark(message->parent);
    rb_gc_mark(message->buffer);
}

static void
message_free(void *ptr)
{
    message_t *message = (message_t *)ptr;

    if (message == NULL) {
        return;
    }
    if (message->kind == MSG_BUILDER && message->owned &&
        message->tree != NULL) {
        pbw_message_free(message->tree);
    }
    holder_release(message->holder);
    if (message->slices != message->inline_slices) {
        free(message->slices);
    }
    xfree(message);
}

static VALUE
schema_alloc(VALUE klass)
{
    schema_t *schema;
    VALUE obj = TypedData_Make_Struct(klass, schema_t, &schema_data_type, schema);

    schema->holder = NULL;
    schema->types = Qnil;
    schema->closed = 0;
    return obj;
}

static VALUE
type_alloc(VALUE klass)
{
    type_t *type;
    VALUE obj = TypedData_Make_Struct(klass, type_t, &type_data_type, type);

    type->holder = NULL;
    type->desc = NULL;
    type->count = 0;
    type->fields = NULL;
    type->by_number = NULL;
    type->by_number_size = 0;
    type->by_name = Qnil;
    type->oneofs = 0;
    type->has_required = 0;
    return obj;
}

static VALUE
message_alloc(VALUE klass)
{
    message_t *message;
    VALUE obj = TypedData_Make_Struct(klass, message_t, &message_data_type,
                                      message);

    message->kind = MSG_CLOSED;
    message->type = Qnil;
    message->tree = NULL;
    message->holder = NULL;
    message->parent = Qnil;
    message->owned = 0;
    message->enc_cap = 0;
    message->buffer = Qnil;
    message->data = NULL;
    message->off = 0;
    message->len = 0;
    message->depth = 0;
    message->slices = message->inline_slices;
    message->nslices = 0;
    message->cap = SLICE_INLINE;
    return obj;
}

static const char *
type_name(type_t *type)
{
    const char *name = pbw_descriptor_name(type->desc);

    return name != NULL ? name : "message";
}

static pbw_schema *
schema_ptr(VALUE obj)
{
    schema_t *schema;

    TypedData_Get_Struct(obj, schema_t, &schema_data_type, schema);
    if (schema->closed || schema->holder == NULL ||
        schema->holder->schema == NULL) {
        rb_raise(eError, "schema is closed");
    }
    return schema->holder->schema;
}

/* -------------------------------------------------------------- field facts */

static unsigned
wire_type_of(pbw_type type)
{
    switch (type) {
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        return 1;
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        return 5;
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
    case PBW_TYPE_MESSAGE:
        return 2;
    case PBW_TYPE_GROUP:
        return 3;
    default:
        return 0;
    }
}

static int
packable(pbw_type type)
{
    switch (type) {
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
    case PBW_TYPE_MESSAGE:
    case PBW_TYPE_GROUP:
        return 0;
    default:
        return 1;
    }
}

/* A Type object for `desc`, cached on the Schema; recursive schemas resolve
 * because the Type is published before its sub-types are built. */
static VALUE
type_for(VALUE schema_obj, const pbw_descriptor *desc)
{
    schema_t *schema;
    VALUE types;
    VALUE key;
    VALUE found;
    VALUE obj;
    type_t *type;
    size_t count;
    size_t i;

    TypedData_Get_Struct(schema_obj, schema_t, &schema_data_type, schema);
    types = schema->types;
    if (NIL_P(types)) {
        types = rb_hash_new();
        schema->types = types;
    }
    key = rb_utf8_str_new_cstr(pbw_descriptor_name(desc));
    found = rb_hash_aref(types, key);
    if (!NIL_P(found)) {
        return found;
    }
    count = pbw_descriptor_field_count(desc);
    obj = typed_object_alloc(cType, &type_data_type, (void **)&type);
    type->desc = desc;
    type->count = count;
    type->fields = count == 0 ? NULL : ZALLOC_N(field_t, count);
    type->by_name = rb_hash_new();
    rb_hash_aset(types, key, obj); /* before the subs: recursive schemas */

    for (i = 0; i < count; i++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, i);
        field_t *f = &type->fields[i];

        f->number = pbw_field_number(field);
        f->index = (uint32_t)i;
        f->type = (uint8_t)pbw_field_type(field);
        f->wire = (uint8_t)wire_type_of((pbw_type)f->type);
        f->repeated = pbw_field_label(field) == PBW_LABEL_REPEATED;
        f->packed = pbw_field_is_packed(field) != 0;
        f->required = pbw_field_label(field) == PBW_LABEL_REQUIRED;
        f->oneof = (uint8_t)pbw_field_oneof_index(field);
        f->unsupported = f->type == PBW_TYPE_GROUP;
        f->name = rb_utf8_str_new_cstr(pbw_field_name(field));
        rb_obj_freeze(f->name);
        f->sub = Qnil;
        if (f->oneof > type->oneofs) {
            type->oneofs = f->oneof;
        }
        if (f->required) {
            type->has_required = 1;
        }
        rb_hash_aset(type->by_name, f->name, UINT2NUM(i));
    }
    for (i = 0; i < count; i++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, i);
        field_t *f = &type->fields[i];

        if (f->type == PBW_TYPE_MESSAGE) {
            f->sub = type_for(schema_obj, pbw_field_message_type(field));
        }
    }
    {
        uint32_t max = 0;

        for (i = 0; i < count; i++) {
            if (type->fields[i].number > max) {
                max = type->fields[i].number;
            }
        }
        if (max <= 512 && max <= (uint32_t)(count * 8 + 8)) {
            type->by_number = ALLOC_N(uint32_t, (size_t)max + 1);
            type->by_number_size = max + 1;
            for (i = 0; i <= max; i++) {
                type->by_number[i] = UINT32_MAX;
            }
            for (i = 0; i < count; i++) {
                type->by_number[type->fields[i].number] = (uint32_t)i;
            }
        }
    }
    return obj;
}

static type_t *
message_type_of(VALUE message)
{
    message_t *m;
    type_t *type;

    TypedData_Get_Struct(message, message_t, &message_data_type, m);
    TypedData_Get_Struct(m->type, type_t, &type_data_type, type);
    return type;
}

/* One indexed load for a dense field-number space, binary search otherwise
 * (descriptor fields are sorted by number). */
static field_t *
type_field_by_number(type_t *type, uint32_t number)
{
    if (type->by_number != NULL) {
        if (number < type->by_number_size) {
            uint32_t index = type->by_number[number];

            if (index != UINT32_MAX) {
                return &type->fields[index];
            }
        }
        return NULL;
    }
    {
        uint32_t lo = 0;
        uint32_t hi = (uint32_t)type->count;

        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;

            if (type->fields[mid].number < number) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < type->count && type->fields[lo].number == number) {
            return &type->fields[lo];
        }
        return NULL;
    }
}

static field_t *
field_lookup(type_t *type, VALUE key)
{
    VALUE index;

    if (RB_INTEGER_TYPE_P(key)) {
        long number = NUM2LONG(key);
        field_t *field = NULL;

        if (number >= 0 && number <= 536870911L) {
            field = type_field_by_number(type, (uint32_t)number);
        }
        if (field != NULL) {
            return field;
        }
        rb_raise(eError, "%s: no field %ld", type_name(type), number);
    }
    if (SYMBOL_P(key)) {
        key = rb_sym2str(key);
    } else {
        key = StringValue(key);
    }
    index = rb_hash_aref(type->by_name, key);
    if (NIL_P(index)) {
        rb_raise(eError, "%s: no field %s", type_name(type),
                 StringValueCStr(key));
    }
    return &type->fields[NUM2UINT(index)];
}

/* --------------------------------------------------------------- wire read */

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} rdr;

static int
rd_bytes(rdr *r, size_t n)
{
    if ((size_t)(r->end - r->p) < n) {
        return 0;
    }
    r->p += n;
    return 1;
}

static int
rd_varint(rdr *r, uint64_t *out)
{
    const uint8_t *p = r->p;
    const uint8_t *end = r->end;
    uint64_t value = 0;
    unsigned shift = 0;

    while (p < end && shift < 64) {
        uint8_t byte = *p++;

        value |= (uint64_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            r->p = p;
            *out = value;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

static int
rd_fixed(rdr *r, uint64_t *out, size_t width)
{
    const uint8_t *p;

    if ((size_t)(r->end - r->p) < width) {
        return 0;
    }
    p = r->p;
    *out = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24);
    if (width == 8) {
        *out |= ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
                ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
    }
    r->p += width;
    return 1;
}

/* A field tag is a varint32: at most five bytes, a field number above zero and
 * a wire type at most 5. */
static int
rd_tag(rdr *r, uint32_t *fnum, unsigned *wt)
{
    const uint8_t *start = r->p;
    uint64_t value;

    if (!rd_varint(r, &value)) {
        return 0;
    }
    if ((size_t)(r->p - start) > 5) {
        return 0;
    }
    if ((value >> 3) == 0 || (value >> 3) > 0x1fffffffull) {
        return 0;
    }
    *wt = (unsigned)(value & 7);
    if (*wt > 5) {
        return 0;
    }
    *fnum = (uint32_t)(value >> 3);
    return 1;
}

/* Unknown or wrongly typed field: skip its value, groups included. */
static int
rd_skip(rdr *r, uint32_t fnum, unsigned wt, unsigned depth)
{
    switch (wt) {
    case 0: {
        uint64_t value;

        return rd_varint(r, &value);
    }
    case 1:
        return rd_bytes(r, 8);
    case 2: {
        uint64_t n;

        if (!rd_varint(r, &n) || n > (uint64_t)(r->end - r->p)) {
            return 0;
        }
        r->p += (size_t)n;
        return 1;
    }
    case 3:
        if (depth >= RB_PBW_MAX_DEPTH) {
            return 0;
        }
        for (;;) {
            uint32_t inner_fnum;
            unsigned inner_wt;

            if (!rd_tag(r, &inner_fnum, &inner_wt)) {
                return 0;
            }
            if (inner_wt == 4) {
                return inner_fnum == fnum;
            }
            if (!rd_skip(r, inner_fnum, inner_wt, depth + 1)) {
                return 0;
            }
        }
    case 5:
        return rd_bytes(r, 4);
    default:
        return 0; /* stray END_GROUP */
    }
}

/* A packed run must hold a whole number of complete values; returns the count
 * or -1. */
static long
scan_run(field_t *f, const uint8_t *p, size_t len)
{
    rdr r;
    long n = 0;

    if (f->wire != 0) {
        size_t width = f->wire == 1 ? 8 : 4;

        if (len % width != 0) {
            return -1;
        }
        return (long)(len / width);
    }
    r.p = p;
    r.end = p + len;
    while (r.p < r.end) {
        uint64_t value;

        if (!rd_varint(&r, &value)) {
            return -1;
        }
        n++;
    }
    return n;
}

/* ------------------------------------------------------------- wire scan */

static int
slice_reserve(message_t *m, uint32_t need)
{
    uint32_t cap;
    slice_t *grown;

    if (need <= m->cap) {
        return 1;
    }
    cap = m->cap;
    while (cap < need) {
        if (cap > UINT32_MAX / 2) {
            return 0;
        }
        cap *= 2;
    }
    if (m->slices == m->inline_slices) {
        grown = (slice_t *)malloc((size_t)cap * sizeof(*grown));
        if (grown != NULL) {
            memcpy(grown, m->inline_slices,
                   (size_t)m->nslices * sizeof(*grown));
        }
    } else {
        grown = (slice_t *)realloc(m->slices, (size_t)cap * sizeof(*grown));
    }
    if (grown == NULL) {
        return 0;
    }
    m->slices = grown;
    m->cap = cap;
    return 1;
}

/* Records one field, keeping at most one live slice per oneof group. */
static int
slice_add(message_t *m, field_t *f, size_t off, size_t len, uint32_t n,
          int delimited, uint32_t *oneof_last)
{
    uint32_t slot = f->oneof == 0 ? 0 : (uint32_t)f->oneof - 1;

    if (f->oneof != 0 && oneof_last[slot] != 0 &&
        m->slices[oneof_last[slot] - 1].fidx != f->index) {
        /* Another member of the group: it is gone. Repeats of this same member
         * stay, because a message merges with itself. */
        m->slices[oneof_last[slot] - 1].fidx = SLICE_DEAD;
    }
    if (!slice_reserve(m, m->nslices + 1)) {
        return 0;
    }
    m->slices[m->nslices].off = off;
    m->slices[m->nslices].len = len;
    m->slices[m->nslices].fidx = f->index;
    m->slices[m->nslices].n = n;
    m->slices[m->nslices].delimited = (uint8_t)delimited;
    m->nslices++;
    if (f->oneof != 0) {
        oneof_last[slot] = m->nslices;
    }
    return 1;
}

/* One tag scan of one message range: every tag, length and packed run checked.
 * With `m` set it also records a slice per wire field; with `m` NULL it only
 * validates, which is how sub-messages are checked without being materialized. */
static pbw_status
scan_message(message_t *m, type_t *type, const uint8_t *data, size_t off,
             size_t len, uint32_t depth, const char **missing)
{
    rdr r;
    unsigned char *seen = NULL;
    uint32_t oneof_stack[8];
    uint32_t *oneof_last = oneof_stack;
    int oneof_heap = 0;
    pbw_status status = PBW_OK;
    size_t i;

    if (depth >= RB_PBW_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    if (type->has_required) {
        seen = (unsigned char *)calloc(type->count, 1);
        if (seen == NULL) {
            return PBW_ERR_NOMEM;
        }
    }
    if (m != NULL && type->oneofs > 0) {
        if (type->oneofs > (uint32_t)(sizeof(oneof_stack) /
                                      sizeof(*oneof_last))) {
            oneof_last =
                (uint32_t *)calloc(type->oneofs, sizeof(*oneof_last));
            if (oneof_last == NULL) {
                free(seen);
                return PBW_ERR_NOMEM;
            }
            oneof_heap = 1;
        } else {
            memset(oneof_stack, 0, type->oneofs * sizeof(*oneof_last));
        }
    }
    r.p = data + off;
    r.end = r.p + len;
    while (r.p < r.end) {
        uint32_t fnum;
        unsigned wt;
        field_t *f = NULL;

        if (!rd_tag(&r, &fnum, &wt)) {
            status = PBW_ERR_INVALID;
            goto done;
        }
        if (m == NULL && wt != 2 && !type->has_required) {
            /* Validating only: a non-delimited value holds no fields, so only
             * the bytes matter. */
            if (!rd_skip(&r, fnum, wt, depth)) {
                status = PBW_ERR_INVALID;
                goto done;
            }
            continue;
        }
        f = type_field_by_number(type, fnum);
        if (f == NULL || f->unsupported ||
            (wt != (unsigned)f->wire &&
             !(wt == 2 && f->repeated && packable((pbw_type)f->type)))) {
            if (!rd_skip(&r, fnum, wt, depth)) {
                status = PBW_ERR_INVALID;
                goto done;
            }
            continue;
        }
        if (seen != NULL && f->required) {
            seen[f->index] = 1;
        }
        if (wt == 2) {
            uint64_t plen;
            size_t payload;
            uint32_t n = 1;

            if (!rd_varint(&r, &plen) || plen > (uint64_t)(r.end - r.p)) {
                status = PBW_ERR_INVALID;
                goto done;
            }
            payload = (size_t)(r.p - data);
            if (f->type == PBW_TYPE_MESSAGE) {
                type_t *sub = NULL;

                TypedData_Get_Struct(f->sub, type_t, &type_data_type, sub);
                status = scan_message(NULL, sub, data, payload, (size_t)plen,
                                      depth + 1, missing);
                if (status != PBW_OK) {
                    goto done;
                }
            } else if (f->wire != 2) {
                long count = scan_run(f, r.p, (size_t)plen);

                if (count < 0) {
                    status = PBW_ERR_INVALID;
                    goto done;
                }
                n = (uint32_t)count;
            }
            r.p += (size_t)plen;
            if (m != NULL &&
                !slice_add(m, f, payload, (size_t)plen, n, 1, oneof_last)) {
                status = PBW_ERR_NOMEM;
                goto done;
            }
            continue;
        }
        {
            size_t value_off = (size_t)(r.p - data);

            if (!rd_skip(&r, fnum, wt, depth)) {
                status = PBW_ERR_INVALID;
                goto done;
            }
            if (m != NULL &&
                !slice_add(m, f, value_off, 0, 1, 0, oneof_last)) {
                status = PBW_ERR_NOMEM;
                goto done;
            }
        }
    }
    if (seen != NULL) {
        for (i = 0; i < type->count; i++) {
            if (type->fields[i].required && !seen[i]) {
                *missing = pbw_field_name(
                    pbw_descriptor_field_at(type->desc, i));
                status = PBW_ERR_REQUIRED;
                goto done;
            }
        }
    }

done:
    if (oneof_heap) {
        free(oneof_last);
    }
    free(seen);
    return status;
}

static void
scan_check(pbw_status status, type_t *type, const char *missing)
{
    switch (status) {
    case PBW_OK:
        return;
    case PBW_ERR_NOMEM:
        rb_raise(eError, "out of memory");
        return;
    case PBW_ERR_LIMIT:
        rb_raise(eError, "%s: nesting limit exceeded", type_name(type));
        return;
    case PBW_ERR_REQUIRED:
        rb_raise(eError, "%s: required field %s is missing", type_name(type),
                 missing != NULL ? missing : "?");
        return;
    default:
        rb_raise(eError, "%s: invalid serialized data", type_name(type));
        return;
    }
}

/* ----------------------------------------------------------- wire -> Ruby */

static VALUE
scalar_to_ruby(uint8_t type, uint64_t bits)
{
    switch ((pbw_type)type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_ENUM:
        return LL2NUM((int32_t)bits);
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32:
        return ULL2NUM((uint32_t)bits);
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
        return LL2NUM((int64_t)bits);
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64:
        return ULL2NUM(bits);
    case PBW_TYPE_BOOL:
        return bits != 0 ? Qtrue : Qfalse;
    case PBW_TYPE_FLOAT: {
        uint32_t u = (uint32_t)bits;
        float f;

        memcpy(&f, &u, 4);
        return rb_float_new((double)f);
    }
    case PBW_TYPE_DOUBLE: {
        double d;

        memcpy(&d, &bits, 8);
        return rb_float_new(d);
    }
    default:
        rb_raise(eError, "unsupported scalar type");
        return Qnil;
    }
}

static uint64_t
unzigzag(uint64_t value, unsigned width)
{
    uint64_t mask = width == 32 ? 0xffffffffull : ~0ull;

    value &= mask;
    return ((value >> 1) ^ (0ull - (value & 1))) & mask;
}

static VALUE
string_from(uint8_t type, const uint8_t *p, size_t len)
{
    if (type == PBW_TYPE_STRING) {
        return rb_utf8_str_new((const char *)p, (long)len);
    }
    return rb_str_new((const char *)p, (long)len);
}

/* One scalar at `p`, bounded by `end`; `used` reports the bytes consumed. */
static VALUE
scalar_at(field_t *f, const uint8_t *p, const uint8_t *end, size_t *used)
{
    rdr r;
    uint64_t bits = 0;

    r.p = p;
    r.end = end;
    if (f->wire == 0) {
        if (!rd_varint(&r, &bits)) {
            rb_raise(eError, "invalid serialized data");
        }
        if (f->type == PBW_TYPE_SINT32 || f->type == PBW_TYPE_SINT64) {
            bits = unzigzag(bits, f->type == PBW_TYPE_SINT32 ? 32 : 64);
        }
    } else if (!rd_fixed(&r, &bits, f->wire == 1 ? 8 : 4)) {
        rb_raise(eError, "invalid serialized data");
    }
    if (used != NULL) {
        *used = (size_t)(r.p - p);
    }
    return scalar_to_ruby(f->type, bits);
}

static VALUE message_wire_new(VALUE type, VALUE buffer, size_t off, size_t len,
                              uint32_t depth);
static VALUE field_value(message_t *m, field_t *f);

/* One repeated slice: a single unpacked value, or a whole packed run. */
static void
push_slice(message_t *m, field_t *f, const slice_t *s, VALUE array)
{
    const uint8_t *p = m->data + s->off;
    const uint8_t *end = s->delimited ? p + s->len : m->data + m->off + m->len;

    for (;;) {
        size_t used = 0;

        rb_ary_push(array, scalar_at(f, p, end, &used));
        if (!s->delimited) {
            return;
        }
        p += used;
        if (p >= end) {
            return;
        }
    }
}

/* A singular message field that appears more than once merges: the payloads
 * concatenate, which is what parsing them as one message means. */
static VALUE
joined_message(message_t *m, field_t *f)
{
    size_t total = 0;
    size_t i;
    VALUE buffer;
    char *dst;

    for (i = 0; i < m->nslices; i++) {
        if (m->slices[i].fidx == f->index) {
            total += m->slices[i].len;
        }
    }
    buffer = rb_str_new(NULL, (long)total);
    dst = RSTRING_PTR(buffer);
    for (i = 0; i < m->nslices; i++) {
        if (m->slices[i].fidx == f->index) {
            memcpy(dst, m->data + m->slices[i].off, m->slices[i].len);
            dst += m->slices[i].len;
        }
    }
    return message_wire_new(f->sub, rb_str_new_frozen(buffer), 0, total,
                            m->depth + 1);
}

static VALUE
field_value(message_t *m, field_t *f)
{
    ssize_t last = -1;
    uint32_t i;

    if (!f->repeated) {
        for (i = m->nslices; i > 0; i--) {
            if (m->slices[i - 1].fidx == f->index) {
                last = (ssize_t)(i - 1);
                break;
            }
        }
        if (last < 0) {
            return Qnil;
        }
        if (f->type == PBW_TYPE_MESSAGE) {
            uint32_t n = 0;

            for (i = 0; i < m->nslices; i++) {
                n += m->slices[i].fidx == f->index;
            }
            if (n > 1) {
                return joined_message(m, f);
            }
            return message_wire_new(f->sub, m->buffer,
                                    m->slices[last].off, m->slices[last].len,
                                    m->depth + 1);
        }
        if (f->type == PBW_TYPE_STRING || f->type == PBW_TYPE_BYTES) {
            return string_from(f->type, m->data + m->slices[last].off,
                               m->slices[last].len);
        }
        return scalar_at(f, m->data + m->slices[last].off,
                         m->data + m->off + m->len, NULL);
    }

    {
        uint32_t count = 0;
        size_t total = 0;

        for (i = 0; i < m->nslices; i++) {
            if (m->slices[i].fidx == f->index) {
                count++;
                total += m->slices[i].n;
            }
        }
        if (count == 0) {
            return rb_ary_new();
        }
        {
            VALUE array = rb_ary_new_capa((long)total);

            for (i = 0; i < m->nslices; i++) {
                const slice_t *s = &m->slices[i];

                if (s->fidx != f->index) {
                    continue;
                }
                if (f->type == PBW_TYPE_MESSAGE) {
                    rb_ary_push(array, message_wire_new(f->sub, m->buffer,
                                                        s->off, s->len,
                                                        m->depth + 1));
                } else if (f->type == PBW_TYPE_STRING ||
                           f->type == PBW_TYPE_BYTES) {
                    rb_ary_push(array,
                                string_from(f->type, m->data + s->off, s->len));
                } else {
                    push_slice(m, f, s, array);
                }
            }
            return array;
        }
    }
}

/* ------------------------------------------------------ message lifecycle */

static VALUE
message_wire_new(VALUE type, VALUE buffer, size_t off, size_t len,
                 uint32_t depth)
{
    message_t *message;
    type_t *t;
    const char *missing = NULL;
    pbw_status status;
    VALUE obj;

    TypedData_Get_Struct(type, type_t, &type_data_type, t);
    obj = typed_object_alloc(cMessage, &message_data_type,
                                    (void **)&message);
    message->kind = MSG_WIRE;
    message->type = type;
    message->buffer = buffer;
    message->data = (const uint8_t *)RSTRING_PTR(buffer);
    message->off = off;
    message->len = len;
    message->depth = depth;
    status = scan_message(message, t, message->data, off, len, depth, &missing);
    if (status != PBW_OK) {
        scan_check(status, t, missing);
    }
    return obj;
}

/* A builder: an empty C message of the type. */
static VALUE message_builder_new(VALUE type, pbw_holder *holder,
                                 pbw_message *tree, VALUE parent, int owned);

/* A sub-message reached through a builder: the parent owns the tree, so the
 * Ruby object only borrows it (and marks the parent alive). */
static VALUE
message_borrowed(VALUE parent, VALUE type, pbw_message *tree)
{
    message_t *message;
    message_t *owner;
    VALUE obj = typed_object_alloc(cMessage, &message_data_type,
                                          (void **)&message);

    TypedData_Get_Struct(parent, message_t, &message_data_type, owner);
    message->kind = MSG_BUILDER;
    message->type = type;
    message->tree = tree;
    message->holder = owner->holder;
    message->parent = parent;
    message->owned = 0;
    holder_ref(owner->holder);
    return obj;
}

static VALUE
message_builder_new(VALUE type, pbw_holder *holder, pbw_message *tree,
                    VALUE parent, int owned)
{
    message_t *message;
    VALUE obj = typed_object_alloc(cMessage, &message_data_type,
                                          (void **)&message);

    message->kind = MSG_BUILDER;
    message->type = type;
    message->tree = tree;
    message->holder = holder;
    message->parent = parent;
    message->owned = owned;
    holder_ref(holder);
    return obj;
}

static pbw_message *
message_tree(VALUE obj, message_t **out)
{
    message_t *message;
    VALUE parent;

    TypedData_Get_Struct(obj, message_t, &message_data_type, message);
    if (out != NULL) {
        *out = message;
    }
    if (message->kind == MSG_CLOSED) {
        rb_raise(eError, "%s: message is closed",
                 type_name(message_type_of(obj)));
    }
    if (message->kind != MSG_BUILDER) {
        rb_raise(eError, "%s: decoded messages are read-only",
                 type_name(message_type_of(obj)));
    }
    parent = message->parent;
    while (!NIL_P(parent)) {
        message_t *owner;

        TypedData_Get_Struct(parent, message_t, &message_data_type, owner);
        if (owner->kind != MSG_BUILDER) {
            rb_raise(eError, "%s: parent message is closed",
                     type_name(message_type_of(obj)));
        }
        parent = owner->parent;
    }
    return message->tree;
}

/* Drops this object's view of the tree without freeing it (hand-over). */
static void
message_forget(message_t *message)
{
    pbw_holder *holder = message->holder;

    message->tree = NULL;
    message->holder = NULL;
    message->parent = Qnil;
    message->kind = MSG_CLOSED;
    holder_release(holder);
}

/* ---------------------------------------------------- builder: Ruby -> core */

static void
value_error(VALUE owner, field_t *f, const char *expected)
{
    rb_raise(rb_eTypeError, "%s.%s: expected %s",
             type_name(message_type_of(owner)), RSTRING_PTR(f->name), expected);
}

/* Everything value_build() would reject, checked before any value is allocated,
 * so a bad element cannot leave a half-applied repeated field behind. */
static void
validate_value(VALUE owner, field_t *f, VALUE value)
{
    switch ((pbw_type)f->type) {
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
        if (!RB_TYPE_P(value, T_STRING)) {
            value_error(owner, f, "a String");
        }
        return;
    case PBW_TYPE_MESSAGE: {
        message_t *sub;

        if (!rb_obj_is_kind_of(value, cMessage)) {
            value_error(owner, f, "a message");
        }
        if (value == owner) {
            rb_raise(eError, "a message cannot contain itself");
        }
        TypedData_Get_Struct(value, message_t, &message_data_type, sub);
        if (sub->kind == MSG_CLOSED || sub->tree == NULL) {
            rb_raise(eError, "message is closed");
        }
        if (!sub->owned) {
            rb_raise(eError,
                     "a borrowed sub-message cannot be moved to another "
                     "message");
        }
        if (sub->type != f->sub) {
            type_t *subtype;

            TypedData_Get_Struct(f->sub, type_t, &type_data_type, subtype);
            rb_raise(rb_eTypeError, "%s.%s: expected a %s message",
                     type_name(message_type_of(owner)), RSTRING_PTR(f->name),
                     type_name(subtype));
        }
        return;
    }
    case PBW_TYPE_GROUP:
        rb_raise(eError, "%s: group fields are not supported",
                 RSTRING_PTR(f->name));
        return;
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_DOUBLE:
        if (!RB_INTEGER_TYPE_P(value) && !RB_FLOAT_TYPE_P(value)) {
            value_error(owner, f, "a Numeric");
        }
        return;
    case PBW_TYPE_BOOL:
        if (value != Qtrue && value != Qfalse && !NIL_P(value)) {
            value_error(owner, f, "true or false");
        }
        return;
    default:
        if (!RB_INTEGER_TYPE_P(value)) {
            value_error(owner, f, "an Integer");
        }
        return;
    }
}

/* A value the caller either stores or frees. NULL means allocation failure, or
 * - when `moved` is set - a message that was already handed to another field by
 * an earlier element of the same repeated assignment. */
static pbw_value *
value_build(field_t *f, VALUE value, int *moved)
{
    switch ((pbw_type)f->type) {
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
        StringValue(value);
        return pbw_value_new_bytes(RSTRING_PTR(value),
                                   (size_t)RSTRING_LEN(value));
    case PBW_TYPE_MESSAGE: {
        message_t *sub;
        pbw_message *tree;
        pbw_value *built;

        TypedData_Get_Struct(value, message_t, &message_data_type, sub);
        tree = sub->tree;
        if (tree == NULL) {
            *moved = 1;
            return NULL;
        }
        built = pbw_value_new_message(tree);
        if (built == NULL) {
            pbw_message_free(tree);
            return NULL;
        }
        message_forget(sub);
        return built;
    }
    case PBW_TYPE_FLOAT:
        return pbw_value_new_float((float)NUM2DBL(value));
    case PBW_TYPE_DOUBLE:
        return pbw_value_new_double(NUM2DBL(value));
    case PBW_TYPE_BOOL:
        return pbw_value_new_bool(value == Qtrue ? 1 : 0);
    case PBW_TYPE_INT32:
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_SFIXED64:
    case PBW_TYPE_ENUM:
        return pbw_value_new_int(NUM2LL(value));
    default:
        return pbw_value_new_uint(NUM2ULL(value));
    }
}

/* --------------------------------------------------------- Ruby methods */

/* One value of a builder's tree as the Ruby value of its field. */
static VALUE
item_to_ruby(VALUE owner, field_t *f, pbw_value *value)
{
    switch ((pbw_type)f->type) {
    case PBW_TYPE_MESSAGE: {
        pbw_message *child = pbw_value_get_message(value);

        return child == NULL ? Qnil : message_borrowed(owner, f->sub, child);
    }
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES: {
        size_t len = 0;
        const uint8_t *data = (const uint8_t *)pbw_value_get_bytes(value, &len);

        return string_from(f->type, data, len);
    }
    case PBW_TYPE_FLOAT:
        return rb_float_new((double)pbw_value_get_float(value));
    case PBW_TYPE_DOUBLE:
        return rb_float_new(pbw_value_get_double(value));
    case PBW_TYPE_BOOL:
        return pbw_value_get_bool(value) ? Qtrue : Qfalse;
    case PBW_TYPE_INT32:
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_SFIXED64:
    case PBW_TYPE_ENUM:
        return LL2NUM(pbw_value_get_int(value));
    default:
        return ULL2NUM(pbw_value_get_uint(value));
    }
}

static VALUE
schema_s_load(VALUE klass, VALUE bytes)
{
    pbw_schema *schema = NULL;
    pbw_status status;
    schema_t *self;
    VALUE obj;

    StringValue(bytes);
    status = pbw_descriptor_set_load(RSTRING_PTR(bytes),
                                     (size_t)RSTRING_LEN(bytes), &schema);
    if (status != PBW_OK) {
        const char *reason = pbw_schema_error(schema);
        pbw_schema_free(schema);
        rb_raise(eError, "descriptor set rejected: %s",
                 reason != NULL && reason[0] != '\0' ? reason
                                                     : pbw_strerror(status));
    }
    obj = typed_object_alloc(klass, &schema_data_type, (void **)&self);
    self->holder = holder_new(schema);
    self->types = Qnil;
    self->closed = 0;
    return obj;
}

static VALUE
schema_s_from_file(VALUE klass, VALUE path)
{
    VALUE bytes = rb_funcall(rb_cFile, rb_intern2("binread", 7), 1, path);

    return schema_s_load(klass, bytes);
}

static VALUE
schema_message(VALUE self, VALUE name)
{
    pbw_schema *schema = schema_ptr(self);
    const pbw_descriptor *desc;
    pbw_message *tree;
    schema_t *own;
    VALUE type;

    StringValue(name);
    desc = pbw_schema_find(schema, StringValueCStr(name));
    if (desc == NULL || pbw_descriptor_is_enum(desc)) {
        rb_raise(eError, "no message %" PRIsVALUE " in the schema", name);
    }
    tree = pbw_message_new(desc);
    if (tree == NULL) {
        rb_raise(eError, "out of memory");
    }
    TypedData_Get_Struct(self, schema_t, &schema_data_type, own);
    type = type_for(self, desc);
    return message_builder_new(type, own->holder, tree, Qnil, 1);
}

static VALUE
schema_close(VALUE self)
{
    schema_t *schema;

    TypedData_Get_Struct(self, schema_t, &schema_data_type, schema);
    if (!schema->closed && schema->holder != NULL) {
        schema->closed = 1;
        schema->types = Qnil;
        holder_close(schema->holder);
    }
    return self;
}

static VALUE
schema_closed_p(VALUE self)
{
    schema_t *schema;

    TypedData_Get_Struct(self, schema_t, &schema_data_type, schema);
    return schema->closed ? Qtrue : Qfalse;
}

static VALUE
schema_inspect(VALUE self)
{
    schema_t *schema;

    TypedData_Get_Struct(self, schema_t, &schema_data_type, schema);
    return rb_sprintf("#<%" PRIsVALUE " %s>", rb_obj_class(self),
                      schema->closed ? "closed" : "open");
}

static VALUE
message_type(VALUE self)
{
    message_t *message;

    TypedData_Get_Struct(self, message_t, &message_data_type, message);
    if (message->kind == MSG_CLOSED) {
        rb_raise(eError, "%s: message is closed",
                 type_name(message_type_of(self)));
    }
    return rb_utf8_str_new_cstr(type_name(message_type_of(self)));
}

static VALUE
message_set(VALUE self, VALUE key, VALUE value)
{
    message_t *message;
    pbw_message *tree = message_tree(self, &message);
    type_t *type = message_type_of(self);
    field_t *f = field_lookup(type, key);
    const pbw_field *descriptor_field =
        pbw_descriptor_field_at(type->desc, (size_t)f->index);
    pbw_status status;
    int moved = 0;

    if (!f->repeated) {
        pbw_value *built;

        validate_value(self, f, value);
        built = value_build(f, value, &moved);
        if (built == NULL) {
            if (moved) {
                rb_raise(eError, "%s.%s: the message was already moved",
                         type_name(type), RSTRING_PTR(f->name));
            }
            rb_raise(eError, "out of memory");
        }
        status = pbw_message_set_field(tree, descriptor_field, built);
        if (status != PBW_OK) {
            pbw_value_free(built);
            rb_raise(eError, "%s.%s: %s", type_name(type),
                     RSTRING_PTR(f->name), pbw_strerror(status));
        }
        return self;
    }
    {
        long count;
        long i;
        pbw_value **values;

        Check_Type(value, T_ARRAY);
        count = RARRAY_LEN(value);
        for (i = 0; i < count; i++) {
            validate_value(self, f, RARRAY_AREF(value, i));
        }
        values = count > 0 ? ALLOC_N(pbw_value *, (size_t)count) : NULL;
        for (i = 0; i < count; i++) {
            values[i] = value_build(f, RARRAY_AREF(value, i), &moved);
            if (values[i] == NULL) {
                long j;

                for (j = 0; j < i; j++) {
                    pbw_value_free(values[j]);
                }
                xfree(values);
                if (moved) {
                    rb_raise(eError, "%s.%s: the message was already moved",
                             type_name(type), RSTRING_PTR(f->name));
                }
                rb_raise(eError, "out of memory");
            }
        }
        pbw_message_clear(tree, f->number);
        for (i = 0; i < count; i++) {
            status = pbw_message_set_field(tree, descriptor_field, values[i]);
            if (status != PBW_OK) {
                long j;

                for (j = i; j < count; j++) {
                    pbw_value_free(values[j]);
                }
                xfree(values);
                rb_raise(eError, "%s.%s: %s", type_name(type),
                         RSTRING_PTR(f->name), pbw_strerror(status));
            }
        }
        xfree(values);
        return self;
    }
}

static VALUE
message_get(VALUE self, VALUE key)
{
    message_t *message;

    TypedData_Get_Struct(self, message_t, &message_data_type, message);
    if (message->kind == MSG_CLOSED) {
        rb_raise(eError, "%s: message is closed",
                 type_name(message_type_of(self)));
    }
    {
        type_t *type = message_type_of(self);
        field_t *f = field_lookup(type, key);

        if (f->unsupported) {
            rb_raise(eError, "%s: group fields are not supported",
                     RSTRING_PTR(f->name));
        }
        if (message->kind == MSG_WIRE) {
            return field_value(message, f);
        }
        {
            /* Also checks that no parent this builder was borrowed from has
             * been closed under it. */
            pbw_message *tree = message_tree(self, &message);
            pbw_value *value = pbw_message_get_field(
                tree, pbw_descriptor_field_at(type->desc, (size_t)f->index));

            if (f->repeated) {
                VALUE array;
                size_t count;
                size_t i;

                if (value == NULL) {
                    return rb_ary_new();
                }
                count = pbw_value_count(value);
                array = rb_ary_new_capa((long)count);
                for (i = 0; i < count; i++) {
                    rb_ary_push(array,
                                item_to_ruby(self, f, pbw_value_at(value, i)));
                }
                return array;
            }
            if (value == NULL) {
                return Qnil;
            }
            return item_to_ruby(self, f, value);
        }
    }
}

static VALUE
message_encode(VALUE self)
{
    message_t *message;

    TypedData_Get_Struct(self, message_t, &message_data_type, message);
    if (message->kind == MSG_CLOSED) {
        rb_raise(eError, "%s: message is closed",
                 type_name(message_type_of(self)));
    }
    if (message->kind == MSG_WIRE) {
        VALUE out = rb_str_new((const char *)message->data + message->off,
                              (long)message->len);

        return out;
    }
    {
        pbw_message *tree = message_tree(self, &message);
        size_t cap = message->enc_cap;
        size_t wire = pbw_message_wire_size(tree);

        if (cap == 0) {
            cap = wire > 0 ? wire + 32 : 256;
        } else {
            cap += 32;
        }
        for (;;) {
            VALUE out = rb_str_buf_new((long)cap);
            size_t len = 0;
            pbw_status status =
                pbw_encode_to(tree, RSTRING_PTR(out), cap, &len);

            if (status == PBW_OK) {
                rb_str_set_len(out, (long)len);
                message->enc_cap = len;
                return out;
            }
            if (status != PBW_ERR_LIMIT) {
                rb_raise(eError, "%s: %s",
                         type_name(message_type_of(self)),
                         pbw_strerror(status));
            }
            if (cap > ((size_t)-1) / 2) {
                rb_raise(eError, "%s: message too large to encode",
                         type_name(message_type_of(self)));
            }
            cap *= 2;
        }
    }
}

static VALUE
message_decode(VALUE self, VALUE bytes)
{
    message_t *message;
    VALUE buffer;

    TypedData_Get_Struct(self, message_t, &message_data_type, message);
    if (message->kind == MSG_CLOSED) {
        rb_raise(eError, "%s: message is closed",
                 type_name(message_type_of(self)));
    }
    StringValue(bytes);
    buffer = rb_str_new_frozen(bytes);
    return message_wire_new(message->type, buffer, 0,
                            (size_t)RSTRING_LEN(buffer), 0);
}

static VALUE
message_close(VALUE self)
{
    message_t *message;

    TypedData_Get_Struct(self, message_t, &message_data_type, message);
    if (message->kind == MSG_BUILDER) {
        if (message->owned && message->tree != NULL) {
            pbw_message_free(message->tree);
        }
        message->tree = NULL;
        holder_release(message->holder);
        message->holder = NULL;
        message->parent = Qnil;
    }
    if (message->slices != message->inline_slices) {
        free(message->slices);
        message->slices = message->inline_slices;
        message->nslices = 0;
        message->cap = SLICE_INLINE;
    }
    message->buffer = Qnil;
    message->data = NULL;
    message->kind = MSG_CLOSED;
    return self;
}

static VALUE
message_closed_p(VALUE self)
{
    message_t *message;

    TypedData_Get_Struct(self, message_t, &message_data_type, message);
    return message->kind == MSG_CLOSED ? Qtrue : Qfalse;
}

static VALUE
message_inspect(VALUE self)
{
    message_t *message;

    TypedData_Get_Struct(self, message_t, &message_data_type, message);
    if (message->kind == MSG_CLOSED) {
        return rb_sprintf("#<%" PRIsVALUE " closed>", rb_obj_class(self));
    }
    return rb_sprintf("#<%" PRIsVALUE " %s>", rb_obj_class(self),
                      type_name(message_type_of(self)));
}

/* ------------------------------------------------------------- extension */

void
Init_pbwire_ext(void)
{
    mPbwire = rb_define_module("Pbwire");
    eError = rb_define_class_under(mPbwire, "Error", rb_eStandardError);

    rb_define_const(mPbwire, "ABI_VERSION", INT2NUM(PBW_ABI_VERSION));

    cSchema = rb_define_class_under(mPbwire, "Schema", rb_cObject);
    rb_define_alloc_func(cSchema, schema_alloc);
    rb_define_singleton_method(cSchema, "from_bytes", schema_s_load, 1);
    rb_define_singleton_method(cSchema, "from_file", schema_s_from_file, 1);
    rb_define_method(cSchema, "message", schema_message, 1);
    rb_define_method(cSchema, "close", schema_close, 0);
    rb_define_method(cSchema, "release", schema_close, 0);
    rb_define_method(cSchema, "closed?", schema_closed_p, 0);
    rb_define_method(cSchema, "inspect", schema_inspect, 0);
    rb_define_method(cSchema, "to_s", schema_inspect, 0);

    cType = rb_define_class_under(mPbwire, "Type", rb_cObject);
    rb_define_alloc_func(cType, type_alloc);

    cMessage = rb_define_class_under(mPbwire, "Message", rb_cObject);
    rb_define_alloc_func(cMessage, message_alloc);
    rb_define_method(cMessage, "type", message_type, 0);
    rb_define_method(cMessage, "set", message_set, 2);
    rb_define_method(cMessage, "[]=", message_set, 2);
    rb_define_method(cMessage, "get", message_get, 1);
    rb_define_method(cMessage, "[]", message_get, 1);
    rb_define_method(cMessage, "encode", message_encode, 0);
    rb_define_method(cMessage, "decode", message_decode, 1);
    rb_define_method(cMessage, "close", message_close, 0);
    rb_define_method(cMessage, "release", message_close, 0);
    rb_define_method(cMessage, "closed?", message_closed_p, 0);
    rb_define_method(cMessage, "inspect", message_inspect, 0);
    rb_define_method(cMessage, "to_s", message_inspect, 0);
}
