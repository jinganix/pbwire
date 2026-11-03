/* Bulk bridge for the Python binding (bindings/python).
 *
 * The binding itself is cffi in ABI mode: it builds a message tree field by
 * field, which costs one Python-to-C call (plus one value allocation) per
 * scalar. That is fine for a hundred bytes and fatal for a hundred kilobytes:
 * a 75KB message carries 12000 scalar values. These helpers take flat arrays
 * instead, so the binding converts three Python lists once and fills (or
 * drains) the whole message in one call. Same engine, fewer crossings.
 *
 * Ownership follows pbwire: on success the message owns the values; on
 * failure each helper frees what it just created and reports the status it
 * stopped at. Nothing here is Python-specific besides the packing.
 */
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include "pbwire.h"

/* Scalars are all PBW_VALUE_UINT inside pbwire (float/double carry their bit
 * pattern), so one store path covers ints, uints, bools and enums. */
static pbw_status
store(pbw_message *msg, uint32_t number, pbw_value *value)
{
    pbw_status status;

    if (value == NULL) {
        return PBW_ERR_NOMEM;
    }
    status = pbw_message_set(msg, number, value);
    if (status != PBW_OK) {
        pbw_value_free(value);
    }
    return status;
}

/* Every setter takes one target message per value, so the binding fills a whole
 * object tree (parents and children) with one call per type instead of five
 * crossings per submessage. Targets must stay alive for the duration: they are
 * owned by the tree being built. */
int
pbw_py_set_uints(pbw_message *const *msgs, const uint32_t *nums,
                 const uint64_t *vals, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        pbw_status status =
            store(msgs[i], nums[i], pbw_value_new_uint(vals[i]));

        if (status != PBW_OK) {
            return status;
        }
    }
    return PBW_OK;
}

int
pbw_py_set_floats(pbw_message *const *msgs, const uint32_t *nums,
                  const float *vals, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        pbw_status status =
            store(msgs[i], nums[i], pbw_value_new_float(vals[i]));

        if (status != PBW_OK) {
            return status;
        }
    }
    return PBW_OK;
}

int
pbw_py_set_doubles(pbw_message *const *msgs, const uint32_t *nums,
                   const double *vals, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        pbw_status status =
            store(msgs[i], nums[i], pbw_value_new_double(vals[i]));

        if (status != PBW_OK) {
            return status;
        }
    }
    return PBW_OK;
}

/* Strings and bytes share the string constructor: pbwire treats them alike on
 * the wire and validates UTF-8 nowhere except the JSON/text printers.
 * Python joins every payload into one bytes object, so the bridge takes that
 * blob plus offsets instead of an array of pointers (cffi cannot build a
 * char*[] from bytes, and this way nothing is copied twice). */
int
pbw_py_set_bytes(pbw_message *const *msgs, const uint32_t *nums,
                 const char *blob, const size_t *offs, const size_t *lens,
                 size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        pbw_status status = store(
            msgs[i], nums[i], pbw_value_new_bytes(blob + offs[i], lens[i]));

        if (status != PBW_OK) {
            return status;
        }
    }
    return PBW_OK;
}

/* Singular values report pbw_value_count() == 0; treat them as one item so
 * callers do not need a separate path per field label. Each getter returns the
 * item count and writes min(count, cap) items when out is not NULL, so the
 * binding sizes its buffer with one call and fills it with the next. */
/* sign: 0 keeps the raw bits (uints, bools), 32 and 64 widen a signed
 * int32/int64 into the same 64-bit slot, so the binding can hand the buffer
 * straight to array('q') instead of post-processing every value in Python. */
static uint64_t
as_value(const pbw_value *value, int sign)
{
    if (sign == 32) {
        return (uint64_t)(int64_t)pbw_value_get_int32(value);
    }
    if (sign == 64) {
        return (uint64_t)pbw_value_get_int(value);
    }
    return pbw_value_get_uint(value);
}

