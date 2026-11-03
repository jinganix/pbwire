#ifndef PBWIRE_INTERNAL_H
#define PBWIRE_INTERNAL_H

#include "pbwire.h"

#include <stdlib.h>
#include <string.h>

/* Limits mirrored from the protobuf reference implementations and from
 * enif_protobuf's codec: unbounded nesting is a stack-overflow vector. */
#if defined(__GNUC__) || defined(__clang__)
#define PBW_INLINE __attribute__((always_inline)) static inline
/* Branch and prefetch hints: no-ops on compilers without them, so the core
 * stays buildable with a plain C99 compiler. */
#define PBW_EXPECT(x, v) __builtin_expect((x), (v))
#define PBW_PREFETCH(p) __builtin_prefetch(p)
#else
#define PBW_INLINE static inline
#define PBW_EXPECT(x, v) (x)
#define PBW_PREFETCH(p) ((void)0)
#endif

#define PBW_MAX_DEPTH 100
#define PBW_MAX_FIELD_NUMBER 536870911u /* 2^29 - 1 */
#define PBW_FNUM_MAP_LIMIT 512

/* Wire types. */
#define PBW_WIRE_VARINT 0
#define PBW_WIRE_64BIT 1
#define PBW_WIRE_LEN 2
#define PBW_WIRE_START_GROUP 3
#define PBW_WIRE_END_GROUP 4
#define PBW_WIRE_32BIT 5

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    int oom;
} pbw_buf;

/* Bump allocator. Everything a caller decodes with a pbw_ctx lives in one of
 * these, so resetting a context is a pointer move and freeing it is a handful
 * of free() calls instead of one per field value. */
typedef struct pbw_arena_chunk {
    struct pbw_arena_chunk *next;
    size_t used;
    size_t cap;
} pbw_arena_chunk;

typedef struct {
    pbw_arena_chunk *head;
    /* Oversized single allocations (see PBW_ARENA_LARGE_THRESHOLD): one malloc
     * each, freed on reset/free instead of pinning the reusable head chunk. */
    pbw_arena_chunk *large;
    /* Max head-chunk bytes kept across reset; 0 drops everything (plain-like
     * churn, no retention). Set by pbw_ctx_set_retain_cap, defaults to
     * PBW_ARENA_LARGE_THRESHOLD. */
    size_t retain_cap;
    size_t chunk_size;
    int readonly; /* a context arena: messages decoded into it are read-only */
    void *block;  /* non-NULL: the arena and its first chunk share one malloc */
} pbw_arena;

/* Single allocations above this bypass the bump chunk (nginx-style large
 * handling) and are freed on reset, so one outlier never pins the context.
 * Matches the chunk-growth cap so steady reuse still converges. */
#define PBW_ARENA_LARGE_THRESHOLD ((size_t)1 << 20)

struct pbw_field {
    char *name;
    /* dotted scope + "." + name for extensions ("pkg.Ext"), NULL otherwise */
    char *full_name;
    char *type_name;
    /* proto2 default, as written in the .proto (unescaped); NULL when unset */
    char *default_value;
    pbw_schema *schema;
    uint32_t number;
    uint32_t packed;
    uint32_t oneof_index;
    uint8_t is_extension;
    pbw_type type;
    pbw_label label;
    pbw_descriptor *sub;
    /* Wire bytes precomputed by pbw_schema_link(): the tag the field is encoded
     * with, the tag of its packed form, and its expected wire type. Encoding
     * and decoding then never format a tag. */
    uint8_t tag[5];
    uint8_t len_tag[5];
    /* The first four tag bytes as one word, for a single-store tag write. */
    uint32_t tag_word;
    uint32_t len_tag_word;
    uint8_t tag_len;
    uint8_t len_tag_len;
    uint8_t wire_type;
};

struct pbw_enum_value {
    char *name;
    int32_t number;
};

