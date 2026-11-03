/* Testee for the official Protobuf conformance suite.
 *
 * This is the one program the suite needs: it reads ConformanceRequest
 * messages from stdin, and writes ConformanceResponse messages to stdout, both
 * length-delimited by a 4-byte little-endian length.
 *
 * Everything here is pbwire: the two protocol messages below are built by hand
 * (they are three fields wide), and the message under test comes from the
 * descriptor set compiled at build time -- which is exactly the job
 * pbw_descriptor_set_load() exists for. No generated code, no libprotobuf.
 *
 * The binary wire format, the JSON mapping and the text format are
 * implemented (only JSPB reports "skipped"), so the tester runs the whole
 * suite with nothing skipped. Two binary tests report a failure and
 * are expected to: the reference C++ testee from the same protobuf checkout
 * fails them identically (a 5-byte tag varint above UINT32_MAX is truncated
 * instead of rejected).
 *
 * Usage: testee <descriptor-set> */

#include "pbwire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define read_fd _read
#define write_fd _write
#else
#include <unistd.h>
#define read_fd read
#define write_fd write
#endif

enum {
    WIRE_PROTOBUF = 1,
    WIRE_JSON = 2,
    WIRE_TEXT = 4
};

enum {
    CATEGORY_JSON_IGNORE_UNKNOWN = 3
};

static pbw_schema *requests;
static pbw_schema *responses;
static pbw_schema *under_test;

/* ------------------------------------------------------------ field access */

static pbw_value *
get(const pbw_message *msg, uint32_t number)
{
    return pbw_message_get(msg, number);
}

static const void *
as_bytes(const pbw_message *msg, uint32_t number, size_t *len)
{
    pbw_value *v = get(msg, number);

    return v == NULL ? NULL : pbw_value_get_bytes(v, len);
}

static const char *
as_string(const pbw_message *msg, uint32_t number)
{
    size_t len = 0;
    const void *data = as_bytes(msg, number, &len);

    return data == NULL ? NULL : (const char *)data;
}

static int
as_enum(const pbw_message *msg, uint32_t number)
{
    pbw_value *v = get(msg, number);

    return v == NULL ? 0 : (int)pbw_value_get_int32(v);
}

/* ------------------------------------------------------------------- I/O */

static int
read_exact(void *out, size_t len)
{
    uint8_t *p = (uint8_t *)out;

    while (len > 0) {
        long got = (long)read_fd(0, p, len);

        if (got <= 0) {
            return 0;
        }
        p += got;
        len -= (size_t)got;
    }
    return 1;
}

static int
write_exact(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len > 0) {
        long put = (long)write_fd(1, p, len);

        if (put <= 0) {
            return 0;
        }
        p += put;
        len -= (size_t)put;
    }
    return 1;
}

/* ------------------------------------------------------------- the protocol */

/* conformance.ConformanceRequest, built by hand: the suite sends exactly these
 * fields and nothing else. */
