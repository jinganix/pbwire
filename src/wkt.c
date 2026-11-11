/* Well-known-type helpers over the dynamic API: google.protobuf.Any packing
 * plus Timestamp/Duration millisecond conversion. Field numbers only (Any 1/2,
 * seconds/nanos 1/2), so any descriptor with the same shape works; the core
 * never special-cases WKTs anywhere else. */

#include "internal.h"

#include <stdint.h>

pbw_status
pbw_any_pack(const pbw_descriptor *any_desc, const char *type_url,
             const void *payload, size_t len, pbw_message **out)
{
    pbw_message *msg;
    pbw_value *vurl;
    pbw_value *vpay;
    pbw_status status;

    if (out == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    if (any_desc == NULL || type_url == NULL || (payload == NULL && len > 0)) {
        return PBW_ERR_INVALID;
    }
    msg = pbw_message_new(any_desc);
    vurl = pbw_value_new_string(type_url);
    vpay = pbw_value_new_bytes(payload, len);
    if (msg == NULL || vurl == NULL || vpay == NULL) {
        pbw_message_free(msg);
        pbw_value_free(vurl);
        pbw_value_free(vpay);
        return PBW_ERR_NOMEM;
    }
    /* set() owns the value on success only; free what it rejects. */
    status = pbw_message_set(msg, 1, vurl);
    if (status != PBW_OK) {
        pbw_value_free(vurl);
        pbw_value_free(vpay);
        pbw_message_free(msg);
        return status;
    }
    status = pbw_message_set(msg, 2, vpay);
    if (status != PBW_OK) {
        pbw_value_free(vpay);
        pbw_message_free(msg);
        return status;
    }
    *out = msg;
    return PBW_OK;
}

const char *
pbw_any_type_url(const pbw_message *any_msg)
{
    pbw_value *v;

    if (any_msg == NULL) {
        return NULL;
    }
    v = pbw_message_get(any_msg, 1);
    return v == NULL ? NULL : pbw_value_get_string(v);
}

const void *
pbw_any_payload(const pbw_message *any_msg, size_t *len)
{
    pbw_value *v;

    if (len != NULL) {
        *len = 0;
    }
    if (any_msg == NULL) {
        return NULL;
    }
    v = pbw_message_get(any_msg, 2);
    return v == NULL ? NULL : pbw_value_get_bytes(v, len);
}

/* seconds (field 1) + nanos (field 2) to milliseconds, shared by Timestamp
 * and Duration: same shape, same range rule. */
static pbw_status
wkt_to_millis(const pbw_message *msg, int64_t *out)
{
    pbw_value *vsec;
    pbw_value *vnano;
    int64_t sec;
    int32_t nanos = 0;

    if (msg == NULL || out == NULL) {
        return PBW_ERR_INVALID;
    }
    vsec = pbw_message_get(msg, 1);
    if (vsec == NULL) {
        return PBW_ERR_INVALID;
    }
    sec = (int64_t)pbw_value_get_uint(vsec);
    vnano = pbw_message_get(msg, 2);
    if (vnano != NULL) {
        nanos = pbw_value_get_int32(vnano);
    }
    if (nanos > 999999999 || nanos < -999999999) {
        return PBW_ERR_INVALID;
    }
    /* sec*1000 + nanos/1000000 must stay inside int64, and the sub-second
     * part only moves the result towards zero or away from it depending on
     * its sign: bound each direction by the actual fraction, so a legal
     * boundary value (sec 9223372036854775, nanos 807000000 == INT64_MAX)
     * round-trips instead of reporting a limit. */
    {
        int64_t frac = nanos / 1000000;
        int64_t hi;
        int64_t lo;

        /* Both bounds are always needed: a positive fraction can push the
         * product *up* and a negative one *down*, but a far negative sec
         * overflows on the low side regardless of the fraction's sign. The
         * bound uses the actual remainder per direction, so a legal boundary
         * value (sec 9223372036854775, nanos 807000000 == INT64_MAX) still
         * converts. */
        hi = (INT64_MAX - (frac > 0 ? frac : 0)) / 1000;
        lo = (INT64_MIN - (frac < 0 ? frac : 0)) / 1000;
        if (sec > hi || sec < lo) {
            return PBW_ERR_LIMIT;
        }
        *out = sec * 1000 + frac;
    }
    return PBW_OK;
}

pbw_status
pbw_timestamp_to_millis(const pbw_message *ts, int64_t *out)
{
    return wkt_to_millis(ts, out);
}

pbw_status
pbw_duration_to_millis(const pbw_message *dur, int64_t *out)
{
    return wkt_to_millis(dur, out);
}

/* Pack direction for the two time shapes: field 1 holds whole seconds,
 * field 2 the sub-second remainder. Duration keeps the same sign in both
 * fields; Timestamp requires nanos in [0, 1e9), so a negative millisecond
 * remainder borrows a second. That borrow can step one second below the
 * representable range at the very bottom of int64, which is refused rather
 * than written back as a pair that cannot convert. */
static pbw_status
wkt_from_millis(const pbw_descriptor *desc, int64_t ms, int floor_nanos,
                pbw_message **out)
{
    pbw_message *msg;
    pbw_value *vsec;
    pbw_value *vnano;
    pbw_status status;
    int64_t secs = ms / 1000;
    int64_t nanos = (ms % 1000) * 1000000;

    if (floor_nanos && nanos < 0) {
        secs -= 1;
        nanos += 1000000000;
    }
    if (out == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    if (desc == NULL) {
        return PBW_ERR_INVALID;
    }
    /* The pair must convert back: secs * 1000 has to stay inside int64, and
     * borrowing a second for a negative remainder can push a value at the
     * very bottom of the int64 range out of it (Timestamp cannot represent
     * INT64_MIN ms, Duration can: its nanos keep the sign). */
    if (secs > INT64_MAX / 1000 || secs < INT64_MIN / 1000) {
        return PBW_ERR_LIMIT;
    }
    msg = pbw_message_new(desc);
    vsec = pbw_value_new_int(secs);
    vnano = pbw_value_new_int(nanos);
    if (msg == NULL || vsec == NULL || vnano == NULL) {
        pbw_message_free(msg);
        pbw_value_free(vsec);
        pbw_value_free(vnano);
        return PBW_ERR_NOMEM;
    }
    status = pbw_message_set(msg, 1, vsec);
    if (status != PBW_OK) {
        pbw_value_free(vsec);
        pbw_value_free(vnano);
        pbw_message_free(msg);
        return status;
    }
    status = pbw_message_set(msg, 2, vnano);
    if (status != PBW_OK) {
        pbw_value_free(vnano);
        pbw_message_free(msg);
        return status;
    }
    *out = msg;
    return PBW_OK;
}

pbw_status
pbw_timestamp_from_millis(const pbw_descriptor *ts_desc, int64_t ms,
                          pbw_message **out)
{
    return wkt_from_millis(ts_desc, ms, 1, out);
}

pbw_status
pbw_duration_from_millis(const pbw_descriptor *dur_desc, int64_t ms,
                         pbw_message **out)
{
    return wkt_from_millis(dur_desc, ms, 0, out);
}

size_t
pbw_fieldmask_path_count(const pbw_message *mask)
{
    pbw_value *v;

    if (mask == NULL) {
        return 0;
    }
    v = pbw_message_get(mask, 1);
    return v == NULL ? 0 : pbw_value_count(v);
}

const char *
pbw_fieldmask_path_at(const pbw_message *mask, size_t index)
{
    pbw_value *v;
    pbw_value *item;

    if (mask == NULL) {
        return NULL;
    }
    v = pbw_message_get(mask, 1);
    if (v == NULL) {
        return NULL;
    }
    item = pbw_value_at(v, index);
    return item == NULL ? NULL : pbw_value_get_string(item);
}
