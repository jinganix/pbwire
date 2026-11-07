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
    /* The sub-second part moves the result by at most 999 ms either way, so
     * bound the multiply tightly enough that adding it cannot overflow:
     * sec*1000 + nanos/1000000 stays inside int64 for every accepted sec. */
    if (sec > (INT64_MAX - 999) / 1000 || sec < (INT64_MIN + 999) / 1000) {
        return PBW_ERR_LIMIT;
    }
    *out = sec * 1000 + nanos / 1000000;
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
 * field 2 the sub-second remainder in nanos (same sign as ms). int64
 * milliseconds always fit: seconds stay within ms/1000 either way. */
static pbw_status
wkt_from_millis(const pbw_descriptor *desc, int64_t ms, pbw_message **out)
{
    pbw_message *msg;
    pbw_value *vsec;
    pbw_value *vnano;
    pbw_status status;

    if (out == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    if (desc == NULL) {
        return PBW_ERR_INVALID;
    }
    msg = pbw_message_new(desc);
    vsec = pbw_value_new_int(ms / 1000);
    vnano = pbw_value_new_int((int64_t)(ms % 1000) * 1000000);
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
    return wkt_from_millis(ts_desc, ms, out);
}

pbw_status
pbw_duration_from_millis(const pbw_descriptor *dur_desc, int64_t ms,
                         pbw_message **out)
{
    return wkt_from_millis(dur_desc, ms, out);
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
