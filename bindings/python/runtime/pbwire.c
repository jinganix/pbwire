/* CPython extension: the Python binding.
 *
 * Messages travel as plain dicts. The C side walks the pbwire schema and
 * converts dict <-> wire bytes directly, the same way the Erlang binding does
 * it in c_src/ep_encoder.c / ep_decoder.c: no generated Python code, no
 * intermediate message tree, no per-value cffi call.
 *
 * Per message type the field facts are resolved once (name, number, type,
 * wire type, packed/required, sub-message type), so the hot loops never touch
 * the descriptor again.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <limits.h>
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
    uint8_t unsupported; /* group: skipped on decode, rejected on encode */
    uint32_t oneof;      /* 1-based oneof group, 0 = not in a oneof */
    PyObject *sub;       /* owned MessageType for MESSAGE fields */
} field_t;

typedef struct {
    PyObject_HEAD PyObject *name; /* owned */
    const char *name_utf8;
    PyObject *schema_obj; /* owned: keeps the schema (and desc) alive */
    const pbw_descriptor *desc;
    size_t nfields;
    field_t *fields;
    PyObject *by_name; /* owned dict: field name -> field index */
    int has_required;
} MessageType;

typedef struct {
    PyObject_HEAD pbw_schema *schema;
} Schema;

static PyTypeObject SchemaType;
static PyTypeObject MessageTypeType;

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

