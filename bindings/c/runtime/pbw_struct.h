/* Generic struct codec: the C binding runtime.
 *
 * Application structs stay plain data. Each message gets a layout table
 * (one row per field: field number -> struct offset) and the engine walks
 * the pbw schema to encode/decode it:
 *
 *   static const pbw_struct_field_t person_fields[] = {
 *       {1, PBW_SF_STRING, 0, offsetof(Person, name), 0, 0, NULL, 0},
 *       ...
 *   };
 *   pbw_struct_encode(schema, "Person", person_fields, 3, &person, &wire, &len);
 *
 * The schema is the truth for field numbers, wire types and proto2/proto3
 * semantics; the layout only answers "where in memory". Every schema field
 * must have a layout row (a missing row is PBW_ERR_INVALID, so a wrong
 * field number fails loudly instead of dropping data silently).
 *
 * Two ways in: the one-shot calls below resolve the schema and plan on every
 * call, and a compiled codec (pbw_struct_codec_*) that resolves it once and
 * reuses caller-owned buffers. Same bytes either way; the codec is the one
 * for hot loops.
 *
 * Memory: encode borrows the struct. Decode allocates (strings, arrays,
 * sub-messages) into a zero-initialized struct. Without an arena every value
 * is its own allocation, released by pbw_struct_free(). With an arena
 * (pbw_struct_codec_decode) the whole tree is bump-allocated from a reusable
 * arena: nothing to free, and a steady-state decode loop allocates nothing.
 * Pure C99, no dependencies beyond pbwire itself.
 */
#ifndef PBW_STRUCT_H
#define PBW_STRUCT_H

#include <stddef.h>
#include <stdint.h>

#include "pbwire.h"

typedef enum {
    PBW_SF_INT32,
    PBW_SF_INT64,
    PBW_SF_UINT32,
    PBW_SF_UINT64,
    PBW_SF_BOOL,
    PBW_SF_FLOAT,
    PBW_SF_DOUBLE,
    PBW_SF_STRING, /* const char *, NULL means absent */
    PBW_SF_BYTES,  /* pbw_bytes_t inline, data == NULL means absent */
    PBW_SF_ENUM,   /* int32_t, needs a has flag unless always present */
    PBW_SF_MESSAGE
} pbw_struct_kind_t;

/* Byte string: arbitrary bytes, may contain NUL. len is the truth;
 * data points at len bytes (plus a NUL guard the decoder appends for
 * convenience, not counted in len). Absent when data == NULL. */
typedef struct pbw_bytes {
    uint8_t *data;
    size_t len;
} pbw_bytes_t;

typedef struct pbw_struct_field {
    uint32_t fnum;
    pbw_struct_kind_t kind;
    int repeated;                          /* 0: singular, 1: repeated (+count holds) */
    size_t offset;                         /* data: value for scalars, pointer for string/message */
    size_t has;                            /* has-flag offset (singular scalars/enums); 0 = none */
    size_t count;                          /* count offset (repeated); 0 = singular */
    size_t elem;                           /* MESSAGE: sizeof(sub-struct), singular or repeated */
    int32_t dflt;                          /* ENUM default when absent (proto2 [default=] or 0) */
    const struct pbw_struct_field *fields; /* sub-layout (MESSAGE) */
    size_t nfields;
    /* Wire facts, filled in by the code generator. A table that leaves type
     * at 0 (tables written before these members existed) makes the codec fall
     * back to the schema, which is slower but identical. With them the codec
     * needs no schema lookup at all. */
    uint8_t type;     /* pbw_type of the field */
    uint8_t wire;     /* wire type of one element */
    uint8_t packed;   /* repeated and packed on the wire */
    uint8_t required; /* proto2 required */
    /* Oneof membership: 1 when the field is in a real oneof, 0 otherwise.
     * With oneof set, cs is the offset of the uint32_t case word holding
     * the selected field number (0 = none). The encoder emits the member
     * only when the case matches; the decoder sets the case and clears the
     * previous member. Tables written before these members existed leave
     * them 0, which means no oneof. */
    uint8_t oneof;
    size_t cs;
} pbw_struct_field_t;

