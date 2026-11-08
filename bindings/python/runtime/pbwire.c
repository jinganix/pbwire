/* CPython extension: the Python binding.
 *
 * Two representations, each converting straight to and from wire bytes -- the
 * pbwire message tree is never built:
 *
 *   dict     <->  wire    encode(dict) / Message.to_dict(): one pass over the
 *                         wire and CPython objects only.
 *   wire     ->   Message decode(): one tag scan that records a slice per wire
 *                         field, so a value materializes only when it is read.
 *
 * A Message keeps the bytes it was parsed from, so encode(message) hands those
 * bytes back and a decode/encode round trip preserves unknown fields, field
 * order and non-canonical encodings exactly -- which a dict round trip cannot.
 *
 * Per message type the field facts are resolved once (name, number, type, wire
 * type, packed/required/oneof, sub-message type), so the hot loops never touch
 * the descriptor again.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pbwire.h"

/* Same cap as the core's PBW_MAX_DEPTH (src/internal.h, not public). */
#define PY_MAX_DEPTH 256

/* ------------------------------------------------------------------- wire */

static size_t
varint_size(uint64_t value)
{
    size_t n = 1;

    while (value >= 0x80) {
        value >>= 7;
        n++;
    }
    return n;
}

static uint8_t *
put_varint(uint8_t *out, uint64_t value)
{
    while (value >= 0x80) {
        *out++ = (uint8_t)((value & 0x7f) | 0x80);
        value >>= 7;
    }
    *out++ = (uint8_t)value;
    return out;
}

static uint64_t
zigzag(uint64_t value, unsigned width)
{
    uint64_t mask = width == 32 ? 0xffffffffull : ~0ull;
    uint64_t sign = width == 32 ? 31 : 63;
    uint64_t u = value & mask;

    return ((u << 1) ^ (0ull - (u >> sign))) & mask;
}

static uint64_t
unzigzag(uint64_t value, unsigned width)
{
    uint64_t mask = width == 32 ? 0xffffffffull : ~0ull;

    /* Mask first: an overlong varint must truncate like the reference. */
    value &= mask;
    return ((value >> 1) ^ (0ull - (value & 1))) & mask;
}

static uint64_t
wire_value(pbw_type type, uint64_t bits)
{
    switch (type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_ENUM:
        return (uint64_t)(int64_t)(int32_t)bits;
    case PBW_TYPE_UINT32:
        return (uint32_t)bits;
    case PBW_TYPE_SINT32:
        return zigzag(bits, 32);
    case PBW_TYPE_SINT64:
        return zigzag(bits, 64);
    case PBW_TYPE_BOOL:
        return bits != 0 ? 1u : 0u;
    default:
        return bits;
    }
}

static unsigned
wire_type(pbw_type type)
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
    return type != PBW_TYPE_STRING && type != PBW_TYPE_BYTES &&
           type != PBW_TYPE_MESSAGE && type != PBW_TYPE_GROUP;
}

/* ----------------------------------------------------------------- writer */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    int failed; /* out of memory */
} sbuf;

static int
sb_reserve(sbuf *b, size_t extra)
{
    size_t need, cap;
    uint8_t *data;

    if (b->failed) {
        return 0;
    }
    need = b->len + extra;
    if (need <= b->cap) {
        return 1;
    }
    cap = b->cap == 0 ? 256 : b->cap;
    while (cap < need) {
        if (cap > SIZE_MAX - cap / 2) {
            cap = need;
            break;
        }
        cap += cap / 2;
    }
    data = realloc(b->data, cap);
    if (data == NULL) {
        b->failed = 1;
        return 0;
    }
    b->data = data;
    b->cap = cap;
    return 1;
}

/* All raw writers assume the caller reserved enough room. */
static void
sb_varint_raw(sbuf *b, uint64_t value)
{
    b->len = (size_t)(put_varint(b->data + b->len, value) - b->data);
}

static void
sb_tag_raw(sbuf *b, uint32_t fnum, unsigned wt)
{
    sb_varint_raw(b, ((uint64_t)fnum << 3) | wt);
}

static void
sb_fixed_raw(sbuf *b, uint64_t value, size_t width)
{
    uint8_t *out = b->data + b->len;

    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
    if (width == 8) {
        out[4] = (uint8_t)(value >> 32);
        out[5] = (uint8_t)(value >> 40);
        out[6] = (uint8_t)(value >> 48);
        out[7] = (uint8_t)(value >> 56);
    }
    b->len += width;
}

/* Patch the one-byte length placeholder: shift the payload right when the
 * length varint is longer. */
static void
sb_len_end(sbuf *b, size_t mark)
{
    size_t payload, n, shift;

    if (b->failed) {
        return;
    }
    payload = b->len - mark - 1;
    n = varint_size(payload);
    shift = n - 1;
    if (shift > 0) {
        if (!sb_reserve(b, shift)) {
            return;
        }
        memmove(b->data + mark + 1 + shift, b->data + mark + 1, payload);
        b->len += shift;
    }
    put_varint(b->data + mark, payload);
}

/* ----------------------------------------------------------------- reader */

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