struct pbw_descriptor {
    char *name;
    int is_enum;
    /* proto3/editions semantics: open enums, no required enforcement */
    int proto3;
    pbw_schema *schema;
    size_t field_count;
    pbw_field *fields; /* sorted by number after link */
    size_t enum_count;
    struct pbw_enum_value *enum_values;
    uint32_t oneof_count;
    pbw_field **fnum_map; /* field number -> field, NULL when sparse */
    uint32_t fnum_map_size;
    /* reserved names from the descriptor (DescriptorProto.reserved_name) */
    char **reserved;
    size_t reserved_count;
    /* map entry (MessageOptions.map_entry): JSON prints it as an object */
    int map_entry;
    /* set by pbw_schema_link(): some field is a proto2 closed enum, so the
     * decoder must detour unknown enum numbers into the unknown fields */
    int has_closed_enum;
    /* Fast tag dispatch: tag_map[byte] is the field whose one-byte tag equals
     * `byte`, NULL when no field (or no one-byte tag) matches. Multibyte tags
     * still use fnum_map. NULL when no field has a one-byte tag. */
    pbw_field **tag_map;
    /* Parallel to tag_map: the precomputed decode action for that tag byte,
     * 0 (PBW_ACT_GENERIC) when the generic field logic must run. */
    uint8_t *tag_action;
};

struct pbw_schema {
    pbw_descriptor **descs;
    size_t count;
    size_t cap;
    int linked;
    int proto3; /* dialect handed to descriptors created from now on */
    /* set by pbw_schema_link(): some proto2 message declares a required field,
     * so a decode has to walk the tree once to validate it */
    int has_required;
    char error[192];
};

/* pbw_value.flags: the struct or its payload was allocated from an arena or
 * lives in a message slot, so it must never be free()d on its own. */
#define PBW_VF_EMBEDDED 1u /* the struct is a slot inside its message */
#define PBW_VF_ARENA 2u    /* allocated from a pbw_ctx arena */

struct pbw_value {
    pbw_value_kind kind;
    uint32_t flags;
    union {
        uint64_t u64;
        struct {
            uint8_t *data;
            size_t len;
        } bytes;
        pbw_message *msg;
        struct {
            pbw_value **items;
            uint32_t len; /* a list can never hold 2^32 items in memory */
            uint32_t cap;
        } list;
    } as;
};

struct pbw_message {
    const pbw_descriptor *desc;
    pbw_arena *arena;     /* non-NULL: the tree is allocated from an arena */
    pbw_arena *own;       /* non-NULL: this message frees that arena */
    pbw_value *values;    /* desc->field_count inline slots, see `present` */
    uint32_t *present;    /* field_count bits: the slot holds a value */
    uint32_t *oneof_case; /* 1-based oneof index -> field number, 0 = unset */
    uint8_t *unknown;
    size_t unknown_len;
    size_t unknown_cap;
    /* Wire bytes this tree was loaded from (root only, 0 = unknown).
     * encode_alloc() uses it as the initial buffer guess: exact for an
     * untouched tree, and merely a guess otherwise (the writer grows on
     * demand), so it needs no invalidation on mutation. Set at decode. */
    size_t wire_len;
};

/* Link-time decode actions for one-byte tags (see tag_action). Only shapes
 * whose semantics fit the inline fast path get one; everything else runs the
 * generic field logic, which is also what multibyte tags use. */
enum {
    PBW_ACT_GENERIC = 0,
    PBW_ACT_I32,   /* varint, sign-extended from 32 bits (int32, open enum) */
    PBW_ACT_I64,   /* varint raw (int64, uint64) */
    PBW_ACT_U32,   /* varint, masked to 32 bits (uint32) */
    PBW_ACT_S32,   /* varint, zigzag-decoded to 32 bits (sint32) */
    PBW_ACT_S64,   /* varint, zigzag-decoded to 64 bits (sint64) */
    PBW_ACT_BOOL,  /* varint, normalized to 0/1 (bool) */
    PBW_ACT_F32,   /* 4 little-endian bytes (fixed32, sfixed32, float) */
    PBW_ACT_F64,   /* 8 little-endian bytes (fixed64, sfixed64, double) */
    PBW_ACT_STR,   /* singular string/bytes, first or later occurrence */
    PBW_ACT_MSG,   /* singular message, merge or replace */
    PBW_ACT_PACKED /* packed form of a repeated packable field */
};

