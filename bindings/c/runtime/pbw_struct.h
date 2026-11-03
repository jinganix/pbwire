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
 * Memory: encode borrows the struct. Decode allocates (strings, arrays,
 * sub-messages) into a zero-initialized struct; release with
 * pbw_struct_free(). Pure C99, no dependencies beyond pbwire itself.
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
    PBW_SF_ENUM,   /* int32_t, needs a has flag unless always present */
    PBW_SF_MESSAGE
} pbw_struct_kind_t;

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
} pbw_struct_field_t;

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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PBW_STRUCT_H */