static int
rd_tag(rdr *r, uint32_t *fnum, unsigned *wt)
{
    const uint8_t *start = r->p;
    uint64_t value;

    if (!rd_varint(r, &value)) {
        return 0;
    }
    if ((size_t)(r->p - start) > 5) { /* overlong tag varint */
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
        if (depth >= PY_MAX_DEPTH) {
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

/* Body of a group field: bytes from here to the matching END_GROUP tag.
 * Nested groups (known or skipped) are consumed whole by rd_skip; an
 * END_GROUP for another number is malformed. */
static int
rd_group_body(rdr *r, uint32_t fnum, unsigned depth, const uint8_t **body,
              size_t *body_len)
{
    const uint8_t *start = r->p;

    if (depth >= PY_MAX_DEPTH) {
        return 0;
    }
    for (;;) {
        const uint8_t *tag_start = r->p;
        uint32_t inner_fnum;
        unsigned inner_wt;

        if (!rd_tag(r, &inner_fnum, &inner_wt)) {
            return 0;
        }
        if (inner_wt == 4) {
            if (inner_fnum != fnum) {
                return 0;
            }
            *body = start;
            *body_len = (size_t)(tag_start - start);
            return 1;
        }
        if (!rd_skip(r, inner_fnum, inner_wt, depth + 1)) {
            return 0;
        }
    }
}

/* --------------------------------------------------------------- types */

typedef struct {
    PyObject *name;    /* owned, interned */
    const char *cname; /* UTF-8 view of name, owned by it */
    uint32_t number;
    uint8_t type;
    uint8_t wire;
    uint8_t repeated;
    uint8_t packed;
    uint8_t required;
    uint32_t oneof; /* 1-based oneof group, 0 = not in a oneof */
    PyObject *sub;  /* owned MessageType for MESSAGE fields */
} field_t;

typedef struct {
    PyObject_HEAD PyObject *name; /* owned */
    const char *name_utf8;
    PyObject *schema_obj; /* owned: keeps the schema (and desc) alive */
    const pbw_descriptor *desc;
    size_t nfields;
    field_t *fields;
    PyObject *by_name; /* owned dict: field name -> field index */
    uint32_t n_oneofs;
    int has_required;
} MessageType;

typedef struct {
    PyObject_HEAD pbw_schema *schema;
    PyObject *cache; /* owned dict: message full name -> MessageType */
} Schema;

static PyTypeObject SchemaType;
static PyTypeObject MessageTypeType;
static PyTypeObject MessagePyType;

static void
set_status(pbw_status status, const char *what)
{
    switch (status) {
    case PBW_ERR_NOMEM:
        PyErr_NoMemory();
        return;
    case PBW_ERR_REQUIRED:
        PyErr_Format(PyExc_ValueError, "%s: a required field is missing",
                     what);
        return;
    case PBW_ERR_LIMIT:
        PyErr_Format(PyExc_ValueError, "%s: nesting limit exceeded", what);
        return;
    default:
        PyErr_Format(PyExc_ValueError, "%s: invalid data", what);
        return;
    }
}

/* -------------------------------------------------------------- encoding */

static void
put_scalar(sbuf *b, uint8_t type, uint64_t bits)
{
    switch (type) {
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        sb_fixed_raw(b, bits, 4);
        return;
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        sb_fixed_raw(b, bits, 8);
        return;
    default:
        sb_varint_raw(b, wire_value((pbw_type)type, bits));
        return;
    }
}

/* Python value -> wire bits for one scalar. */
static int
as_bits(PyObject *v, uint8_t type, uint64_t *out, const char *name)
{
    switch (type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_ENUM: {
        long long x;

        if (!PyLong_Check(v)) {
            PyErr_Format(PyExc_TypeError, "field %s must be an int", name);
            return 0;
        }
        x = PyLong_AsLongLong(v);
        if (x == -1 && PyErr_Occurred()) {
            return 0;
        }
        if (x < INT32_MIN || x > INT32_MAX) {
            PyErr_Format(PyExc_OverflowError, "field %s out of int32 range",
                         name);
            return 0;
        }
        *out = (uint32_t)(int32_t)x;
        return 1;
    }
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32: {
        unsigned long long x;

        if (!PyLong_Check(v)) {
            PyErr_Format(PyExc_TypeError, "field %s must be an int", name);
            return 0;
        }
        x = PyLong_AsUnsignedLongLong(v);
        if (x == (unsigned long long)-1 && PyErr_Occurred()) {
            return 0;
        }
        if (x > 0xffffffffull) {
            PyErr_Format(PyExc_OverflowError, "field %s out of uint32 range",
                         name);
            return 0;
        }
        *out = (uint32_t)x;
        return 1;
    }
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64: {
        long long x;

        if (!PyLong_Check(v)) {
            PyErr_Format(PyExc_TypeError, "field %s must be an int", name);
            return 0;
        }
        x = PyLong_AsLongLong(v);
        if (x == -1 && PyErr_Occurred()) {
            return 0;
        }
        *out = (uint64_t)(int64_t)x;
        return 1;
    }
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64: {
        unsigned long long x;

        if (!PyLong_Check(v)) {
            PyErr_Format(PyExc_TypeError, "field %s must be an int", name);
            return 0;
        }
        x = PyLong_AsUnsignedLongLong(v);
        if (x == (unsigned long long)-1 && PyErr_Occurred()) {
            return 0;
        }
        *out = x;
        return 1;
    }
    case PBW_TYPE_BOOL: {
        int t = PyObject_IsTrue(v);

        if (t < 0) {
            return 0;
        }
        *out = t != 0;
        return 1;
    }
    case PBW_TYPE_FLOAT: {
        double d = PyFloat_AsDouble(v);
        float f;
        uint32_t bits;

        if (d == -1.0 && PyErr_Occurred()) {
            return 0;
        }
        f = (float)d;
        memcpy(&bits, &f, 4);
        *out = bits;
        return 1;
    }
    case PBW_TYPE_DOUBLE: {
        double d = PyFloat_AsDouble(v);
        uint64_t bits;

        if (d == -1.0 && PyErr_Occurred()) {
            return 0;
        }
        memcpy(&bits, &d, 8);
        *out = bits;
        return 1;
    }
    default:
        PyErr_Format(PyExc_TypeError, "field %s: unsupported type", name);
        return 0;
    }
}

/* unicode or bytes -> (pointer, length) valid until the object dies. */
static int
as_bytes(PyObject *v, const char **data, Py_ssize_t *len, const char *name)
{
    if (PyUnicode_Check(v)) {
        const char *s = PyUnicode_AsUTF8AndSize(v, len);

        if (s == NULL) {
            return 0;
        }
        *data = s;
        return 1;
    }
    if (PyBytes_Check(v)) {
        if (PyBytes_AsStringAndSize(v, (char **)data, len) < 0) {
            return 0;
        }
        return 1;
    }
    PyErr_Format(PyExc_TypeError, "field %s must be str or bytes", name);
    return 0;
}

static int enc_message(MessageType *mt, PyObject *obj, sbuf *b, unsigned depth);

/* Repeated fields are almost always lists: use the direct accessors instead of
 * wrapping them in the sequence protocol. */
#define SEQ_FAST(v, what) \
    (PyList_CheckExact(v) ? (Py_INCREF(v), (v)) : PySequence_Fast(v, what))
#define SEQ_SIZE(seq)                              \
    (PyList_CheckExact(seq) ? PyList_GET_SIZE(seq) \
                            : PySequence_Fast_GET_SIZE(seq))
#define SEQ_ITEM(seq, i)                              \
    (PyList_CheckExact(seq) ? PyList_GET_ITEM(seq, i) \
                            : PySequence_Fast_GET_ITEM(seq, i))

/* Room for one repeated scalar: a 5-byte tag plus a 10-byte varint. */
#define ENC_ELEMENT_ROOM 15

static int
enc_field(field_t *f, PyObject *v, sbuf *b, unsigned depth)
{
    /* Messages and groups share the body encoder: messages frame it with a
     * length prefix, groups with START_GROUP/END_GROUP tags. */
    if (f->type == PBW_TYPE_MESSAGE || f->type == PBW_TYPE_GROUP) {
        int group = f->type == PBW_TYPE_GROUP;

        if (f->repeated) {
            PyObject *seq = SEQ_FAST(v, "repeated field needs a sequence");
            Py_ssize_t i, n;

            if (seq == NULL) {
                return 0;
            }
            n = SEQ_SIZE(seq);
            for (i = 0; i < n; i++) {
                size_t mark = 0;

                /* The sub-message may grow the buffer: reserve per element. */
                if (!sb_reserve(b, group ? 10u : 6u)) {
                    Py_DECREF(seq);
                    return 0;
                }
                sb_tag_raw(b, f->number, group ? 3u : 2u);
                if (!group) {
                    mark = b->len;
                    b->data[b->len++] = 0;
                }
                if (!enc_message((MessageType *)f->sub, SEQ_ITEM(seq, i), b,
                                 depth + 1)) {
                    Py_DECREF(seq);
                    return 0;
                }
                if (group) {
                    sb_tag_raw(b, f->number, 4u);
                } else {
                    sb_len_end(b, mark);
                }
            }
            Py_DECREF(seq);
            return 1;
        }
        if (!sb_reserve(b, group ? 10u : 6u)) {
            return 0;
        }
        sb_tag_raw(b, f->number, group ? 3u : 2u);
        if (!group) {
            size_t mark = b->len;

            b->data[b->len++] = 0;
            if (!enc_message((MessageType *)f->sub, v, b, depth + 1)) {
                return 0;
            }
            sb_len_end(b, mark);
        } else {
            if (!enc_message((MessageType *)f->sub, v, b, depth + 1)) {
                return 0;
            }
            sb_tag_raw(b, f->number, 4u);
        }
        return 1;
    }

    if (f->type == PBW_TYPE_STRING || f->type == PBW_TYPE_BYTES) {
        const char *data;
        Py_ssize_t len;
        Py_ssize_t i = 0, n = 1;
        PyObject *seq = NULL;

        if (f->repeated) {
            seq = SEQ_FAST(v, "repeated field needs a sequence");
            if (seq == NULL) {
                return 0;
            }
            n = SEQ_SIZE(seq);
        }
        for (; i < n; i++) {
            if (!as_bytes(seq != NULL ? SEQ_ITEM(seq, i) : v, &data, &len,
                          f->cname)) {
                Py_XDECREF(seq);
                return 0;
            }
            if (!sb_reserve(b, 5 + 10 + (size_t)len)) {
                Py_XDECREF(seq);
                return 0;
            }
            sb_tag_raw(b, f->number, 2);
            sb_varint_raw(b, (uint64_t)len);
            memcpy(b->data + b->len, data, (size_t)len);
            b->len += (size_t)len;
        }
        Py_XDECREF(seq);
        return 1;
    }

    if (f->repeated) {
        PyObject *seq = SEQ_FAST(v, "repeated field needs a sequence");
        Py_ssize_t i, n;

        if (seq == NULL) {
            return 0;
        }
        n = SEQ_SIZE(seq);
        if (n == 0) {
            Py_DECREF(seq);
            return 1;
        }
        if ((size_t)n > (SIZE_MAX - 16) / ENC_ELEMENT_ROOM) {
            Py_DECREF(seq);
            PyErr_NoMemory();
            return 0;
        }
        if (f->packed && packable((pbw_type)f->type)) {
            size_t mark;

            if (!sb_reserve(b, 6 + (size_t)n * 10)) {
                Py_DECREF(seq);
                return 0;
            }
            sb_tag_raw(b, f->number, 2);
            mark = b->len;
            b->data[b->len++] = 0;
            for (i = 0; i < n; i++) {
                uint64_t bits;

                if (!as_bits(SEQ_ITEM(seq, i), f->type, &bits, f->cname)) {
                    Py_DECREF(seq);
                    return 0;
                }
                put_scalar(b, f->type, bits);
            }
            sb_len_end(b, mark);
            Py_DECREF(seq);
            return 1;
        }
        if (!sb_reserve(b, (size_t)n * ENC_ELEMENT_ROOM)) {
            Py_DECREF(seq);
            return 0;
        }
        for (i = 0; i < n; i++) {
            uint64_t bits;

            if (!as_bits(SEQ_ITEM(seq, i), f->type, &bits, f->cname)) {
                Py_DECREF(seq);
                return 0;
            }
            sb_tag_raw(b, f->number, f->wire);
            put_scalar(b, f->type, bits);
        }
        Py_DECREF(seq);
        return 1;
    }

    {
        uint64_t bits;

        if (!as_bits(v, f->type, &bits, f->cname)) {
            return 0;
        }
        if (!sb_reserve(b, ENC_ELEMENT_ROOM)) {
            return 0;
        }
        sb_tag_raw(b, f->number, f->wire);
        put_scalar(b, f->type, bits);
    }
    return 1;
}

static int
enc_message(MessageType *mt, PyObject *obj, sbuf *b, unsigned depth)
{
    size_t i, used = 0;

    if (depth >= PY_MAX_DEPTH) {
        PyErr_SetString(PyExc_ValueError, "nesting limit exceeded");
        return 0;
    }
    if (!PyDict_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "%s must be a dict", mt->name_utf8);
        return 0;
    }
    for (i = 0; i < mt->nfields; i++) {
        field_t *f = &mt->fields[i];
        PyObject *v = PyDict_GetItemWithError(obj, f->name);

        if (v == NULL) {
            if (PyErr_Occurred()) {
                return 0;
            }
            if (f->required) {
                PyErr_Format(PyExc_ValueError,
                             "%s: required field %s is missing",
                             mt->name_utf8, f->cname);
                return 0;
            }
            continue;
        }
        used++;
        if (v == Py_None) {
            if (f->required) {
                PyErr_Format(PyExc_ValueError,
                             "%s: required field %s is missing",
                             mt->name_utf8, f->cname);
                return 0;
            }
            continue;
        }
        if (!enc_field(f, v, b, depth)) {
            return 0;
        }
        if (b->failed) {
            PyErr_NoMemory();
            return 0;
        }
    }
    /* A key that is not a field would be dropped silently otherwise. */
    if (used != (size_t)PyDict_Size(obj)) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;

        while (PyDict_Next(obj, &pos, &key, &value)) {
            size_t k;
            int known = 0;

            for (k = 0; k < mt->nfields; k++) {
                if (mt->fields[k].name == key) {
                    known = 1;
                    break;
                }
            }
            if (!known) {
                PyErr_Format(PyExc_KeyError, "%s has no field %R",
                             mt->name_utf8, key);
                return 0;
            }
        }
    }
    return 1;
}

/* -------------------------------------------------------------- decoding */

/* Python value for one scalar; consumes nothing. */
static PyObject *
scalar_object(uint8_t type, uint64_t bits)
{
    switch (type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_ENUM:
        return PyLong_FromLongLong((int32_t)bits);
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32:
        return PyLong_FromUnsignedLong((uint32_t)bits);
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
        return PyLong_FromLongLong((int64_t)bits);
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64:
        return PyLong_FromUnsignedLongLong(bits);
    case PBW_TYPE_BOOL:
        return PyBool_FromLong(bits != 0);
    case PBW_TYPE_FLOAT: {
        uint32_t u = (uint32_t)bits;
        float f;

        memcpy(&f, &u, 4);
        return PyFloat_FromDouble((double)f);
    }
    case PBW_TYPE_DOUBLE: {
        double d;

        memcpy(&d, &bits, 8);
        return PyFloat_FromDouble(d);
    }
    default:
        PyErr_SetString(PyExc_ValueError, "unsupported scalar type");
        return NULL;
    }
}

static PyObject *
string_object(uint8_t type, const uint8_t *p, size_t len)
{
    if (type == PBW_TYPE_BYTES) {
        return PyBytes_FromStringAndSize((const char *)p, (Py_ssize_t)len);
    }
    {
        PyObject *s = PyUnicode_DecodeUTF8((const char *)p, (Py_ssize_t)len,
                                           NULL);

        if (s != NULL) {
            return s;
        }
        PyErr_Clear(); /* not UTF-8: hand the raw bytes back */
    }
    return PyBytes_FromStringAndSize((const char *)p, (Py_ssize_t)len);
}

/* Element count of a packed run, so the list can be allocated once: varints
 * end on a byte without the continuation bit, fixed-width runs divide. */
static Py_ssize_t
packed_count(unsigned wire, const uint8_t *p, size_t len)
{
    if (wire == 0) {
        const uint8_t *end = p + len;
        Py_ssize_t n = 0;

        while (p < end) {
            n += (*p & 0x80) == 0;
            p++;
        }
        return n;
    }
    {
        size_t width = wire == 1 ? 8 : 4;

        return len % width == 0 ? (Py_ssize_t)(len / width) : -1;
    }
}

/* Appends to the per-field list, creating and publishing it on first use. */
static int
append_list(PyObject *dict, PyObject **slot, PyObject *name, PyObject *value)
{
    if (*slot == NULL) {
        PyObject *list = PyList_New(0);

        if (list == NULL) {
            Py_DECREF(value);
            return 0;
        }
        if (PyDict_SetItem(dict, name, list) < 0) {
            Py_DECREF(list);
            Py_DECREF(value);
            return 0;
        }
        *slot = list; /* dict owns it now; this reference is ours */
        Py_DECREF(list);
    }
    if (PyList_Append(*slot, value) < 0) {
        Py_DECREF(value);
        return 0;
    }
    Py_DECREF(value);
    return 1;
}

/* Protobuf merge semantics: a singular message field that shows up twice on
 * the wire merges into what was already there instead of replacing it. */
static int
merge_into(PyObject *dst, PyObject *src)
{
    PyObject *key, *value;
    Py_ssize_t pos = 0;

    while (PyDict_Next(src, &pos, &key, &value)) {
        PyObject *prev = PyDict_GetItemWithError(dst, key);

        if (prev == NULL && PyErr_Occurred()) {
            return 0;
        }
        if (prev != NULL && PyDict_Check(prev) && PyDict_Check(value)) {
            if (!merge_into(prev, value)) {
                return 0;
            }
            continue;
        }
        if (prev != NULL && PyList_Check(prev) && PyList_Check(value)) {
            Py_ssize_t i, n = PyList_GET_SIZE(value);

            for (i = 0; i < n; i++) {
                if (PyList_Append(prev, PyList_GET_ITEM(value, i)) < 0) {
                    return 0;
                }
            }
            continue;
        }
        if (PyDict_SetItem(dst, key, value) < 0) {
            return 0;
        }
    }
    return 1;
}

/* One oneof member set on the wire clears the others. */
static int
clear_oneof(MessageType *mt, PyObject *dict, uint32_t oneof, size_t keep)
{
    size_t i;

    for (i = 0; i < mt->nfields; i++) {
        if (mt->fields[i].oneof != oneof || i == keep) {
            continue;
        }
        if (PyDict_DelItem(dict, mt->fields[i].name) < 0) {
            if (!PyErr_ExceptionMatches(PyExc_KeyError)) {
                return 0;
            }
            PyErr_Clear();
        }
    }
    return 1;
}

/* Stores one singular value (steals the reference), applying oneof clearing
 * and sub-message merging. */
static int
store_singular(MessageType *mt, PyObject *dict, size_t fidx, PyObject *value)
{
    field_t *f = &mt->fields[fidx];

    if (f->oneof != 0 && !clear_oneof(mt, dict, f->oneof, fidx)) {
        Py_DECREF(value);
        return 0;
    }
    if (f->type == PBW_TYPE_MESSAGE || f->type == PBW_TYPE_GROUP) {
        PyObject *prev = PyDict_GetItemWithError(dict, f->name);

        if (prev == NULL && PyErr_Occurred()) {
            Py_DECREF(value);
            return 0;
        }
        if (prev != NULL && PyDict_Check(prev)) {
            if (!merge_into(prev, value)) {
                Py_DECREF(value);
                return 0;
            }
            Py_DECREF(value);
            return 1;
        }
    }
    if (PyDict_SetItem(dict, f->name, value) < 0) {
        Py_DECREF(value);
        return 0;
    }
    Py_DECREF(value);
    return 1;
}

/* Lists of repeated fields are indexed per field; almost every message has
 * few enough fields for the stack to hold them. */
#define DEC_INLINE_LISTS 8

static PyObject *
dec_message(MessageType *mt, const uint8_t *wire, size_t wire_len,
            unsigned depth)
{
    PyObject *dict;
    PyObject *inline_lists[DEC_INLINE_LISTS];
    PyObject **lists;
    int lists_heap = 0;
    rdr r;
    size_t cursor = 0;

    if (depth >= PY_MAX_DEPTH) {
        PyErr_SetString(PyExc_ValueError, "nesting limit exceeded");
        return NULL;
    }
    dict = PyDict_New();
    if (dict == NULL) {
        return NULL;
    }
    if (mt->nfields <= DEC_INLINE_LISTS) {
        lists = inline_lists;
        memset(inline_lists, 0, mt->nfields * sizeof(*lists));
    } else {
        lists = calloc(mt->nfields, sizeof(*lists));
        if (lists == NULL) {
            Py_DECREF(dict);
            return PyErr_NoMemory();
        }
        lists_heap = 1;
    }
    r.p = wire;
    r.end = wire + wire_len;
    while (r.p < r.end) {
        uint32_t fnum;
        unsigned wt;
        field_t *f = NULL;
        size_t i, fidx = 0;

        if (!rd_tag(&r, &fnum, &wt)) {
            PyErr_SetString(PyExc_ValueError, "truncated or malformed tag");
            goto fail;
        }
        for (i = 0; i < mt->nfields; i++) {
            size_t k = cursor + i; /* < 2 * nfields: one conditional subtract */

            if (k >= mt->nfields) {
                k -= mt->nfields;
            }
            if (mt->fields[k].number == fnum) {
                f = &mt->fields[k];
                fidx = k;
                cursor = k;
                break;
            }
        }
        if (f == NULL ||
            (wt != f->wire && !(wt == 2 && f->repeated &&
                                packable((pbw_type)f->type)))) {
            if (!rd_skip(&r, fnum, wt, depth)) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
                goto fail;
            }
            continue;
        }

        if (f->type == PBW_TYPE_GROUP) {
            const uint8_t *body;
            size_t body_len;
            PyObject *value;

            if (!rd_group_body(&r, fnum, depth, &body, &body_len)) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
                goto fail;
            }
            value = dec_message((MessageType *)f->sub, body, body_len,
                                depth + 1);
            if (value == NULL) {
                goto fail;
            }
            if (f->repeated) {
                if (!append_list(dict, &lists[fidx], f->name, value)) {
                    goto fail;
                }
            } else if (!store_singular(mt, dict, fidx, value)) {
                goto fail;
            }
            continue;
        }

        if (f->type == PBW_TYPE_MESSAGE || f->type == PBW_TYPE_STRING ||
            f->type == PBW_TYPE_BYTES) {
            uint64_t len;
            PyObject *value;

            if (!rd_varint(&r, &len) || len > (uint64_t)(r.end - r.p)) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
                goto fail;
            }
            if (f->type == PBW_TYPE_MESSAGE) {
                value = dec_message((MessageType *)f->sub, r.p, (size_t)len,
                                    depth + 1);
            } else {
                value = string_object(f->type, r.p, (size_t)len);
            }
            if (value == NULL) {
                goto fail;
            }
            r.p += (size_t)len;
            if (f->repeated) {
                if (!append_list(dict, &lists[fidx], f->name, value)) {
                    goto fail;
                }
            } else if (!store_singular(mt, dict, fidx, value)) {
                goto fail;
            }
            continue;
        }

        if (wt == 2) {
            /* packed run: allocate the list once, fill it by slot */
            uint64_t len;
            Py_ssize_t n, pos = 0;
            PyObject *list;
            rdr inner;

            if (!rd_varint(&r, &len) || len > (uint64_t)(r.end - r.p)) {
                PyErr_SetString(PyExc_ValueError, "truncated packed field");
                goto fail;
            }
            list = lists[fidx];
            if (list == NULL) {
                n = packed_count(f->wire, r.p, (size_t)len);
                if (n < 0) {
                    PyErr_SetString(PyExc_ValueError,
                                    "packed field is not a whole number of "
                                    "elements");
                    goto fail;
                }
                if (n == 0) {
                    /* An empty run carries no element: the field stays absent
                     * (a later non-empty run still creates the list), so the
                     * eager dict agrees with the lazy view. */
                    r.p += (size_t)len;
                    continue;
                }
                list = PyList_New(n);
                if (list == NULL ||
                    PyDict_SetItem(dict, f->name, list) < 0) {
                    Py_XDECREF(list);
                    goto fail;
                }
                Py_DECREF(list); /* the dict owns it */
                lists[fidx] = list;
            } else {
                n = 0; /* already sized by an earlier run: append */
            }
            inner.p = r.p;
            inner.end = r.p + (size_t)len;
            while (inner.p < inner.end) {
                uint64_t bits;
                PyObject *value;

                if (f->wire == 0) {
                    if (!rd_varint(&inner, &bits)) {
                        PyErr_SetString(PyExc_ValueError,
                                        "truncated packed field");
                        goto fail;
                    }
                    if (f->type == PBW_TYPE_SINT32 ||
                        f->type == PBW_TYPE_SINT64) {
                        bits = unzigzag(bits, f->type == PBW_TYPE_SINT32 ? 32
                                                                         : 64);
                    }
                } else if (!rd_fixed(&inner, &bits, f->wire == 1 ? 8 : 4)) {
                    PyErr_SetString(PyExc_ValueError, "truncated packed field");
                    goto fail;
                }
                value = scalar_object(f->type, bits);
                if (value == NULL) {
                    goto fail;
                }
                if (pos < n) {
                    PyList_SET_ITEM(list, pos, value); /* steals the ref */
                    pos++;
                } else if (PyList_Append(list, value) < 0) {
                    Py_DECREF(value);
                    goto fail;
                } else {
                    Py_DECREF(value);
                }
            }
            r.p = inner.end;
            continue;
        }

        {
            uint64_t bits;
            PyObject *value;

            if (wt == 0) {
                if (!rd_varint(&r, &bits)) {
                    PyErr_SetString(PyExc_ValueError, "truncated field");
                    goto fail;
                }
                if (f->type == PBW_TYPE_SINT32 || f->type == PBW_TYPE_SINT64) {
                    bits = unzigzag(bits, f->type == PBW_TYPE_SINT32 ? 32 : 64);
                }
            } else if (!rd_fixed(&r, &bits, wt == 1 ? 8 : 4)) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
                goto fail;
            }
            value = scalar_object(f->type, bits);
            if (value == NULL) {
                goto fail;
            }
            if (f->repeated) {
                if (!append_list(dict, &lists[fidx], f->name, value)) {
                    goto fail;
                }
            } else if (!store_singular(mt, dict, fidx, value)) {
                goto fail;
            }
        }
    }

    if (mt->has_required) {
        size_t i;

        for (i = 0; i < mt->nfields; i++) {
            if (mt->fields[i].required &&
                !PyDict_Contains(dict, mt->fields[i].name)) {
                PyErr_Format(PyExc_ValueError,
                             "%s: required field %s is missing",
                             mt->name_utf8, mt->fields[i].cname);
                goto fail;
            }
        }
    }
    if (lists_heap) {
        free(lists);
    }
    return dict;