size_t
pbw_py_get_uints(const pbw_message *msg, uint32_t number, uint64_t *out,
                 size_t cap, int sign)
{
    pbw_value *value = pbw_message_get(msg, number);
    size_t i, n;

    if (value == NULL) {
        return 0;
    }
    n = pbw_value_count(value);
    if (n == 0) {
        if (out == NULL || cap == 0) {
            return 1;
        }
        out[0] = as_value(value, sign);
        return 1;
    }
    if (out != NULL) {
        size_t m = n > cap ? cap : n;

        for (i = 0; i < m; i++) {
            out[i] = as_value(pbw_value_at(value, i), sign);
        }
    }
    return n;
}

/* Repeated/singular message fields: hands out the child pointers directly so
 * the binding does not walk pbw_value_at + pbw_value_get_message per child. */
size_t
pbw_py_get_messages(const pbw_message *msg, uint32_t number, pbw_message **out,
                    size_t cap)
{
    pbw_value *value = pbw_message_get(msg, number);
    size_t i, n;

    if (value == NULL) {
        return 0;
    }
    n = pbw_value_count(value);
    if (n == 0) {
        if (out == NULL || cap == 0) {
            return 1;
        }
        out[0] = pbw_value_get_message(value);
        return 1;
    }
    if (out != NULL) {
        size_t m = n > cap ? cap : n;

        for (i = 0; i < m; i++) {
            out[i] = pbw_value_get_message(pbw_value_at(value, i));
        }
    }
    return n;
}

/* Strings and bytes: returns the item count, fills as many (offset, length)
 * pairs as cap allows, and copies as much of the concatenated payload as
 * blob_cap allows into blob. Two calls (size, then fill) keep the crossing
 * count constant no matter how many strings a repeated field holds. */
size_t
pbw_py_get_bytes(const pbw_message *msg, uint32_t number, char *blob,
                 size_t blob_cap, size_t *offs, size_t *lens, size_t cap)
{
    pbw_value *value = pbw_message_get(msg, number);
    size_t i, n, count, seen = 0;

    if (value == NULL) {
        return 0;
    }
    count = pbw_value_count(value);
    n = count == 0 ? 1 : count;
    for (i = 0; i < n && i < cap; i++) {
        const pbw_value *item = count == 0 ? value : pbw_value_at(value, i);
        size_t len = 0;
        const void *data = pbw_value_get_bytes(item, &len);

        if (offs != NULL) {
            offs[i] = seen;
        }
        if (lens != NULL) {
            lens[i] = len;
        }
        if (blob != NULL && seen < blob_cap) {
            size_t take = blob_cap - seen < len ? blob_cap - seen : len;

            memcpy(blob + seen, data, take);
        }
        seen += len;
    }
    return n;
}

size_t
pbw_py_get_floats(const pbw_message *msg, uint32_t number, float *out,
                  size_t cap)
{
    pbw_value *value = pbw_message_get(msg, number);
    size_t i, n;

    if (value == NULL) {
        return 0;
    }
    n = pbw_value_count(value);
    if (n == 0) {
        if (out == NULL || cap == 0) {
            return 1;
        }
        out[0] = pbw_value_get_float(value);
        return 1;
    }
    if (out != NULL) {
        size_t m = n > cap ? cap : n;

        for (i = 0; i < m; i++) {
            out[i] = pbw_value_get_float(pbw_value_at(value, i));
        }
    }
    return n;
}

size_t
pbw_py_get_doubles(const pbw_message *msg, uint32_t number, double *out,
                   size_t cap)
{
    pbw_value *value = pbw_message_get(msg, number);
    size_t i, n;

    if (value == NULL) {
        return 0;
    }
    n = pbw_value_count(value);
    if (n == 0) {
        if (out == NULL || cap == 0) {
            return 1;
        }
        out[0] = pbw_value_get_double(value);
        return 1;
    }
    if (out != NULL) {
        size_t m = n > cap ? cap : n;

        for (i = 0; i < m; i++) {
            out[i] = pbw_value_get_double(pbw_value_at(value, i));
        }
    }
    return n;
}
