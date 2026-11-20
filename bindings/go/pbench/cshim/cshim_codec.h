/*
 * C half of the Go v2 probe.
 *
 * The Go side owns the struct layout (it is measured once with
 * unsafe.Offsetof at New time) and passes this table down.  C copies the
 * table into codec-owned memory, resolves field number -> row once, and
 * then every Marshal/Unmarshal call reads or writes Go memory through those
 * offsets.  No Go pointer is retained in C memory: the struct pointer is
 * passed as an integer for the duration of one call only.
 *
 * This file deliberately implements the wire primitives itself (varints,
 * zigzag, fixed-width fields, packed payloads) instead of calling the
 * pbwire core.  The probe is about the language-binding shape, and the
 * Erlang NIF does exactly the same: its own C file walks language values
 * and writes wire bytes.
 */
#ifndef PBW_GO_V2_CODEC_H
#define PBW_GO_V2_CODEC_H

#include <stddef.h>
#include <stdint.h>

typedef struct cshim_codec cshim_codec;

/* Kinds understood by the probe.  They name the in-memory value shape and
 * the wire transform, not just the protobuf type: a fixed32 field and a
 * float field share a wire width but differ in the proto3 zero test. */
enum {
    CSHIMK_I32 = 0,   /* int32 / enum: sign-extended varint */
    CSHIMK_I64,       /* int64: varint */
    CSHIMK_U32,       /* uint32: varint */
    CSHIMK_U64,       /* uint64: varint */
    CSHIMK_S32,       /* sint32: zigzag varint */
    CSHIMK_S64,       /* sint64: zigzag varint */
    CSHIMK_FIX32,     /* fixed32 / sfixed32: 4-byte little endian */
    CSHIMK_FIX64,     /* fixed64 / sfixed64: 8-byte little endian */
    CSHIMK_FLOAT,     /* float: 4-byte little endian, zero test is 0.0 */
    CSHIMK_DOUBLE,    /* double: 8-byte little endian, zero test is 0.0 */
    CSHIMK_BOOL,      /* bool: varint 0/1 */
    CSHIMK_STR,       /* Go string header (data at +0, length at +8) */
    CSHIMK_MSG,       /* Go pointer to a nested Go struct */
    CSHIMK_OPT_I32,   /* proto3 optional int32: Go pointer to int32 */
    CSHIMK_OPT_STR,   /* proto3 optional string: Go pointer to a raw header */
    CSHIMK_BYTES     /* Go []byte header (data at +0, length at +8) */
};

/*
 * One row per field.  All members are uint32_t so the Go mirror struct has
 * no padding surprises.
 *
 *   fnum     wire field number
 *   kind     CSHIMK_* above
 *   repeated 1 when the field is a Go slice header
 *   packed   1 when repeated scalars are packed on the wire
 *   wire     expected wire type of one element (0, 1, 2 or 5)
 *   offset   byte offset of the value in the Go struct; for a string or
 *            slice this is the header offset
 *   has      offset of the presence pointer for CSHIMK_OPT_*, else 0
 *   count    absolute byte offset of the Go len word for strings/slices,
 *            else 0 (Go strings and slices store ptr, len, cap)
 *   elem     in-memory element size: 4 for int32, 1 for bool, 16 for a
 *            string header, sizeof(void *) for []*Inner3
 *   explicit 1 when presence is a pointer at has (any scalar, string or
 *            bytes kind): NULL skips on encode, allocates on decode.
 *            Replaces the OPT_* kinds for new tables; those stay for the
 *            hand-written plan until it is generated.
 *   required 1 when the field always emits, even at its zero value
 *            (proto2 required). Never combined with repeated/explicit.
 *   group    1 when the row frames a proto2 group with start/end tags
 *            instead of a length prefix (Kind is KindMessage).
 *   sub      registry index of a message-typed field's sub-table, -1 when
 *            the row holds no submessage.
 */
typedef struct cshim_row {
    uint32_t fnum;
    uint32_t kind;
    uint32_t repeated;
    uint32_t packed;
    uint32_t wire;
    uint32_t offset;
    uint32_t has;
    uint32_t count;
    uint32_t elem;
    uint32_t explicit;
    uint32_t required;
    uint32_t group;
    int32_t sub;
} cshim_row;

/* One message table in the registry. The codec copies the rows at build
 * time; resolve and accumulators are derived per table. */
typedef struct cshim_table {
    const cshim_row *rows;
    size_t nrows;
    size_t msg_size;
} cshim_table;

/* Status codes.  On success exactly one cgo call is made per operation; the
 * error path returns a code and never needs a second call to fetch text.
 * CSHIM_ERR_SPACE is internal to the Go-side grow-and-retry loop for encode. */
enum {
    CSHIM_OK = 0,
    CSHIM_ERR_NOMEM = 1,
    CSHIM_ERR_WIRE = 2,
    CSHIM_ERR_DEPTH = 3,
    CSHIM_ERR_PLAN = 4,
    CSHIM_ERR_SPACE = 5
};

/* Encode writes into the caller's buffer.  If it does not fit, status is
 * CSHIM_ERR_SPACE and the caller retries with a larger buffer. */
typedef struct cshim_enc_result {
    int status;
    size_t len;
} cshim_enc_result;

/* Decode result.  str_bytes is the total size of every string payload seen,
 * which lets Go allocate one exact-size string arena per call. */
typedef struct cshim_dec_result {
    int status;
    size_t fail_off;
    size_t str_bytes;
} cshim_dec_result;

/* Build a codec over a table registry. tables[root] is the root message;
 * message rows address their sub-table through the row's sub index. */
int cshim_codec_new(cshim_codec **out, const cshim_table *tables,
                 size_t ntables, size_t root);

void cshim_codec_free(cshim_codec *h);

/* Marshal.  base is the Go struct address passed as an integer.  C writes
 * directly into out[0..cap); out is Go-owned and not retained after this
 * call returns.  On CSHIM_ERR_SPACE the caller must retry with a larger buffer. */
cshim_enc_result cshim_encode(cshim_codec *h, uintptr_t base, uint8_t *out,
                        size_t cap);

/* Unmarshal.  base must point at a zeroed Go struct.  The codec writes raw
 * values (scalars directly, strings as a data/length pair pointing into
 * wire, slices as a pointer/length/cap header, optionals as pointers, nested
 * messages as pointers to C-arena-resident Go structs).  Go copies every
 * string into one Go arena and every repeated field into one Go backing
 * array before returning, so no raw pointer survives a returned struct. */
cshim_dec_result cshim_decode(cshim_codec *h, uintptr_t base, const uint8_t *wire,
                        size_t wire_len);

#endif /* PBW_GO_V2_CODEC_H */