fail:
    if (lists_heap) {
        free(lists);
    }
    Py_DECREF(dict);
    return NULL;
}

/* ---------------------------------------------------------- lazy message */

/* One wire field: where its value lives and, for length-delimited fields,
 * where its payload is. Offsets are absolute, into the Message's buffer. */
typedef struct {
    uint32_t fidx;
    uint32_t off;
    uint32_t len;
    uint32_t delimited;
} slice_t;

#define SLICE_DEAD 0xffffffffu /* cleared by a later member of its oneof */
#define MSG_INLINE_SLICES 16

typedef struct {
    PyObject_HEAD PyObject *mtype; /* owned MessageType */
    PyObject *buffer;              /* owned bytes that own `data` */
    const uint8_t *data;
    uint32_t off; /* this message's wire range */
    uint32_t len;
    uint16_t depth;
    slice_t *slices; /* `inline_slices` until it outgrows them */
    uint32_t nslices;
    uint32_t cap;
    slice_t inline_slices[MSG_INLINE_SLICES];
} Message;

static int
msg_reserve(Message *m, uint32_t need)
{
    uint32_t cap;
    slice_t *grown;

    if (need <= m->cap) {
        return 1;
    }
    cap = m->cap;
    while (cap < need) {
        if (cap > UINT32_MAX / 2) {
            /* A slice count beyond 2^31 cannot be described; fail instead of
             * wrapping the capacity to zero. */
            return 0;
        }
        cap *= 2;
    }
    if (m->slices == m->inline_slices) {
        grown = malloc((size_t)cap * sizeof(*grown));
        if (grown != NULL) {
            memcpy(grown, m->inline_slices,
                   (size_t)m->nslices * sizeof(*grown));
        }
    } else {
        grown = realloc(m->slices, (size_t)cap * sizeof(*grown));
    }
    if (grown == NULL) {
        PyErr_NoMemory();
        return 0;
    }
    m->slices = grown;
    m->cap = cap;
    return 1;
}