/* A message keeps its values inline: one allocation holds the struct, the value
 * slots, the presence bitmap and the oneof cases. */
static inline int
pbw_msg_present(const pbw_message *msg, size_t index)
{
    return (int)((msg->present[index >> 5] >> (index & 31u)) & 1u);
}

static inline void
pbw_msg_set_present(pbw_message *msg, size_t index, int on)
{
    uint32_t bit = 1u << (index & 31u);

    if (on) {
        msg->present[index >> 5] |= bit;
    } else {
        msg->present[index >> 5] &= ~bit;
    }
}

/* Appends raw wire bytes to msg->unknown; PBW_OK or PBW_ERR_NOMEM. */
int pbw_message_add_unknown(pbw_message *msg, const uint8_t *data, size_t len);

/* wire.c */
static inline size_t
pbw_varint_size(uint64_t value)
{
    size_t size = 1;
    while (value >= 0x80) {
        value >>= 7;
        size++;
    }
    return size;
}
size_t pbw_varint_write(uint8_t *out, uint64_t value);
int pbw_varint_read(const uint8_t *p, const uint8_t *end, uint64_t *value,
                    size_t *used);
/* Reads a field tag. A tag is a varint32: at most 5 bytes, and the value is
 * truncated to 32 bits exactly like the reference implementations. */
int pbw_tag_read(const uint8_t *p, const uint8_t *end, uint32_t *tag,
                 size_t *used);
/* Reads a length prefix: a strict varint32, unlike pbw_varint_read(). */
int pbw_len_read(const uint8_t *p, const uint8_t *end, size_t *len,
                 size_t *used);
void pbw_buf_init(pbw_buf *buf);
void pbw_buf_free(pbw_buf *buf);
/* Grows buf so that `extra` more bytes fit; capacity survives across calls. */
int pbw_buf_reserve(pbw_buf *buf, size_t extra);
void pbw_write_le32(uint8_t *out, uint32_t value);
uint32_t pbw_read_le32(const uint8_t *in);
uint64_t pbw_read_le64(const uint8_t *in);

/* arena.c */
void *pbw_arena_alloc_slow(pbw_arena *arena, size_t size);

static inline void *
pbw_arena_alloc(pbw_arena *arena, size_t size)
{
    pbw_arena_chunk *chunk = arena->head;
    void *ptr;

    size = (size + 7u) & ~(size_t)7u;
    if (chunk == NULL || chunk->cap - chunk->used < size) {
        return pbw_arena_alloc_slow(arena, size);
    }
    ptr = (char *)(chunk + 1) + chunk->used;
    chunk->used += size;
    return ptr;
}
/* Allocates an arena whose struct, first chunk and chunk header are one malloc,
 * for a tree that is decoded and freed as a whole. */
pbw_arena *pbw_arena_new_packed(size_t first_chunk);
void pbw_arena_reset(pbw_arena *arena);
void pbw_arena_free(pbw_arena *arena);
size_t pbw_arena_used(const pbw_arena *arena);

/* decode.c / encode.c */
pbw_status pbw_decode_arena(pbw_arena *arena, const pbw_descriptor *desc,
                            const void *buf, size_t len, pbw_message **out);
/* Same with an explicit nesting cap (PBW_MAX_DEPTH when limits are default). */
pbw_status pbw_decode_arena_lim(pbw_arena *arena, const pbw_descriptor *desc,
                                const void *buf, size_t len, uint32_t dlim,
                                pbw_message **out);