/* A reusable encode buffer. Zero-initialize it before the first call;
 * reusing it keeps the capacity, so a steady-state loop does not allocate. */
typedef struct pbw_struct_buf {
    uint8_t *data;
    size_t len; /* bytes written */
    size_t cap;
} pbw_struct_buf;

/* Decode arena: the memory a codec decode allocates (strings, arrays,
 * sub-messages) is bump-allocated from blocks that are never moved, so the
 * pointers a decoded struct holds stay valid. Each decode resets the arena,
 * which invalidates the previous tree; the blocks themselves are reused, so
 * a steady-state decode loop allocates nothing. NULL means "no arena" and
 * makes decode malloc every value instead (release with pbw_struct_free). */
typedef struct pbw_struct_arena pbw_struct_arena;

/* Compiled layout of one message: the schema and field rows resolved once,
 * wire facts and tag bytes precomputed. Immutable after init, so one codec
 * can serve any number of encode/decode calls. */
typedef struct pbw_struct_codec pbw_struct_codec;

#ifdef __cplusplus
extern "C" {
#endif

/* Struct -> wire bytes. The struct is borrowed, never freed. */
pbw_status pbw_struct_encode(const pbw_schema *schema, const char *message,
                             const pbw_struct_field_t *fields, size_t nfields,
                             const void *base, uint8_t **wire, size_t *wire_len);

/* Wire bytes -> struct. base must point at zeroed memory; strings, arrays
 * and sub-messages are allocated into it. Release with pbw_struct_free(). */
pbw_status pbw_struct_decode(const pbw_schema *schema, const char *message,
                             const pbw_struct_field_t *fields, size_t nfields,
                             const uint8_t *wire, size_t wire_len, void *base);

/* Release what pbw_struct_decode() allocated (safe on zeroed structs). */
void pbw_struct_free(const pbw_struct_field_t *fields, size_t nfields,
                     void *base);

/* Load a FileDescriptorSet file (protoc --descriptor_set_out) into a
 * schema. File errors report PBW_ERR_INVALID (NOMEM on allocation
 * failure); a bad descriptor set propagates pbw_descriptor_set_load's
 * status with its *out contract (written on both paths, free on both). */
pbw_status pbw_struct_schema_load(const char *path, pbw_schema **out);

/* Compile a message layout. Every schema field must have a row, and a row
 * whose kind/wire type disagrees with the schema is PBW_ERR_INVALID. The
 * layout table and the schema are referenced, not copied, so both must
 * outlive the codec. *out is NULL on failure; release with
 * pbw_struct_codec_free(). */
pbw_status pbw_struct_codec_init(pbw_struct_codec **out,
                                 const pbw_schema *schema, const char *message,
                                 const pbw_struct_field_t *fields,
                                 size_t nfields);

void pbw_struct_codec_free(pbw_struct_codec *codec);

/* Struct -> wire bytes in buf, replacing its contents and keeping the
 * capacity for the next call: buf->data/buf->len are the bytes. */
pbw_status pbw_struct_codec_encode(const pbw_struct_codec *codec,
                                   const void *base, pbw_struct_buf *buf);

/* Wire bytes -> struct. base must point at zeroed memory. With arena == NULL
 * every value is its own allocation (release with pbw_struct_free); with an
 * arena (created on first use, *arena may be NULL) the whole tree is
 * bump-allocated from it: pass the same arena again to reuse, and release
 * with pbw_struct_arena_free. Arena-decoded pointers do not point at
 * individual allocations, so never pass that struct to pbw_struct_free(). */
pbw_status pbw_struct_codec_decode(const pbw_struct_codec *codec,
                                   const uint8_t *wire, size_t wire_len,
                                   pbw_struct_arena **arena, void *base);

/* Release an encode buffer (safe on a zeroed buffer). */
void pbw_struct_buf_free(pbw_struct_buf *buf);

/* Release a decode arena and everything allocated from it (NULL is a no-op).
 * Any struct decoded with it becomes a dangling view. */
void pbw_struct_arena_free(pbw_struct_arena *arena);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PBW_STRUCT_H */