/* Records one field of this message, keeping at most one live slice per oneof
 * group. `oneof_last` holds index+1 of the group's live slice. */
static int
msg_add(Message *m, MessageType *mt, field_t *f, uint32_t off, uint32_t len,
        int delimited, uint32_t *oneof_last)
{
    uint32_t slot = f->oneof == 0 ? 0 : f->oneof - 1;
    uint32_t fidx = (uint32_t)(f - mt->fields);

    if (f->oneof != 0 && oneof_last[slot] != 0 &&
        m->slices[oneof_last[slot] - 1].fidx != fidx) {
        /* Another member of the group: it is gone. Repeats of this same member
         * stay, because a message merges with itself. */
        m->slices[oneof_last[slot] - 1].fidx = SLICE_DEAD;
    }
    if (!msg_reserve(m, m->nslices + 1)) {
        return 0;
    }
    m->slices[m->nslices].fidx = fidx;
    m->slices[m->nslices].off = off;
    m->slices[m->nslices].len = len;
    m->slices[m->nslices].delimited = (uint32_t)delimited;
    m->nslices++;
    if (f->oneof != 0) {
        oneof_last[slot] = m->nslices;
    }
    return 1;
}

/* A packed run must hold a whole number of complete values. */
static int
scan_run(field_t *f, const uint8_t *p, size_t len)
{
    rdr r;

    if (f->wire != 0) {
        return len % (f->wire == 1 ? 8 : 4) == 0;
    }
    r.p = p;
    r.end = p + len;
    while (r.p < r.end) {
        uint64_t value;

        if (!rd_varint(&r, &value)) {
            return 0;
        }
    }
    return 1;
}

/* One tag scan of one message range, every tag and length checked. With `m`
 * set it also records a slice per wire field; with `m` NULL it only validates,
 * which is how sub-messages are checked without being materialized. */