/* Effective nesting cap: 0 or an over-large value means the default. */
PBW_INLINE uint32_t
pbw_effective_depth(const pbw_limits *limits)
{
    if (limits == NULL || limits->max_depth == 0 ||
        limits->max_depth > PBW_MAX_DEPTH) {
        return PBW_MAX_DEPTH;
    }
    return limits->max_depth;
}
pbw_status pbw_encode_buf(pbw_buf *buf, const pbw_message *msg);

/* schema.c */
/* Hot path of pbw_descriptor_find_field(): inlined on purpose, it runs once per
 * field while decoding. */
static inline const pbw_field *
pbw_find_field(const pbw_descriptor *desc, uint32_t number)
{
    size_t lo;
    size_t hi;

    if (desc->fnum_map != NULL) {
        if (number < desc->fnum_map_size) {
            return desc->fnum_map[number];
        }
        return NULL;
    }
    lo = 0;
    hi = desc->field_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (desc->fields[mid].number < number) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < desc->field_count && desc->fields[lo].number == number) {
        return &desc->fields[lo];
    }
    return NULL;
}

void pbw_descriptor_free(pbw_descriptor *desc);
/* 1 when `name` is a reserved field name of this message. */
int pbw_descriptor_is_reserved(const pbw_descriptor *desc, const char *name);
/* Finds an extension field by its full name ("pkg.Ext"); hand-built
 * extensions without a full name match by their short name. */
const pbw_field *pbw_schema_find_extension(const pbw_schema *schema,
                                           const char *name);

/* message.c */
pbw_value *pbw_value_new_a(pbw_arena *arena, pbw_value_kind kind);
/* Fills value with a NUL-terminated copy of data; 0 on allocation failure. */
int pbw_value_init_bytes(pbw_value *value, pbw_arena *arena, const void *data,
                         size_t len);
/* Copies value into the field slot and marks it present (decoder storage). */
void pbw_message_put(pbw_message *msg, size_t index, const pbw_value *value);
pbw_value *pbw_value_new_list_a(pbw_arena *arena);
pbw_value *pbw_value_new_uint_a(pbw_arena *arena, uint64_t bits);
pbw_value *pbw_value_new_bytes_a(pbw_arena *arena, const void *data,
                                 size_t len);
pbw_value *pbw_value_new_message_a(pbw_arena *arena, pbw_message *msg);
pbw_message *pbw_message_new_a(pbw_arena *arena, const pbw_descriptor *desc);
pbw_value *pbw_message_list_for(pbw_message *msg, const pbw_field *field);
pbw_value *pbw_value_new_list(void);
/* Appends item to list; the items array comes from `arena` when it is not
 * NULL. Takes ownership of item. */
int pbw_value_list_push(pbw_value *list, pbw_value *item, pbw_arena *arena);
/* Grows list capacity to `cap` without adding items, for inputs whose item
 * count is known upfront (packed fixed-width fields). Mirrors the growth
 * policy of pbw_value_list_push; 1 on success (or when already big enough). */
int pbw_value_list_reserve(pbw_value *list, uint32_t cap, pbw_arena *arena);
/* Frees what a value points at (bytes, nested message, list items) but not the
 * pbw_value struct itself. */
void pbw_value_destroy(pbw_value *value);
/* Stores a caller-owned value into a field and takes ownership of it. */
int pbw_message_store(pbw_message *msg, const pbw_field *field,
                      pbw_value *value);

/* encode.c / decode.c */
int pbw_is_packable(pbw_type type);
uint32_t pbw_expected_wire_type(pbw_type type);

/* A proto2 (closed) enum rejects unknown numbers: the reference parsers move
 * them into the unknown fields instead of the field. */
static inline int
pbw_enum_is_closed(const pbw_field *field)
{
    return field->type == PBW_TYPE_ENUM && field->sub != NULL &&
           !field->sub->proto3;
}

#endif /* PBWIRE_INTERNAL_H */