static pbw_schema *
build_requests(void)
{
    pbw_schema *s = pbw_schema_new();
    pbw_descriptor *m = pbw_schema_add_message(s, "conformance.ConformanceRequest");

    pbw_message_add_field(m, "protobuf_payload", 1, PBW_TYPE_BYTES, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "json_payload", 2, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    /* WireFormat and TestCategory are enums, but pbwire enums are named types
     * and the wire format of an enum is a varint: int32 keeps the protocol
     * schema-free. */
    pbw_message_add_field(m, "requested_output_format", 3, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "message_type", 4, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "test_category", 5, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "jspb_payload", 7, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "text_payload", 8, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "print_unknown_fields", 9, PBW_TYPE_BOOL,
                          PBW_LABEL_OPTIONAL);
    if (pbw_schema_link(s) != PBW_OK) {
        fprintf(stderr, "testee: request schema: %s\n", pbw_schema_error(s));
        pbw_schema_free(s);
        return NULL;
    }
    return s;
}

/* conformance.ConformanceResponse: exactly one result field is set. */
static pbw_schema *
build_responses(void)
{
    pbw_schema *s = pbw_schema_new();
    pbw_descriptor *m = pbw_schema_add_message(s, "conformance.ConformanceResponse");

    pbw_message_add_field(m, "parse_error", 1, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "runtime_error", 2, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "protobuf_payload", 3, PBW_TYPE_BYTES, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "json_payload", 4, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "skipped", 5, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "serialize_error", 6, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "jspb_payload", 7, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "text_payload", 8, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(m, "timeout_error", 9, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    if (pbw_schema_link(s) != PBW_OK) {
        fprintf(stderr, "testee: response schema: %s\n", pbw_schema_error(s));
        pbw_schema_free(s);
        return NULL;
    }
    return s;
}

/* ---------------------------------------------------------------- the testee */

/* Fills `out` with one result field and returns 1, or returns 0 on a pbwire
 * failure that would leave the response empty. */
static int
conform(const pbw_message *request, pbw_message *out)
{
    const int format = as_enum(request, 3);
    const int category = as_enum(request, 5);
    const char *type = as_string(request, 4);
    size_t payload_len = 0;
    const void *payload = as_bytes(request, 1, &payload_len);
    size_t text_len = 0;
    const char *text = as_string(request, 8);
    size_t json_len = 0;
    const char *json = as_string(request, 2);
    int print_unknown = as_enum(request, 9);

    if (text != NULL) {
        text_len = strlen(text);
    }
    if (json != NULL) {
        json_len = strlen(json);
    }
    /* JSPB has no pbwire layer: reported as skipped. */
    if (format != WIRE_PROTOBUF && format != WIRE_JSON &&
        format != WIRE_TEXT) {
        return pbw_message_set(out, 5, pbw_value_new_string("no JSPB layer")) ==
               PBW_OK;
    }
    if (type == NULL) {
        return pbw_message_set(out, 2, pbw_value_new_string("no message_type")) ==
               PBW_OK;
    }
    {
        const pbw_descriptor *desc = pbw_schema_find(under_test, type);
        pbw_message *decoded = NULL;
        pbw_status status;

        if (desc == NULL) {
            char why[256];

            snprintf(why, sizeof(why), "unknown message type %s", type);
            return pbw_message_set(out, 2, pbw_value_new_string(why)) == PBW_OK;
        }
        /* input by presence: json_payload, text_payload or protobuf_payload */
        if (json != NULL) {
            pbw_json_parse_options opts;

            opts.ignore_unknown_fields =
                category == CATEGORY_JSON_IGNORE_UNKNOWN;
            status = pbw_json_parse_opts(desc, json, json_len, &opts,
                                         &decoded);
            if (status != PBW_OK) {
                return pbw_message_set(out, 1,
                                       pbw_value_new_string(pbw_strerror(status))) ==
                       PBW_OK;
            }
        } else if (text != NULL) {
            status = pbw_text_parse(desc, text, text_len, &decoded);
            if (status != PBW_OK) {
                return pbw_message_set(out, 1,
                                       pbw_value_new_string(pbw_strerror(status))) ==
                       PBW_OK;
            }
        } else if (payload != NULL) {
            status = pbw_decode(desc, payload, payload_len, &decoded);
            if (status != PBW_OK) {
                return pbw_message_set(out, 1,
                                       pbw_value_new_string(pbw_strerror(status))) ==
                       PBW_OK;
            }
        } else {
            return pbw_message_set(out, 5,
                                   pbw_value_new_string("unsupported payload")) ==
                   PBW_OK;
        }
        if (format == WIRE_JSON) {
            /* binary or text in, JSON out */
            char *jout = NULL;
            size_t jlen = 0;

            status = pbw_json_print(decoded, &jout, &jlen);
            pbw_message_free(decoded);
            if (status != PBW_OK) {
                return pbw_message_set(
                           out, 6, pbw_value_new_string(pbw_strerror(status))) ==
                       PBW_OK;
            }
            {
                int ok = pbw_message_set(out, 4, pbw_value_new_bytes(jout, jlen)) ==
                         PBW_OK;

                pbw_free(jout);
                return ok;
            }
        } else if (format == WIRE_TEXT) {
            /* protobuf in, text out (also answers text->text round trips) */
            char *tout = NULL;
            size_t tlen = 0;

            status = pbw_text_print_opts(decoded, print_unknown, &tout, &tlen);
            pbw_message_free(decoded);
            if (status != PBW_OK) {
                return pbw_message_set(
                           out, 6, pbw_value_new_string(pbw_strerror(status))) ==
                       PBW_OK;
            }
            {
                int ok = pbw_message_set(out, 8, pbw_value_new_bytes(tout, tlen)) ==
                         PBW_OK;

                pbw_free(tout);
                return ok;
            }
        } else {
            uint8_t *bytes = NULL;
            size_t len = 0;

            status = pbw_encode(decoded, &bytes, &len);
            pbw_message_free(decoded);
            if (status != PBW_OK) {
                return pbw_message_set(
                           out, 6, pbw_value_new_string(pbw_strerror(status))) ==
                       PBW_OK;
            }
            {
                int ok = pbw_message_set(out, 3, pbw_value_new_bytes(bytes, len)) ==
                         PBW_OK;

                pbw_free(bytes);
                return ok;
            }
        }
    }
}

static int
serve_one(void)
{
    uint8_t header[4];
    uint32_t len;
    uint8_t *buf;
    pbw_message *request = NULL;
    pbw_message *response;
    uint8_t *reply = NULL;
    size_t reply_len = 0;
    int ok;

    if (!read_exact(header, sizeof(header))) {
        return 0; /* clean end of input */
    }
    len = (uint32_t)header[0] | ((uint32_t)header[1] << 8) |
          ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
    buf = (uint8_t *)malloc(len ? len : 1);
    if (buf == NULL || !read_exact(buf, len)) {
        free(buf);
        return 0;
    }

    response = pbw_message_new(pbw_schema_find(responses, "conformance.ConformanceResponse"));
    if (pbw_decode(pbw_schema_find(requests, "conformance.ConformanceRequest"), buf,
                   len, &request) != PBW_OK ||
        response == NULL) {
        /* A request we cannot even parse is a protocol error, not a test
         * failure; report it and stop so the runner shows something useful. */
        fprintf(stderr, "testee: unparseable request (%u bytes)\n", len);
        free(buf);
        pbw_message_free(response);
        return 0;
    }
    free(buf);

    ok = conform(request, response) && pbw_encode(response, &reply, &reply_len) == PBW_OK;
    pbw_message_free(request);
    pbw_message_free(response);
    if (!ok) {
        fprintf(stderr, "testee: could not build a response\n");
        pbw_free(reply);
        return 0;
    }
    header[0] = (uint8_t)(reply_len & 0xff);
    header[1] = (uint8_t)((reply_len >> 8) & 0xff);
    header[2] = (uint8_t)((reply_len >> 16) & 0xff);
    header[3] = (uint8_t)((reply_len >> 24) & 0xff);
    ok = write_exact(header, sizeof(header)) && write_exact(reply, reply_len);
    pbw_free(reply);
    return ok;
}

int
main(int argc, char **argv)
{
    FILE *file;
    long size;
    uint8_t *bytes;
    int running;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <descriptor-set>\n", argv[0]);
        return 2;
    }
    file = fopen(argv[1], "rb");
    if (file == NULL) {
        fprintf(stderr, "testee: cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    bytes = (uint8_t *)malloc((size_t)size);
    if (bytes == NULL || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "testee: cannot read %s\n", argv[1]);
        fclose(file);
        free(bytes);
        return 2;
    }
    fclose(file);

    if (pbw_descriptor_set_load(bytes, (size_t)size, &under_test) != PBW_OK) {
        fprintf(stderr, "testee: descriptor set did not load: %s\n",
                pbw_schema_error(under_test));
        free(bytes);
        return 2;
    }
    free(bytes);
    requests = build_requests();
    responses = build_responses();
    if (requests == NULL || responses == NULL) {
        return 2;
    }

    while ((running = serve_one()) != 0) {
        /* serve_one() loops until stdin closes */
    }
    pbw_schema_free(requests);
    pbw_schema_free(responses);
    pbw_schema_free(under_test);
    return 0;
}