static int
scan_message(Message *m, MessageType *mt, const uint8_t *data, size_t off,
             size_t len, unsigned depth)
{
    uint32_t oneof_stack[16];
    uint32_t *oneof_last = oneof_stack;
    rdr r;
    size_t cursor = 0;

    if (depth >= PY_MAX_DEPTH) {
        PyErr_SetString(PyExc_ValueError, "nesting limit exceeded");
        return 0;
    }
    if (m != NULL) {
        if (mt->n_oneofs > (uint32_t)(sizeof(oneof_stack) /
                                      sizeof(*oneof_last))) {
            oneof_last = calloc(mt->n_oneofs, sizeof(*oneof_last));
            if (oneof_last == NULL) {
                PyErr_NoMemory();
                return 0;
            }
        } else if (mt->n_oneofs != 0) {
            memset(oneof_stack, 0, mt->n_oneofs * sizeof(*oneof_last));
        }
    }
    r.p = data + off;
    r.end = r.p + len;
    while (r.p < r.end) {
        uint32_t fnum;
        unsigned wt;
        field_t *f = NULL;
        size_t i;

        if (!rd_tag(&r, &fnum, &wt)) {
            PyErr_SetString(PyExc_ValueError, "truncated or malformed tag");
            goto fail;
        }
        if (m == NULL && wt != 2) {
            /* Validating only: a non-delimited value holds no fields, so what
             * the field is does not matter, only that its bytes are there. */
            if (!rd_skip(&r, fnum, wt, depth)) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
                goto fail;
            }
            continue;
        }
        for (i = 0; i < mt->nfields; i++) {
            size_t k = cursor + i; /* < 2 * nfields: one conditional subtract */

            if (k >= mt->nfields) {
                k -= mt->nfields;
            }
            if (mt->fields[k].number == fnum) {
                f = &mt->fields[k];
                cursor = k;
                break;
            }
        }
        if (f == NULL ||
            (wt != f->wire &&
             !(wt == 2 && f->repeated && packable((pbw_type)f->type)))) {
            if (!rd_skip(&r, fnum, wt, depth)) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
                goto fail;
            }
            continue;
        }
        if (f->type == PBW_TYPE_GROUP) {
            const uint8_t *body;
            size_t body_len;

            if (!rd_group_body(&r, fnum, depth, &body, &body_len) ||
                (uint64_t)(body - data) > 0xffffffffull ||
                (uint64_t)body_len > 0xffffffffull) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
                goto fail;
            }
            if (!scan_message(NULL, (MessageType *)f->sub, body, 0,
                              body_len, depth + 1)) {
                goto fail;
            }
            if (m != NULL &&
                !msg_add(m, mt, f, (uint32_t)(body - data),
                         (uint32_t)body_len, 1, oneof_last)) {
                goto fail;
            }
            continue;
        }
        if (wt == 2) {
            uint64_t plen;
            uint32_t payload;

            if (!rd_varint(&r, &plen) ||
                plen > (uint64_t)(r.end - r.p) || plen > 0xffffffffull) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
                goto fail;
            }
            payload = (uint32_t)(r.p - data);
            if (f->type == PBW_TYPE_MESSAGE) {
                if (!scan_message(NULL, (MessageType *)f->sub, data, payload,
                                  (size_t)plen, depth + 1)) {
                    goto fail;
                }
            } else if (f->wire != 2 && !scan_run(f, r.p, (size_t)plen)) {
                PyErr_SetString(PyExc_ValueError, "truncated packed field");
                goto fail;
            }
            r.p += (size_t)plen;
            /* An empty packed run carries no element: recording it would size
             * the lazy list by slices (PyList_New(1)) while put_run fills
             * nothing, leaving a NULL item behind. Strings/bytes (wire 2) keep
             * their zero-length value. */
            if (m != NULL && (plen != 0 || f->wire == 2) &&
                !msg_add(m, mt, f, payload, (uint32_t)plen, 1, oneof_last)) {
                goto fail;
            }
            continue;
        }
        {
            uint32_t value = (uint32_t)(r.p - data);

            if (!rd_skip(&r, fnum, wt, depth)) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
                goto fail;
            }
            if (m != NULL && !msg_add(m, mt, f, value, 0, 0, oneof_last)) {
                goto fail;
            }
        }
    }
    if (oneof_last != oneof_stack) {
        free(oneof_last);
    }
    return 1;

fail:
    if (oneof_last != oneof_stack) {
        free(oneof_last);
    }
    return 0;
}

/* GC support: MessageType points back at its Schema (which caches the
 * types), so dropped schemas would otherwise leak as uncollectable
 * cycles. Messages cannot start a cycle (their targets never point
 * back), but they are tracked anyway so a cycle reachable through
 * `mtype` stays collectable. */
static int
Message_traverse(Message *self, visitproc visit, void *arg)
{
    Py_VISIT(self->mtype);
    Py_VISIT(self->buffer);
    return 0;
}

static int
Message_clear(Message *self)
{
    if (self->slices != self->inline_slices) {
        free(self->slices);
        self->slices = self->inline_slices;
        self->cap = MSG_INLINE_SLICES;
    }
    Py_CLEAR(self->mtype);
    Py_CLEAR(self->buffer);
    return 0;
}

static void
Message_dealloc(Message *self)
{
    PyObject_GC_UnTrack(self);
    Message_clear(self);
    if (self->slices != self->inline_slices) {
        free(self->slices);
    }
    PyObject_GC_Del(self);
}

/* A Message that shares this one's buffer over `off`..`off + len`. */
static PyObject *
sub_message(Message *parent, field_t *f, uint32_t off, uint32_t len)
{
    Message *sub;

    if (parent->depth + 1 >= PY_MAX_DEPTH) {
        PyErr_SetString(PyExc_ValueError, "nesting limit exceeded");
        return NULL;
    }
    sub = PyObject_GC_New(Message, &MessagePyType);
    if (sub == NULL) {
        return NULL;
    }
    PyObject_GC_Track(sub);
    sub->mtype = f->sub;
    Py_INCREF(f->sub);
    sub->buffer = parent->buffer;
    Py_INCREF(parent->buffer);
    sub->data = parent->data;
    sub->off = off;
    sub->len = len;
    sub->depth = (uint16_t)(parent->depth + 1);
    sub->slices = sub->inline_slices;
    sub->nslices = 0;
    sub->cap = MSG_INLINE_SLICES;
    if (!scan_message(sub, (MessageType *)f->sub, sub->data, off, len,
                      sub->depth)) {
        Py_DECREF(sub);
        return NULL;
    }
    return (PyObject *)sub;
}

/* A singular message field that appears more than once merges: the payloads
 * concatenate, which is exactly what parsing them as one message means. */
static PyObject *
joined_message(Message *m, field_t *f, uint32_t fidx)
{
    uint64_t total = 0;
    PyObject *buffer;
    Message *out;
    uint8_t *dst;
    uint32_t i;

    for (i = 0; i < m->nslices; i++) {
        if (m->slices[i].fidx == fidx) {
            total += m->slices[i].len;
        }
    }
    if (total > 0xffffffffull) {
        PyErr_SetString(PyExc_ValueError, "merged message is too large");
        return NULL;
    }
    buffer = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)total);
    if (buffer == NULL) {
        return NULL;
    }
    dst = (uint8_t *)PyBytes_AS_STRING(buffer);
    for (i = 0; i < m->nslices; i++) {
        if (m->slices[i].fidx == fidx) {
            memcpy(dst, m->data + m->slices[i].off, m->slices[i].len);
            dst += m->slices[i].len;
        }
    }
    out = PyObject_GC_New(Message, &MessagePyType);
    if (out == NULL) {
        Py_DECREF(buffer);
        return NULL;
    }
    PyObject_GC_Track(out);
    out->mtype = f->sub;
    Py_INCREF(f->sub);
    out->buffer = buffer;
    out->data = (const uint8_t *)PyBytes_AS_STRING(buffer);
    out->off = 0;
    out->len = (uint32_t)total;
    out->depth = (uint16_t)(m->depth + 1);
    out->slices = out->inline_slices;
    out->nslices = 0;
    out->cap = MSG_INLINE_SLICES;
    if (!scan_message(out, (MessageType *)f->sub, out->data, 0, out->len,
                      out->depth)) {
        Py_DECREF(out);
        return NULL;
    }
    return (PyObject *)out;
}

/* Last live slice for a field, or -1. */
static Py_ssize_t
last_slice(Message *m, uint32_t fidx)
{
    Py_ssize_t i;

    for (i = (Py_ssize_t)m->nslices - 1; i >= 0; i--) {
        if (m->slices[i].fidx == fidx) {
            return i;
        }
    }
    return -1;
}

static int
slices_of(Message *m, uint32_t fidx)
{
    uint32_t i, n = 0;

    for (i = 0; i < m->nslices; i++) {
        n += m->slices[i].fidx == fidx;
    }
    return (int)n;
}

/* One scalar (or string/bytes) value from its slice. */
static PyObject *
slice_value(Message *m, field_t *f, const slice_t *s)
{
    uint64_t bits;
    rdr r;

    if (s->delimited) {
        return string_object(f->type, m->data + s->off, s->len);
    }
    r.p = m->data + s->off;
    r.end = m->data + m->off + m->len;
    if (f->wire == 0) {
        if (!rd_varint(&r, &bits)) {
            PyErr_SetString(PyExc_ValueError, "truncated field");
            return NULL;
        }
        if (f->type == PBW_TYPE_SINT32 || f->type == PBW_TYPE_SINT64) {
            bits = unzigzag(bits, f->type == PBW_TYPE_SINT32 ? 32 : 64);
        }
    } else if (!rd_fixed(&r, &bits, f->wire == 1 ? 8 : 4)) {
        PyErr_SetString(PyExc_ValueError, "truncated field");
        return NULL;
    }
    return scalar_object(f->type, bits);
}

/* Places a materialized value in the list: slot when the list was presized,
 * append after that. */
static int
put_value(PyObject *list, Py_ssize_t *pos, Py_ssize_t n, PyObject *value)
{
    if (*pos < n) {
        PyList_SET_ITEM(list, *pos, value); /* steals the reference */
        (*pos)++;
        return 1;
    }
    if (PyList_Append(list, value) < 0) {
        Py_DECREF(value);
        return 0;
    }
    Py_DECREF(value);
    return 1;
}