static int
enc_field(field_t *f, PyObject *v, sbuf *b, unsigned depth)
{
    if (f->unsupported) {
        PyErr_Format(PyExc_TypeError, "field %s: groups are unsupported",
                     f->cname);
        return 0;
    }
    if (f->type == PBW_TYPE_MESSAGE) {
        if (f->repeated) {
            PyObject *seq = SEQ_FAST(v, "repeated field needs a sequence");
            Py_ssize_t i, n;

            if (seq == NULL) {
                return 0;
            }
            n = SEQ_SIZE(seq);
            for (i = 0; i < n; i++) {
                size_t mark;

                if (!sb_reserve(b, 5 + 1)) {
                    Py_DECREF(seq);
                    return 0;
                }
                sb_tag_raw(b, f->number, 2);
                mark = b->len;
                b->data[b->len++] = 0;
                if (!enc_message((MessageType *)f->sub,
                                 SEQ_ITEM(seq, i), b, depth + 1)) {
                    Py_DECREF(seq);
                    return 0;
                }
                sb_len_end(b, mark);
            }
            Py_DECREF(seq);
            return 1;
        }
        if (!sb_reserve(b, 5 + 1)) {
            return 0;
        }
        sb_tag_raw(b, f->number, 2);
        {
            size_t mark = b->len;

            b->data[b->len++] = 0;
            if (!enc_message((MessageType *)f->sub, v, b, depth + 1)) {
                return 0;
            }
            sb_len_end(b, mark);
        }
        return 1;
    }

    if (f->type == PBW_TYPE_STRING || f->type == PBW_TYPE_BYTES) {
        const char *data;
        Py_ssize_t len;

        if (f->repeated) {
            PyObject *seq = SEQ_FAST(v, "repeated field needs a sequence");
            Py_ssize_t i, n;

            if (seq == NULL) {
                return 0;
            }
            n = SEQ_SIZE(seq);
            for (i = 0; i < n; i++) {
                if (!as_bytes(SEQ_ITEM(seq, i), &data, &len,
                              f->cname)) {
                    Py_DECREF(seq);
                    return 0;
                }
                if (!sb_reserve(b, 5 + 10 + (size_t)len)) {
                    Py_DECREF(seq);
                    return 0;
                }
                sb_tag_raw(b, f->number, 2);
                sb_varint_raw(b, (uint64_t)len);
                memcpy(b->data + b->len, data, (size_t)len);
                b->len += (size_t)len;
            }
            Py_DECREF(seq);
            return 1;
        }
        if (!as_bytes(v, &data, &len, f->cname)) {
            return 0;
        }
        if (!sb_reserve(b, 5 + 10 + (size_t)len)) {
            return 0;
        }
        sb_tag_raw(b, f->number, 2);
        sb_varint_raw(b, (uint64_t)len);
        memcpy(b->data + b->len, data, (size_t)len);
        b->len += (size_t)len;
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
        if (f->packed && packable((pbw_type)f->type)) {
            size_t mark;

            if (!sb_reserve(b, 5 + 1)) {
                Py_DECREF(seq);
                return 0;
            }
            sb_tag_raw(b, f->number, 2);
            mark = b->len;
            b->data[b->len++] = 0;
            for (i = 0; i < n; i++) {
                uint64_t bits;

                if (!as_bits(SEQ_ITEM(seq, i), f->type, &bits,
                             f->cname) ||
                    !sb_reserve(b, 10)) {
                    Py_DECREF(seq);
                    return 0;
                }
                put_scalar(b, f->type, bits);
            }
            sb_len_end(b, mark);
            Py_DECREF(seq);
            return 1;
        }
        for (i = 0; i < n; i++) {
            uint64_t bits;

            if (!as_bits(SEQ_ITEM(seq, i), f->type, &bits,
                         f->cname) ||
                !sb_reserve(b, 15)) {
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
        if (!sb_reserve(b, 15)) {
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
        PyErr_Format(PyExc_TypeError, "%s must be a dict",
                     mt->name_utf8);
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
                             mt->name_utf8,
                             f->cname);
                return 0;
            }
            continue;
        }
        used++;
        if (v == Py_None) {
            if (f->required) {
                PyErr_Format(PyExc_ValueError,
                             "%s: required field %s is missing",
                             mt->name_utf8,
                             f->cname);
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
    if (used != (size_t)PyDict_GET_SIZE(obj)) {
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

static PyObject *
MessageType_encode(MessageType *self, PyObject *args)
{
    PyObject *obj;
    sbuf b;
    PyObject *out;

    if (!PyArg_ParseTuple(args, "O:encode", &obj)) {
        return NULL;
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
    if (f->type == PBW_TYPE_MESSAGE) {
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

static PyObject *
dec_message(MessageType *mt, const uint8_t *wire, size_t wire_len,
            unsigned depth)
{
    PyObject *dict;
    PyObject **lists = NULL;
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
            size_t k = (cursor + i) % mt->nfields;

            if (mt->fields[k].number == fnum) {
                f = &mt->fields[k];
                fidx = k;
                cursor = k;
                break;
            }
        }
        if (f == NULL || f->unsupported ||
            (wt != f->wire && !(wt == 2 && f->repeated &&
                                packable((pbw_type)f->type)))) {
            if (!rd_skip(&r, fnum, wt, depth)) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
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
                if (lists == NULL) {
                    lists = calloc(mt->nfields, sizeof(*lists));
                    if (lists == NULL) {
                        Py_DECREF(value);
                        PyErr_NoMemory();
                        goto fail;
                    }
                }
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
            if (lists == NULL) {
                lists = calloc(mt->nfields, sizeof(*lists));
                if (lists == NULL) {
                    PyErr_NoMemory();
                    goto fail;
                }
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
                if (lists == NULL) {
                    lists = calloc(mt->nfields, sizeof(*lists));
                    if (lists == NULL) {
                        Py_DECREF(value);
                        PyErr_NoMemory();
                        goto fail;
                    }
                }
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
                             mt->name_utf8,
                             mt->fields[i].cname);
                goto fail;
            }
        }
    }
    free(lists);
    return dict;

fail:
    free(lists);
    Py_DECREF(dict);
    return NULL;
}

static PyObject *
MessageType_decode(MessageType *self, PyObject *args)
{
    Py_buffer view;

    if (!PyArg_ParseTuple(args, "y*:decode", &view)) {
        return NULL;
    }
    {
        PyObject *out = dec_message(self, (const uint8_t *)view.buf,
                                    (size_t)view.len, 0);

        PyBuffer_Release(&view);
        return out;
    }
}

/* -------------------------------------------------------------- building */

/* MessageType objects are built once per descriptor, subs included. The cache
 * breaks cycles in recursive schemas (a message that contains itself). */
static PyObject *
make_message(PyObject *schema_obj, const pbw_descriptor *desc, PyObject *cache)
{
    PyObject *key, *existing, *self;
    MessageType *mt;
    size_t count, i;

    key = PyLong_FromVoidPtr((void *)desc);
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
    mt = PyObject_New(MessageType, &MessageTypeType);
    if (mt == NULL) {
        Py_DECREF(key);
        return NULL;
    }
    self = (PyObject *)mt;
    mt->name = PyUnicode_FromString(pbw_descriptor_name(desc));
    mt->name_utf8 = pbw_descriptor_name(desc);
    mt->schema_obj = schema_obj;
    Py_INCREF(schema_obj);
    mt->desc = desc;
    mt->nfields = count;
    mt->has_required = 0;
    mt->by_name = PyDict_New();
    mt->fields = count == 0 ? NULL : calloc(count, sizeof(field_t));
    if (mt->name == NULL || mt->by_name == NULL ||
        (count != 0 && mt->fields == NULL)) {
        Py_DECREF(key);
        Py_DECREF(self);
        return NULL;
    }
    if (PyDict_SetItem(cache, key, self) < 0) { /* recursive schemas */
        Py_DECREF(key);
        Py_DECREF(self);
        return NULL;
    }
    Py_DECREF(key);

    for (i = 0; i < count; i++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, i);
        field_t *f = &mt->fields[i];
        pbw_type type;

        if (field == NULL) {
            Py_DECREF(self);
            return NULL;
        }
        type = pbw_field_type(field);
        f->name = PyUnicode_FromString(pbw_field_name(field));
        f->cname = pbw_field_name(field);
        f->number = pbw_field_number(field);
        f->type = (uint8_t)type;
        f->wire = (uint8_t)wire_type(type);
        f->repeated = pbw_field_label(field) == PBW_LABEL_REPEATED;
        f->packed = pbw_field_is_packed(field) != 0;
        f->required = pbw_field_label(field) == PBW_LABEL_REQUIRED;
        f->unsupported = type == PBW_TYPE_GROUP;
        f->oneof = pbw_field_oneof_index(field);
        mt->has_required |= f->required;
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

        if (f->type == PBW_TYPE_MESSAGE) {
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
    return self;
}

/* ------------------------------------------------------------ lazy view */

/* A parsed message costs one tag scan and a slice per wire field; Python
 * objects appear only when a field is read. For 3000 packed ints that is one
 * slice, not 3000 objects. */
typedef struct {
    uint32_t fidx;
    uint32_t len_delimited;
    size_t off; /* into MessageView.data */
    size_t len; /* payload length when len_delimited */
} slice_t;

typedef struct {
    PyObject_HEAD PyObject *mtype; /* owned MessageType */
    PyObject *buffer;              /* owned bytes object that owns data */
    const uint8_t *data;
    size_t len;
    slice_t *slices;
    size_t nslices;
    PyObject **cache; /* lazily materialized per field, calloc'd on demand */
} MessageView;

static PyTypeObject MessageViewType;

static PyObject *parse_view(PyObject *mtype_obj, const uint8_t *data, size_t len,
                            PyObject *buffer, unsigned depth);
static PyObject *view_to_dict(MessageView *self);

static void
MessageView_dealloc(MessageView *self)
{
    if (self->cache != NULL && self->mtype != NULL) {
        size_t i, n = ((MessageType *)self->mtype)->nfields;

        for (i = 0; i < n; i++) {
            Py_XDECREF(self->cache[i]);
        }
        free(self->cache);
    }
    free(self->slices);
    Py_XDECREF(self->mtype);
    Py_XDECREF(self->buffer);
    PyObject_Del(self);
}

static PyObject *
view_of_slice(MessageView *v, field_t *f, const slice_t *s)
{
    if (!s->len_delimited) {
        rdr r;
        uint64_t bits;

        r.p = v->data + s->off;
        r.end = v->data + v->len;
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
    if (f->type == PBW_TYPE_MESSAGE) {
        return parse_view(f->sub, v->data + s->off, s->len, v->buffer, 0);
    }
    return string_object(f->type, v->data + s->off, s->len);
}

/* Appends the element(s) of one slice to `list`. */
static int
append_slice(MessageView *v, field_t *f, PyObject *list, const slice_t *s,
             int deep)
{
    if (s->len_delimited && f->wire == 0) {
        /* packed run */
        rdr r;

        r.p = v->data + s->off;
        r.end = r.p + s->len;
        while (r.p < r.end) {
            uint64_t bits;
            PyObject *item;

            if (!rd_varint(&r, &bits)) {
                PyErr_SetString(PyExc_ValueError, "truncated packed field");
                return 0;
            }
            if (f->type == PBW_TYPE_SINT32 || f->type == PBW_TYPE_SINT64) {
                bits = unzigzag(bits, f->type == PBW_TYPE_SINT32 ? 32 : 64);
            }
            item = scalar_object(f->type, bits);
            if (item == NULL) {
                return 0;
            }
            if (PyList_Append(list, item) < 0) {
                Py_DECREF(item);
                return 0;
            }
            Py_DECREF(item);
        }
        return 1;
    }
    {
        PyObject *item = view_of_slice(v, f, s);

        if (item == NULL) {
            return 0;
        }
        if (deep && f->type == PBW_TYPE_MESSAGE) {
            PyObject *eager = view_to_dict((MessageView *)item);

            Py_DECREF(item);
            if (eager == NULL) {
                return 0;
            }
            item = eager;
        }
        if (PyList_Append(list, item) < 0) {
            Py_DECREF(item);
            return 0;
        }
        Py_DECREF(item);
    }
    return 1;
}

/* Materializes one field (cached). */
static PyObject *
view_field(MessageView *v, size_t fidx)
{
    MessageType *mt = (MessageType *)v->mtype;
    field_t *f = &mt->fields[fidx];
    PyObject *value = NULL;
    size_t i;

    if (v->cache == NULL) {
        v->cache = calloc(mt->nfields, sizeof(*v->cache));
        if (v->cache == NULL) {
            return PyErr_NoMemory();
        }
    }
    if (v->cache[fidx] != NULL) {
        Py_INCREF(v->cache[fidx]);
        return v->cache[fidx];
    }
    for (i = 0; i < v->nslices; i++) {
        if (v->slices[i].fidx != fidx) {
            continue;
        }
        if (!f->repeated) {
            value = view_of_slice(v, f, &v->slices[i]); /* last wins */
        } else if (value == NULL) {
            value = PyList_New(0);
        }
        if (value != NULL && f->repeated &&
            !append_slice(v, f, value, &v->slices[i], 0)) {
            Py_CLEAR(value);
        }
        if (value == NULL) {
            return NULL;
        }
    }
    if (value == NULL) {
        Py_RETURN_NONE; /* absent field: not cached, stays None */
    }
    v->cache[fidx] = value;
    Py_INCREF(value);
    return value;
}

static PyObject *
view_getattro(MessageView *self, PyObject *name)
{
    MessageType *mt = (MessageType *)self->mtype;
    PyObject *idx = PyDict_GetItemWithError(mt->by_name, name);

    if (idx != NULL) {
        return view_field(self, (size_t)PyLong_AsSize_t(idx));
    }
    if (PyErr_Occurred()) {
        return NULL;
    }
    return PyObject_GenericGetAttr((PyObject *)self, name);
}

static PyObject *
view_getitem(MessageView *self, PyObject *name)
{
    MessageType *mt = (MessageType *)self->mtype;
    PyObject *idx;

    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "field name must be a str");
        return NULL;
    }
    idx = PyDict_GetItemWithError(mt->by_name, name);
    if (idx == NULL) {
        if (PyErr_Occurred()) {
            return NULL;
        }
        PyErr_SetObject(PyExc_KeyError, name);
        return NULL;
    }
    {
        PyObject *value = view_field(self, (size_t)PyLong_AsSize_t(idx));

        if (value == Py_None) { /* absent field is a KeyError, like a dict */
            Py_DECREF(value);
            PyErr_SetObject(PyExc_KeyError, name);
            return NULL;
        }
        return value;
    }
}

/* Field names present in the wire, in wire order, without duplicates. */
static PyObject *
view_keys(MessageView *self)
{
    MessageType *mt = (MessageType *)self->mtype;
    PyObject *list = PyList_New(0);
    char *seen;
    size_t i;

    if (list == NULL) {
        return NULL;
    }
    seen = calloc(mt->nfields == 0 ? 1 : mt->nfields, 1);
    if (seen == NULL) {
        Py_DECREF(list);
        return PyErr_NoMemory();
    }
    for (i = 0; i < self->nslices; i++) {
        size_t fidx = self->slices[i].fidx;

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
view_to_dict(MessageView *self)
{
    MessageType *mt = (MessageType *)self->mtype;
    PyObject *dict = PyDict_New();
    PyObject **lists = NULL;
    size_t i;

    if (dict == NULL) {
        return NULL;
    }
    if (self->nslices > 0) {
        lists = calloc(mt->nfields == 0 ? 1 : mt->nfields, sizeof(*lists));
        if (lists == NULL) {
            Py_DECREF(dict);
            return PyErr_NoMemory();
        }
    }
    for (i = 0; i < self->nslices; i++) {
        const slice_t *s = &self->slices[i];
        field_t *f = &mt->fields[s->fidx];

        if (f->repeated) {
            PyObject *list = lists[s->fidx];

            if (list == NULL) {
                list = PyList_New(0);
                if (list == NULL ||
                    PyDict_SetItem(dict, f->name, list) < 0) {
                    Py_XDECREF(list);
                    goto fail;
                }
                Py_DECREF(list); /* the dict owns it */
                lists[s->fidx] = list;
            }
            if (!append_slice(self, f, list, s, 1)) {
                goto fail;
            }
        } else {
            PyObject *item = view_of_slice(self, f, s);

            if (item != NULL && f->type == PBW_TYPE_MESSAGE) {
                PyObject *deep = view_to_dict((MessageView *)item);

                Py_DECREF(item);
                item = deep;
            }
            if (item == NULL) {
                goto fail;
            }
            if (PyDict_SetItem(dict, f->name, item) < 0) {
                Py_DECREF(item);
                goto fail;
            }
            Py_DECREF(item);
        }
    }
    free(lists);
    if (mt->has_required) {
        size_t k;

        for (k = 0; k < mt->nfields; k++) {
            if (mt->fields[k].required &&
                !PyDict_Contains(dict, mt->fields[k].name)) {
                PyErr_Format(PyExc_ValueError,
                             "%s: required field %s is missing", mt->name_utf8,
                             mt->fields[k].cname);
                Py_DECREF(dict);
                return NULL;
            }
        }
    }
    return dict;

fail:
    free(lists);
    Py_DECREF(dict);
    return NULL;
}

static int
view_contains(PyObject *self_obj, PyObject *name)
{
    MessageView *self = (MessageView *)self_obj;
    MessageType *mt = (MessageType *)self->mtype;
    size_t i;

    if (!PyUnicode_Check(name)) {
        return 0;
    }
    if (PyDict_GetItemWithError(mt->by_name, name) == NULL) {
        return PyErr_Occurred() ? -1 : 0;
    }
    {
        PyObject *idx = PyDict_GetItem(mt->by_name, name);
        size_t fidx = (size_t)PyLong_AsSize_t(idx);

        if (PyErr_Occurred()) {
            return -1;
        }
        for (i = 0; i < self->nslices; i++) {
            if (self->slices[i].fidx == fidx) {
                return 1;
            }
        }
    }
    return 0;
}

static Py_ssize_t
view_length(MessageView *self)
{
    PyObject *keys = view_keys(self);
    Py_ssize_t n;

    if (keys == NULL) {
        return -1;
    }
    n = PyList_GET_SIZE(keys);
    Py_DECREF(keys);
    return n;
}

static PyObject *
view_repr(MessageView *self)
{
    return PyUnicode_FromFormat("<pbwire.Message %s, %zu field(s) on the wire>",
                                ((MessageType *)self->mtype)->name_utf8,
                                (size_t)self->nslices);
}

static PyObject *
view_to_dict_method(MessageView *self, PyObject *unused)
{
    (void)unused;
    return view_to_dict(self);
}

/* Turns a (possibly lazy) message value into plain dicts, consuming `value`. */
static PyObject *
deepen(field_t *f, PyObject *value)
{
    if (f->type != PBW_TYPE_MESSAGE) {
        return value;
    }
    if (!f->repeated) {
        PyObject *deep = view_to_dict((MessageView *)value);

        Py_DECREF(value);
        return deep;
    }
    {
        PyObject *list = PyList_New(0);
        Py_ssize_t i, n = PyList_GET_SIZE(value);

        if (list == NULL) {
            Py_DECREF(value);
            return NULL;
        }
        for (i = 0; i < n; i++) {
            PyObject *deep =
                view_to_dict((MessageView *)PyList_GET_ITEM(value, i));

            if (deep == NULL || PyList_Append(list, deep) < 0) {
                Py_XDECREF(deep);
                Py_DECREF(list);
                Py_DECREF(value);
                return NULL;
            }
            Py_DECREF(deep);
        }
        Py_DECREF(value);
        return list;
    }
}

static PyObject *
view_iter(MessageView *self)
{
    PyObject *pairs = PyList_New(0);
    size_t i;

    if (pairs == NULL) {
        return NULL;
    }
    for (i = 0; i < self->nslices; i++) {
        size_t fidx = self->slices[i].fidx;
        MessageType *mt = (MessageType *)self->mtype;
        PyObject *value, *pair;

        if (i > 0 && self->slices[i - 1].fidx == fidx) {
            continue; /* already yielded this field */
        }
        value = view_field(self, fidx);
        if (value == NULL) {
            Py_DECREF(pairs);
            return NULL;
        }
        value = deepen(&mt->fields[fidx], value);
        if (value == NULL) {
            Py_DECREF(pairs);
            return NULL;
        }
        pair = PyTuple_Pack(2, mt->fields[fidx].name, value);
        Py_DECREF(value);
        if (pair == NULL || PyList_Append(pairs, pair) < 0) {
            Py_XDECREF(pair);
            Py_DECREF(pairs);
            return NULL;
        }
        Py_DECREF(pair);
    }
    {
        PyObject *it = PyObject_GetIter(pairs);

        Py_DECREF(pairs);
        return it;
    }
}

static PyObject *
parse_view(PyObject *mtype_obj, const uint8_t *data, size_t len,
           PyObject *buffer, unsigned depth)
{
    MessageType *mt = (MessageType *)mtype_obj;
    MessageView *v;
    rdr r;
    size_t cursor = 0, cap = 0;

    if (depth >= PY_MAX_DEPTH) {
        PyErr_SetString(PyExc_ValueError, "nesting limit exceeded");
        return NULL;
    }
    v = PyObject_New(MessageView, &MessageViewType);
    if (v == NULL) {
        return NULL;
    }
    v->mtype = mtype_obj;
    Py_INCREF(mtype_obj);
    v->buffer = buffer;
    Py_INCREF(buffer);
    v->data = data;
    v->len = len;
    v->slices = NULL;
    v->nslices = 0;
    v->cache = NULL;

    r.p = data;
    r.end = data + len;
    while (r.p < r.end) {
        uint32_t fnum;
        unsigned wt;
        field_t *f = NULL;
        size_t i;
        slice_t slice;

        if (!rd_tag(&r, &fnum, &wt)) {
            PyErr_SetString(PyExc_ValueError, "truncated or malformed tag");
            goto fail;
        }
        for (i = 0; i < mt->nfields; i++) {
            size_t k = (cursor + i) % mt->nfields;

            if (mt->fields[k].number == fnum) {
                f = &mt->fields[k];
                slice.fidx = (uint32_t)k;
                cursor = k;
                break;
            }
        }
        if (f == NULL || f->unsupported ||
            (wt != f->wire && !(wt == 2 && f->repeated &&
                                packable((pbw_type)f->type)))) {
            if (!rd_skip(&r, fnum, wt, depth)) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
                goto fail;
            }
            continue;
        }
        if (wt == 2 && (f->wire == 2 || f->repeated)) {
            uint64_t plen;

            if (!rd_varint(&r, &plen) ||
                plen > (uint64_t)(r.end - r.p)) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
                goto fail;
            }
            slice.len_delimited = 1;
            slice.off = (size_t)(r.p - data);
            slice.len = (size_t)plen;
            r.p += (size_t)plen;
        } else {
            slice.len_delimited = 0;
            slice.off = (size_t)(r.p - data);
            slice.len = 0;
            if (!rd_skip(&r, fnum, wt, depth)) {
                PyErr_SetString(PyExc_ValueError, "truncated field");
                goto fail;
            }
        }
        if (v->nslices == cap) {
            size_t grown_cap = cap == 0 ? 16 : cap * 2;
            slice_t *grown = realloc(v->slices, grown_cap * sizeof(*grown));

            if (grown == NULL) {
                PyErr_NoMemory();
                goto fail;
            }
            v->slices = grown;
            cap = grown_cap;
        }
        v->slices[v->nslices++] = slice;
    }
    return (PyObject *)v;

fail:
    Py_DECREF(v);
    return NULL;
}

static PyObject *
MessageType_parse(MessageType *self, PyObject *args)
{
    PyObject *bytes;

    if (!PyArg_ParseTuple(args, "S:parse", &bytes)) {
        return NULL; /* bytes only: the view keeps it alive, no copy */
    }
    return parse_view((PyObject *)self,
                      (const uint8_t *)PyBytes_AS_STRING(bytes),
                      (size_t)PyBytes_GET_SIZE(bytes), bytes, 0);
}

static PyMethodDef MessageView_methods[] = {
    {"to_dict", (PyCFunction)view_to_dict_method, METH_NOARGS,
     "to_dict() -> dict, the eager form of this message"},
    {NULL, NULL, 0, NULL},
};

static PyMappingMethods MessageView_as_mapping = {
    (lenfunc)view_length,
    (binaryfunc)view_getitem,
    NULL,
};

static PySequenceMethods MessageView_as_sequence = {
    .sq_contains = view_contains,
};

static PyTypeObject MessageViewType = {
    PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = "pbwire.Message",
    .tp_basicsize = sizeof(MessageView),
    .tp_dealloc = (destructor)MessageView_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "A lazy view over parsed protobuf bytes: fields materialize on "
              "access.",
    .tp_methods = MessageView_methods,
    .tp_getattro = (getattrofunc)view_getattro,
    .tp_as_mapping = &MessageView_as_mapping,
    .tp_iter = (getiterfunc)view_iter,
    .tp_as_sequence = &MessageView_as_sequence,
    .tp_repr = (reprfunc)view_repr,
};

/* ----------------------------------------------------------- type methods */

static void
MessageType_dealloc(MessageType *self)
{
    size_t i;

    if (self->fields != NULL) {
        for (i = 0; i < self->nfields; i++) {
            Py_XDECREF(self->fields[i].name);
            Py_XDECREF(self->fields[i].sub);
        }
    }
    free(self->fields);
    Py_XDECREF(self->by_name);
    Py_XDECREF(self->name);
    Py_XDECREF(self->schema_obj);
    PyObject_Del(self);
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
     "encode(dict) -> bytes"},
    {"decode", (PyCFunction)MessageType_decode, METH_VARARGS,
     "decode(bytes) -> dict"},
    {"parse", (PyCFunction)MessageType_parse, METH_VARARGS,
     "parse(bytes) -> Message, a lazy view"},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef MessageType_getset[] = {
    {"name", (getter)MessageType_get_name, NULL, "message full name", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static void
Schema_dealloc(Schema *self)
{
    if (self->schema != NULL) {
        pbw_schema_free(self->schema);
    }
    PyObject_Del(self);
}

static PyObject *
Schema_message(Schema *self, PyObject *args)
{
    const char *name;
    const pbw_descriptor *desc;
    PyObject *cache, *out;

    if (!PyArg_ParseTuple(args, "s:message", &name)) {
        return NULL;
    }
    desc = pbw_schema_find(self->schema, name);
    if (desc == NULL) {
        PyErr_Format(PyExc_KeyError, "no message %s in the schema", name);
        return NULL;
    }
    cache = PyDict_New();
    if (cache == NULL) {
        return NULL;
    }
    out = make_message((PyObject *)self, desc, cache);
    Py_DECREF(cache);
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
        set_status(status, what);
        return NULL;
    }
    self = PyObject_New(Schema, &SchemaType);
    if (self == NULL) {
        pbw_schema_free(schema);
        return NULL;
    }
    self->schema = schema;
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
Schema_close(Schema *self, PyObject *unused)
{
    (void)unused;
    if (self->schema != NULL) {
        pbw_schema_free(self->schema);
        self->schema = NULL;
    }
    Py_RETURN_NONE;
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
    {"close", (PyCFunction)Schema_close, METH_NOARGS, "release the schema"},
    {"__enter__", (PyCFunction)Schema_enter, METH_NOARGS, "context manager"},
    {"__exit__", (PyCFunction)Schema_exit, METH_VARARGS, "context manager"},
    {NULL, NULL, 0, NULL},
};

static PyTypeObject SchemaType = {
    PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = "pbwire.Schema",
    .tp_basicsize = sizeof(Schema),
    .tp_dealloc = (destructor)Schema_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "A FileDescriptorSet loaded into a pbwire schema.",
    .tp_methods = Schema_methods,
};

static PyTypeObject MessageTypeType = {
    PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = "pbwire.MessageType",
    .tp_basicsize = sizeof(MessageType),
    .tp_dealloc = (destructor)MessageType_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
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
    "pbwire: protobuf schemas at runtime, messages as dicts.",
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
        PyType_Ready(&MessageViewType) < 0) {
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
    Py_INCREF(&MessageViewType);
    PyModule_AddObject(m, "Message", (PyObject *)&MessageViewType);
    return m;
}