/* Elements of one packed run. */
static int
put_run(Message *m, field_t *f, const slice_t *s, PyObject *list,
        Py_ssize_t *pos, Py_ssize_t n)
{
    rdr r;

    r.p = m->data + s->off;
    r.end = r.p + s->len;
    while (r.p < r.end) {
        uint64_t bits;
        PyObject *value;

        if (f->wire == 0) {
            if (!rd_varint(&r, &bits)) {
                PyErr_SetString(PyExc_ValueError, "truncated packed field");
                return 0;
            }
            if (f->type == PBW_TYPE_SINT32 || f->type == PBW_TYPE_SINT64) {
                bits = unzigzag(bits, f->type == PBW_TYPE_SINT32 ? 32 : 64);
            }
        } else if (!rd_fixed(&r, &bits, f->wire == 1 ? 8 : 4)) {
            PyErr_SetString(PyExc_ValueError, "truncated packed field");
            return 0;
        }
        value = scalar_object(f->type, bits);
        if (value == NULL || !put_value(list, pos, n, value)) {
            return 0;
        }
    }
    return 1;
}

/* The message's value for one field: None when the field is absent, a list for
 * repeated fields, a Message for singular message fields. */
static PyObject *
field_value(Message *m, uint32_t fidx)
{
    MessageType *mt = (MessageType *)m->mtype;
    field_t *f = &mt->fields[fidx];
    PyObject *list;

    if (!f->repeated) {
        Py_ssize_t i = last_slice(m, fidx);

        if (i < 0) {
            Py_RETURN_NONE;
        }
        if (f->type == PBW_TYPE_MESSAGE || f->type == PBW_TYPE_GROUP) {
            if (slices_of(m, fidx) > 1) {
                return joined_message(m, f, fidx);
            }
            return sub_message(m, f, m->slices[i].off, m->slices[i].len);
        }
        return slice_value(m, f, &m->slices[i]);
    }

    {
        Py_ssize_t n = slices_of(m, fidx);

        if (n == 0) {
            Py_RETURN_NONE;
        }
        if (f->type == PBW_TYPE_MESSAGE || f->type == PBW_TYPE_GROUP ||
            f->type == PBW_TYPE_STRING || f->type == PBW_TYPE_BYTES) {
            Py_ssize_t k = 0;
            uint32_t i;

            list = PyList_New(n);
            if (list == NULL) {
                return NULL;
            }
            for (i = 0; i < m->nslices; i++) {
                PyObject *value;

                if (m->slices[i].fidx != fidx) {
                    continue;
                }
                if (f->type == PBW_TYPE_MESSAGE ||
                    f->type == PBW_TYPE_GROUP) {
                    value = sub_message(m, f, m->slices[i].off,
                                        m->slices[i].len);
                } else {
                    value = string_object(f->type, m->data + m->slices[i].off,
                                          m->slices[i].len);
                }
                if (value == NULL) {
                    Py_DECREF(list);
                    return NULL;
                }
                PyList_SET_ITEM(list, k++, value);
            }
            return list;
        }
    }

    {
        Py_ssize_t n = slices_of(m, fidx), pos = 0;
        uint32_t i;

        list = PyList_New(n);
        if (list == NULL) {
            return NULL;
        }
        for (i = 0; i < m->nslices; i++) {
            const slice_t *s = &m->slices[i];

            if (s->fidx != fidx) {
                continue;
            }
            if (s->delimited) {
                if (!put_run(m, f, s, list, &pos, n)) {
                    Py_DECREF(list);
                    return NULL;
                }
            } else {
                PyObject *value = slice_value(m, f, s);

                if (value == NULL || !put_value(list, &pos, n, value)) {
                    Py_DECREF(list);
                    return NULL;
                }
            }
        }
        return list;
    }
}

/* Field index for a name, or -1. */
static Py_ssize_t
field_index(MessageType *mt, PyObject *name)
{
    PyObject *idx = PyDict_GetItemWithError(mt->by_name, name);

    if (idx == NULL) {
        return -1;
    }
    return (Py_ssize_t)PyLong_AsSize_t(idx);
}

/* Field names present on the wire, in wire order, without duplicates. */
static PyObject *
message_keys(Message *self)
{
    MessageType *mt = (MessageType *)self->mtype;
    PyObject *list = PyList_New(0);
    char *seen;
    uint32_t i;

    if (list == NULL) {
        return NULL;
    }
    seen = calloc(mt->nfields == 0 ? 1 : mt->nfields, 1);
    if (seen == NULL) {
        Py_DECREF(list);
        return PyErr_NoMemory();
    }
    for (i = 0; i < self->nslices; i++) {
        uint32_t fidx = self->slices[i].fidx;

        if (fidx == SLICE_DEAD) {
            continue;
        }
        if (!seen[fidx]) {
            seen[fidx] = 1;
            if (PyList_Append(list, mt->fields[fidx].name) < 0) {
                free(seen);
                Py_DECREF(list);
                return NULL;
            }
        }
    }
    free(seen);
    return list;
}

static PyObject *
Message_getattro(Message *self, PyObject *name)
{
    Py_ssize_t fidx = field_index((MessageType *)self->mtype, name);

    if (fidx >= 0) {
        return field_value(self, (uint32_t)fidx);
    }
    if (PyErr_Occurred()) {
        return NULL;
    }
    return PyObject_GenericGetAttr((PyObject *)self, name);
}

static PyObject *
Message_subscript(Message *self, PyObject *name)
{
    Py_ssize_t fidx;

    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "field name must be a str");
        return NULL;
    }
    fidx = field_index((MessageType *)self->mtype, name);
    if (fidx < 0) {
        if (PyErr_Occurred()) {
            return NULL;
        }
        PyErr_SetObject(PyExc_KeyError, name);
        return NULL;
    }
    {
        PyObject *value = field_value(self, (uint32_t)fidx);

        if (value == Py_None) { /* absent field is a KeyError, like a dict */
            Py_DECREF(value);
            PyErr_SetObject(PyExc_KeyError, name);
            return NULL;
        }
        return value;
    }
}

static int
Message_contains(PyObject *self_obj, PyObject *name)
{
    Message *self = (Message *)self_obj;
    Py_ssize_t fidx;

    if (!PyUnicode_Check(name)) {
        return 0;
    }
    fidx = field_index((MessageType *)self->mtype, name);
    if (fidx < 0) {
        return PyErr_Occurred() ? -1 : 0;
    }
    return last_slice(self, (uint32_t)fidx) >= 0;
}

static Py_ssize_t
Message_length(Message *self)
{
    PyObject *keys = message_keys(self);
    Py_ssize_t n;

    if (keys == NULL) {
        return -1;
    }
    n = PyList_GET_SIZE(keys);
    Py_DECREF(keys);
    return n;
}

static PyObject *
Message_to_dict(Message *self, PyObject *unused)
{
    (void)unused;
    return dec_message((MessageType *)self->mtype, self->data + self->off,
                       self->len, 0);
}

static PyObject *
Message_keys_method(Message *self, PyObject *unused)
{
    (void)unused;
    return message_keys(self);
}

static PyObject *
Message_values(Message *self, PyObject *unused)
{
    MessageType *mt = (MessageType *)self->mtype;
    PyObject *list = PyList_New(0);
    char *seen;
    uint32_t i;

    (void)unused;
    if (list == NULL) {
        return NULL;
    }
    seen = calloc(mt->nfields == 0 ? 1 : mt->nfields, 1);
    if (seen == NULL) {
        Py_DECREF(list);
        return PyErr_NoMemory();
    }
    for (i = 0; i < self->nslices; i++) {
        uint32_t fidx = self->slices[i].fidx;
        PyObject *value;

        if (fidx == SLICE_DEAD || seen[fidx]) {
            continue;
        }
        seen[fidx] = 1;
        value = field_value(self, fidx);
        if (value == NULL || PyList_Append(list, value) < 0) {
            Py_XDECREF(value);
            free(seen);
            Py_DECREF(list);
            return NULL;
        }
        Py_DECREF(value);
    }
    free(seen);
    return list;
}

static PyObject *
Message_items(Message *self, PyObject *unused)
{
    MessageType *mt = (MessageType *)self->mtype;
    PyObject *list = PyList_New(0);
    char *seen;
    uint32_t i;

    (void)unused;
    if (list == NULL) {
        return NULL;
    }
    seen = calloc(mt->nfields == 0 ? 1 : mt->nfields, 1);
    if (seen == NULL) {
        Py_DECREF(list);
        return PyErr_NoMemory();
    }
    for (i = 0; i < self->nslices; i++) {
        uint32_t fidx = self->slices[i].fidx;
        PyObject *value, *pair;

        if (fidx == SLICE_DEAD || seen[fidx]) {
            continue;
        }
        seen[fidx] = 1;
        value = field_value(self, fidx);
        if (value == NULL) {
            free(seen);
            Py_DECREF(list);
            return NULL;
        }
        pair = PyTuple_Pack(2, mt->fields[fidx].name, value);
        Py_DECREF(value);
        if (pair == NULL || PyList_Append(list, pair) < 0) {
            Py_XDECREF(pair);
            free(seen);
            Py_DECREF(list);
            return NULL;
        }
        Py_DECREF(pair);
    }
    free(seen);
    return list;
}

static PyObject *
Message_get(Message *self, PyObject *args)
{
    PyObject *name;
    PyObject *fallback = Py_None;
    Py_ssize_t fidx;

    if (!PyArg_ParseTuple(args, "O|O:get", &name, &fallback)) {
        return NULL;
    }
    if (!PyUnicode_Check(name)) {
        Py_INCREF(fallback);
        return fallback;
    }
    fidx = field_index((MessageType *)self->mtype, name);
    if (fidx < 0) {
        if (PyErr_Occurred()) {
            return NULL;
        }
        Py_INCREF(fallback);
        return fallback;
    }
    {
        PyObject *value = field_value(self, (uint32_t)fidx);

        if (value == Py_None) {
            Py_DECREF(value);
            Py_INCREF(fallback);
            return fallback;
        }
        return value;
    }
}

static PyObject *
Message_get_type(Message *self, void *closure)
{
    PyObject *name = ((MessageType *)self->mtype)->name;

    (void)closure;
    Py_INCREF(name);
    return name;
}

static PyObject *
Message_repr(Message *self)
{
    Py_ssize_t n = Message_length(self);

    if (n < 0) {
        return NULL;
    }
    return PyUnicode_FromFormat("<pbwire.Message %s, %zd field(s) on the wire>",
                                ((MessageType *)self->mtype)->name_utf8, n);
}

static PyMethodDef Message_methods[] = {
    {"to_dict", (PyCFunction)Message_to_dict, METH_NOARGS,
     "to_dict() -> dict, the eager form of this message"},
    {"keys", (PyCFunction)Message_keys_method, METH_NOARGS,
     "keys() -> field names present on the wire, in wire order"},
    {"values", (PyCFunction)Message_values, METH_NOARGS,
     "values() -> values present on the wire, in wire order"},
    {"items", (PyCFunction)Message_items, METH_NOARGS,
     "items() -> (name, value) pairs present on the wire"},
    {"get", (PyCFunction)Message_get, METH_VARARGS,
     "get(name[, default]) -> value, or default when the field is absent"},
    {NULL, NULL, 0, NULL},
};

static PyMappingMethods Message_as_mapping = {
    (lenfunc)Message_length,       /* mp_length */
    (binaryfunc)Message_subscript, /* mp_subscript */
    NULL,
};

static PySequenceMethods Message_as_sequence = {
    .sq_contains = Message_contains,
};

static PyGetSetDef Message_getset[] = {
    {"type", (getter)Message_get_type, NULL, "message full name", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyTypeObject MessagePyType = {
    PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = "pbwire.Message",
    .tp_basicsize = sizeof(Message),
    .tp_dealloc = (destructor)Message_dealloc,
    .tp_traverse = (traverseproc)Message_traverse,
    .tp_clear = (inquiry)Message_clear,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "A parsed message: fields are read from the wire on demand.",
    .tp_methods = Message_methods,
    .tp_getattro = (getattrofunc)Message_getattro,
    .tp_as_mapping = &Message_as_mapping,
    .tp_as_sequence = &Message_as_sequence,
    .tp_getset = Message_getset,
    .tp_repr = (reprfunc)Message_repr,
};

/* ------------------------------------------------------------ type methods */

static int
MessageType_traverse(MessageType *self, visitproc visit, void *arg)
{
    size_t i;

    Py_VISIT(self->schema_obj);
    Py_VISIT(self->by_name);
    Py_VISIT(self->name);
    if (self->fields != NULL) {
        for (i = 0; i < self->nfields; i++) {
            Py_VISIT(self->fields[i].name);
            Py_VISIT(self->fields[i].sub);
        }
    }
    return 0;
}

static int
MessageType_clear(MessageType *self)
{
    size_t i;

    if (self->fields != NULL) {
        for (i = 0; i < self->nfields; i++) {
            Py_CLEAR(self->fields[i].name);
            Py_CLEAR(self->fields[i].sub);
        }
    }
    Py_CLEAR(self->by_name);
    Py_CLEAR(self->name);
    Py_CLEAR(self->schema_obj);
    return 0;
}

static void
MessageType_dealloc(MessageType *self)
{
    PyObject_GC_UnTrack(self);
    MessageType_clear(self);
    free(self->fields);
    PyObject_GC_Del(self);
}

static PyObject *
MessageType_encode(MessageType *self, PyObject *args)
{
    PyObject *obj;
    sbuf b;
    PyObject *out;

    if (!PyArg_ParseTuple(args, "O:encode", &obj)) {
        return NULL;
    }
    if (PyObject_TypeCheck(obj, &MessagePyType)) {
        Message *m = (Message *)obj;

        if (m->mtype != (PyObject *)self) {
            PyErr_Format(PyExc_TypeError, "%s cannot encode a %s",
                         self->name_utf8,
                         ((MessageType *)m->mtype)->name_utf8);
            return NULL;
        }
        return PyBytes_FromStringAndSize((const char *)m->data + m->off,
                                         (Py_ssize_t)m->len);
    }
    memset(&b, 0, sizeof(b));
    if (!enc_message(self, obj, &b, 0)) {
        free(b.data);
        return NULL;
    }
    out = PyBytes_FromStringAndSize((const char *)b.data, (Py_ssize_t)b.len);
    free(b.data);
    return out;
}

static PyObject *
MessageType_decode(MessageType *self, PyObject *args)
{
    PyObject *obj;
    PyObject *buffer;
    Message *m;

    /* bytes are immutable and kept alive by the Message, so no copy; any other
     * buffer gets copied into bytes first. */
    if (!PyArg_ParseTuple(args, "O:decode", &obj)) {
        return NULL;
    }
    if (PyBytes_Check(obj)) {
        buffer = obj;
        Py_INCREF(buffer);
    } else {
        Py_buffer view;

        if (PyObject_GetBuffer(obj, &view, PyBUF_SIMPLE) < 0) {
            return NULL;
        }
        buffer = (uint64_t)view.len > 0xffffffffull
                     ? NULL
                     : PyBytes_FromStringAndSize((const char *)view.buf,
                                                 (Py_ssize_t)view.len);
        PyBuffer_Release(&view);
        if (buffer == NULL) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PyExc_ValueError, "message is too large");
            }
            return NULL;
        }
    }

    if ((uint64_t)PyBytes_GET_SIZE(buffer) > 0xffffffffull) {
        Py_DECREF(buffer);
        PyErr_SetString(PyExc_ValueError, "message is too large");
        return NULL;
    }
    m = PyObject_GC_New(Message, &MessagePyType);
    if (m == NULL) {
        Py_DECREF(buffer);
        return NULL;
    }
    PyObject_GC_Track(m);
    m->mtype = (PyObject *)self;
    Py_INCREF(self);
    m->buffer = buffer;
    m->data = (const uint8_t *)PyBytes_AS_STRING(buffer);
    m->off = 0;
    m->len = (uint32_t)PyBytes_GET_SIZE(buffer);
    m->depth = 0;
    m->slices = m->inline_slices;
    m->nslices = 0;
    m->cap = MSG_INLINE_SLICES;
    if (!scan_message(m, self, m->data, 0, m->len, 0)) {
        Py_DECREF(m);
        return NULL;
    }
    return (PyObject *)m;
}

static PyObject *
MessageType_get_name(MessageType *self, void *closure)
{
    (void)closure;
    Py_INCREF(self->name);
    return self->name;
}

static PyMethodDef MessageType_methods[] = {
    {"encode", (PyCFunction)MessageType_encode, METH_VARARGS,
     "encode(dict_or_message) -> bytes"},
    {"decode", (PyCFunction)MessageType_decode, METH_VARARGS,
     "decode(bytes) -> Message, fields read on demand"},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef MessageType_getset[] = {
    {"name", (getter)MessageType_get_name, NULL, "message full name", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

/* -------------------------------------------------------------- building */

/* MessageType objects are built once per schema, subs included: the cache is
 * keyed by the descriptor's full name, which also breaks cycles in recursive
 * schemas (a message that contains itself). */
static PyObject *
make_message(PyObject *schema_obj, const pbw_descriptor *desc, PyObject *cache)
{
    PyObject *key, *existing, *self;
    MessageType *mt;
    size_t count, i;

    key = PyUnicode_FromString(pbw_descriptor_name(desc));
    if (key == NULL) {
        return NULL;
    }
    existing = PyDict_GetItemWithError(cache, key);
    if (existing != NULL) {
        Py_DECREF(key);
        Py_INCREF(existing);
        return existing;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(key);
        return NULL;
    }

    count = pbw_descriptor_field_count(desc);
    mt = PyObject_GC_New(MessageType, &MessageTypeType);
    if (mt == NULL) {
        Py_DECREF(key);
        return NULL;
    }
    self = (PyObject *)mt;
    mt->name = key; /* the cache key is the message name */
    mt->name_utf8 = pbw_descriptor_name(desc);
    mt->schema_obj = schema_obj;
    Py_INCREF(schema_obj);
    mt->desc = desc;
    mt->nfields = count;
    mt->n_oneofs = 0;
    mt->has_required = 0;
    mt->by_name = PyDict_New();
    mt->fields = count == 0 ? NULL : calloc(count, sizeof(field_t));
    if (mt->by_name == NULL || (count != 0 && mt->fields == NULL)) {
        Py_DECREF(self);
        return NULL;
    }
    if (PyDict_SetItem(cache, key, self) < 0) { /* recursive schemas */
        Py_DECREF(self);
        return NULL;
    }

    for (i = 0; i < count; i++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, i);
        field_t *f = &mt->fields[i];

        if (field == NULL) {
            Py_DECREF(self);
            return NULL;
        }
        f->type = (uint8_t)pbw_field_type(field);
        f->name = PyUnicode_FromString(pbw_field_name(field));
        f->cname = pbw_field_name(field);
        f->number = pbw_field_number(field);
        f->wire = (uint8_t)wire_type((pbw_type)f->type);
        f->repeated = pbw_field_label(field) == PBW_LABEL_REPEATED;
        f->packed = pbw_field_is_packed(field) != 0;
        f->required = pbw_field_label(field) == PBW_LABEL_REQUIRED;
        f->oneof = pbw_field_oneof_index(field);
        mt->has_required |= f->required;
        if (f->oneof > mt->n_oneofs) {
            mt->n_oneofs = f->oneof;
        }
        if (f->name == NULL) {
            Py_DECREF(self);
            return NULL;
        }
        {
            PyObject *idx = PyLong_FromSize_t(i);

            if (idx == NULL || PyDict_SetItem(mt->by_name, f->name, idx) < 0) {
                Py_XDECREF(idx);
                Py_DECREF(self);
                return NULL;
            }
            Py_DECREF(idx);
        }
    }
    /* Sub messages after every name is known, so a recursive type is usable
     * as soon as it comes back out of the cache. */
    for (i = 0; i < count; i++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, i);
        field_t *f = &mt->fields[i];

        if (f->type == PBW_TYPE_MESSAGE || f->type == PBW_TYPE_GROUP) {
            const pbw_descriptor *sub = pbw_field_message_type(field);

            if (sub == NULL) {
                Py_DECREF(self);
                return NULL;
            }
            f->sub = make_message(schema_obj, sub, cache);
            if (f->sub == NULL) {
                Py_DECREF(self);
                return NULL;
            }
        }
    }
    PyObject_GC_Track(self);
    return self;
}

/* ---------------------------------------------------------------- schema */

static int
Schema_traverse(Schema *self, visitproc visit, void *arg)
{
    Py_VISIT(self->cache);
    return 0;
}

static int
Schema_clear(Schema *self)
{
    Py_CLEAR(self->cache);
    return 0;
}

static void
Schema_dealloc(Schema *self)
{
    PyObject_GC_UnTrack(self);
    Schema_clear(self);
    if (self->schema != NULL) {
        pbw_schema_free(self->schema);
    }
    PyObject_GC_Del(self);
}

static PyObject *
Schema_message(Schema *self, PyObject *args)
{
    const char *name;
    const pbw_descriptor *desc;
    PyObject *key, *out;

    if (!PyArg_ParseTuple(args, "s:message", &name)) {
        return NULL;
    }
    key = PyUnicode_FromString(name);
    if (key == NULL) {
        return NULL;
    }
    out = PyDict_GetItemWithError(self->cache, key);
    if (out == NULL && !PyErr_Occurred()) {
        desc = pbw_schema_find(self->schema, name);
        if (desc == NULL) {
            Py_DECREF(key);
            PyErr_Format(PyExc_KeyError, "no message %s in the schema", name);
            return NULL;
        }
        out = make_message((PyObject *)self, desc, self->cache);
        if (out == NULL) {
            Py_DECREF(key);
            return NULL;
        }
        Py_DECREF(out); /* the cache owns it */
    }
    Py_XDECREF(key);
    if (out == NULL) {
        return NULL;
    }
    Py_INCREF(out);
    return out;
}

/* Reads a FileDescriptorSet file into memory. */
static int
read_file(const char *path, char **data, Py_ssize_t *len)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (f == NULL) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0) {
        fclose(f);
        PyErr_Format(PyExc_ValueError, "%s: empty descriptor set", path);
        return 0;
    }
    rewind(f);
    buf = malloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        PyErr_NoMemory();
        return 0;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        PyErr_Format(PyExc_OSError, "%s: short read", path);
        return 0;
    }
    fclose(f);
    *data = buf;
    *len = (Py_ssize_t)size;
    return 1;
}

static PyObject *
new_schema(const void *data, size_t len, const char *what)
{
    pbw_schema *schema = NULL;
    pbw_status status = pbw_descriptor_set_load(data, len, &schema);
    Schema *self;

    if (status != PBW_OK) {
        /* The loader hands back the partial schema on failure too, so its
         * reason is the only useful message; release the schema after
         * copying the text out (set_status() would only say "invalid data"). */
        const char *why = schema != NULL ? pbw_schema_error(schema) : NULL;

        if (why != NULL && why[0] != '\0') {
            PyErr_Format(PyExc_ValueError, "%s: %s", what, why);
        } else {
            set_status(status, what);
        }
        pbw_schema_free(schema);
        return NULL;
    }
    self = PyObject_GC_New(Schema, &SchemaType);
    if (self == NULL) {
        pbw_schema_free(schema);
        return NULL;
    }
    self->schema = schema;
    self->cache = PyDict_New();
    if (self->cache == NULL) {
        Py_DECREF(self);
        return NULL;
    }
    PyObject_GC_Track(self);
    return (PyObject *)self;
}

static PyObject *
Schema_from_file(PyObject *cls, PyObject *args)
{
    const char *path;
    char *data;
    Py_ssize_t len;
    PyObject *out;

    (void)cls;
    if (!PyArg_ParseTuple(args, "s:from_file", &path)) {
        return NULL;
    }
    if (!read_file(path, &data, &len)) {
        return NULL;
    }
    out = new_schema(data, (size_t)len, path);
    free(data);
    return out;
}

static PyObject *
Schema_load(PyObject *cls, PyObject *args)
{
    Py_buffer view;
    PyObject *out;

    (void)cls;
    if (!PyArg_ParseTuple(args, "y*:load", &view)) {
        return NULL;
    }
    out = new_schema(view.buf, (size_t)view.len, "descriptor set");
    PyBuffer_Release(&view);
    return out;
}

static PyObject *
Schema_enter(Schema *self, PyObject *unused)
{
    (void)unused;
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *
Schema_exit(Schema *self, PyObject *unused)
{
    (void)self;
    (void)unused;
    Py_RETURN_FALSE;
}

static PyMethodDef Schema_methods[] = {
    {"from_file", (PyCFunction)Schema_from_file, METH_VARARGS | METH_CLASS,
     "from_file(path) -> Schema, from a FileDescriptorSet"},
    {"load", (PyCFunction)Schema_load, METH_VARARGS | METH_CLASS,
     "load(bytes) -> Schema, from FileDescriptorSet bytes"},
    {"message", (PyCFunction)Schema_message, METH_VARARGS,
     "message(name) -> MessageType"},
    {"__enter__", (PyCFunction)Schema_enter, METH_NOARGS, "context manager"},
    {"__exit__", (PyCFunction)Schema_exit, METH_VARARGS, "context manager"},
    {NULL, NULL, 0, NULL},
};

static PyTypeObject SchemaType = {
    PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = "pbwire.Schema",
    .tp_basicsize = sizeof(Schema),
    .tp_dealloc = (destructor)Schema_dealloc,
    .tp_traverse = (traverseproc)Schema_traverse,
    .tp_clear = (inquiry)Schema_clear,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "A FileDescriptorSet loaded into a pbwire schema.",
    .tp_methods = Schema_methods,
};

static PyTypeObject MessageTypeType = {
    PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = "pbwire.MessageType",
    .tp_basicsize = sizeof(MessageType),
    .tp_dealloc = (destructor)MessageType_dealloc,
    .tp_traverse = (traverseproc)MessageType_traverse,
    .tp_clear = (inquiry)MessageType_clear,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "One message type of a pbwire schema.",
    .tp_methods = MessageType_methods,
    .tp_getset = MessageType_getset,
};

/* ---------------------------------------------------------------- module */

static PyObject *
module_abi_version(PyObject *self, PyObject *args)
{
    (void)self;
    (void)args;
    return PyLong_FromLong(PBW_ABI_VERSION);
}

static PyMethodDef module_methods[] = {
    {"abi_version", module_abi_version, METH_NOARGS, "PBW_ABI_VERSION"},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "pbwire",
    "pbwire: protobuf schemas at runtime, messages parsed from the wire.",
    -1,
    module_methods,
    NULL,
    NULL,
    NULL,
    NULL,
};

PyMODINIT_FUNC
PyInit_pbwire(void)
{
    PyObject *m;

    if (PyType_Ready(&SchemaType) < 0 ||
        PyType_Ready(&MessageTypeType) < 0 ||
        PyType_Ready(&MessagePyType) < 0) {
        return NULL;
    }
    m = PyModule_Create(&module_def);
    if (m == NULL) {
        return NULL;
    }
    if (pbw_abi_version() != PBW_ABI_VERSION) {
        PyErr_Format(PyExc_ImportError,
                     "libpbwire ABI %d does not match this module (%d)",
                     pbw_abi_version(), PBW_ABI_VERSION);
        Py_DECREF(m);
        return NULL;
    }
    Py_INCREF(&SchemaType);
    PyModule_AddObject(m, "Schema", (PyObject *)&SchemaType);
    Py_INCREF(&MessageTypeType);
    PyModule_AddObject(m, "MessageType", (PyObject *)&MessageTypeType);
    Py_INCREF(&MessagePyType);
    PyModule_AddObject(m, "Message", (PyObject *)&MessagePyType);
    return m;
}
