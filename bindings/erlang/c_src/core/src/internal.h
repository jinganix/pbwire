#ifndef PBWIRE_INTERNAL_H
#define PBWIRE_INTERNAL_H

#include "pbwire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Limits mirrored from the protobuf reference implementations and from
 * pbwire's codec: unbounded nesting is a stack-overflow vector. */
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

#define PBW_MAX_FIELD_NUMBER 536870911u /* 2^29 - 1 */
#define PBW_FNUM_MAP_LIMIT 512

/* Wire types. */
#define PBW_WIRE_VARINT 0
#define PBW_WIRE_64BIT 1
#define PBW_WIRE_LEN 2
#define PBW_WIRE_START_GROUP 3
#define PBW_WIRE_END_GROUP 4
#define PBW_WIRE_32BIT 5

/* Byte distance between two pointers into the same buffer, without the
 * ptrdiff_t overflow that `(size_t)(end - p)` has for multi-gigabyte
 * buffers on 32-bit targets. Both pointers must be into (or one past)
 * the same object; otherwise the result is meaningless but still defined. */
PBW_INLINE size_t
pbw_remaining(const uint8_t *end, const uint8_t *p)
{
    uintptr_t ue = (uintptr_t)(const void *)end;
    uintptr_t up = (uintptr_t)(const void *)p;

    if (ue < up) {
        return 0;
    }
    return (size_t)(ue - up);
}

/* Non-NULL empty input: `NULL + 0` is undefined even when nothing is read,
 * so every entry point normalizes a NULL empty buffer to this first. */
extern const uint8_t pbw_empty_input[1];
PBW_INLINE const uint8_t *
pbw_normalize_input(const void *buf, size_t len)
{
    (void)len;
    if (buf != NULL) {
        return (const uint8_t *)buf;
    }
    return pbw_empty_input;
}

/* 1 when `pos <= cap`; guards every `cap - pos` site against wrap on
 * corrupted state. Corruption cannot happen through the public API, but
 * a wrapped subtraction would turn into a huge "available" length and
 * then into an out-of-bounds access. */
PBW_INLINE int
pbw_cap_holds(size_t cap, size_t pos)
{
    return cap >= pos;
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    int oom;
    /* Caller-owned memory (pbw_encode_to): growing is impossible, so a write
     * that does not fit fails the encode instead of reallocating. */
    int fixed;
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
    int has_heap; /* a heap value was stored into this arena tree: free must walk */
} pbw_arena;

/* Payload offsets keep every arena allocation 8-byte aligned. malloc returns
 * 8-aligned memory on the 32-bit targets this runs on, but sizeof of these
 * headers is 12 and 28 bytes on ILP32, so (chunk + 1) would land 4 bytes off
 * and a 64-bit value slot there would fault where 8-byte alignment is
 * required (arm/aarch32). Rounding up costs a few bytes per chunk and
 * nothing per allocation. */
#define PBW_CHUNK_HDR ((sizeof(pbw_arena_chunk) + 7u) & ~(size_t)7u)
#define PBW_ARENA_HDR ((sizeof(pbw_arena) + 7u) & ~(size_t)7u)

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
    /* Editions FeatureSet resolution (0 = inherit / not an editions file).
     * presence: 1 EXPLICIT, 2 IMPLICIT, 3 LEGACY_REQUIRED (required).
     * enum_open: 1 OPEN, 2 CLOSED (unknown enum numbers move to unknowns).
     * msg_enc: 1 LENGTH_PREFIXED, 2 DELIMITED (group tags on the wire).
     * Only the descriptor-set loader sets these; hand-built schemas stay 0
     * and keep the dialect-wide behavior. */
    uint8_t presence;
    uint8_t enum_open;
    uint8_t msg_enc;
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
    /* Link-time decode action (see PBW_ACT_*): the shape-specific fast path
     * decode_fields() runs for this field, or PBW_ACT_GENERIC when the
     * generic field logic must run (oneof members, groups, closed enums).
     * Computed for every field regardless of tag size, so one- and
     * multi-byte tags share a single action switch. */
    uint8_t dec_action;
};

struct pbw_enum_value {
    char *name;
    int32_t number;
};

/* One rpc method: resolved names only, no transport. Streaming flags are
 * plain ints (0/1) so bindings read them without an enum. */
struct pbw_method {
    char *name;
    char *input_type;  /* fully qualified, no leading dot */
    char *output_type; /* fully qualified, no leading dot */
    int client_streaming;
    int server_streaming;
};

/* One service: the ordered method list from ServiceDescriptorProto. */
struct pbw_service {
    char *name; /* fully qualified, no leading dot ("pkg.Service") */
    pbw_method *methods;
    size_t method_count;
    pbw_schema *schema;
};

struct pbw_descriptor {
    char *name;
    int is_enum;
    /* proto3/editions semantics: open enums, no required enforcement */
    int proto3;
    /* MessageSet wire format (MessageOptions.message_set_wire_format):
     * group items decode into their extension fields instead of staying
     * unknown. Only the descriptor-set loader sets this. */
    int is_messageset;
    /* Editions file (FileDescriptorProto.edition value, e.g. 1000 for
     * EDITION_2023); 0 for syntax proto2/proto3 files and hand-built
     * schemas. Field FeatureSet overrides only exist when this is set. */
    int edition;
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
    /* set by pbw_schema_link(): this message or a reachable submessage
     * declares a proto2 required field, so a decode has to walk the tree
     * once to validate it. Pure-proto3 subtrees stay 0 and are skipped. */
    int has_required;
    /* Fast tag dispatch: tag_map[byte] is the field whose one-byte tag (or
     * one-byte packed-form tag) equals `byte`, NULL when no field matches.
     * Multibyte tags still use fnum_map. NULL when no field has a one-byte
     * tag. The decode action lives on the field itself (see dec_action),
     * so this table is lookup-only. */
    pbw_field **tag_map;
    /* Fast name dispatch for table/dict bindings: open-addressed hash of
     * field name -> field, NULL when the message has no fields. Lookup is
     * one hash plus probes instead of a linear strcmp scan. */
    const pbw_field **name_map;
    size_t name_map_size; /* power of two, 0 when empty */
};

struct pbw_schema {
    pbw_descriptor **descs;
    size_t count;
    size_t cap;
    /* Open-addressed name index over descs (power of two), so
     * pbw_schema_find() is O(1): loading a FileDescriptorSet resolves one
     * name per message/enum field and per extension, and a linear scan made
     * that quadratic in the schema size. NULL when the index could not be
     * built (out of memory), which falls back to the scan. */
    pbw_descriptor **name_index;
    size_t name_index_size;
    size_t name_index_count;
    struct pbw_service **services;
    size_t service_count;
    size_t service_cap;
    /* Edition of the file currently being loaded (0 = syntax proto2/proto3);
     * descriptors copy it at creation, like the proto3 dialect below. */
    int edition;
    int linked;
    /* Set once a link has succeeded: field counts can no longer change, so a
     * message's value slots always match its descriptor. */
    int frozen;
    int proto3; /* dialect handed to descriptors created from now on */
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
    pbw_value *values;    /* nfields inline slots, see `present` */
    uint32_t *present;    /* field_count bits: the slot holds a value */
    uint32_t *oneof_case; /* 1-based oneof index -> field number, 0 = unset */
    size_t nfields;       /* desc->field_count at creation: free needs no desc */
    uint8_t *unknown;
    size_t unknown_len;
    size_t unknown_cap;
    /* Wire bytes this tree was loaded from (root only, 0 = unknown).
     * encode_alloc() uses it as the initial buffer guess: exact for an
     * untouched tree, and merely a guess otherwise (the writer grows on
     * demand), so it needs no invalidation on mutation. Set at decode. */
    size_t wire_len;
};

/* Link-time decode actions, one per field (see dec_action). Only shapes
 * whose semantics fit the inline fast path get one: oneof members, groups
 * and closed enums keep PBW_ACT_GENERIC and run the generic field logic.
 * The packed (length-delimited) form of a repeated packable field is
 * detected by its wire type at decode time, so it needs no action of its
 * own: every repeated action below describes the unpacked form. */
enum {
    PBW_ACT_GENERIC = 0,
    PBW_ACT_I32,        /* singular varint, sign-extended from 32 bits */
    PBW_ACT_I64,        /* singular varint raw (int64, uint64) */
    PBW_ACT_U32,        /* singular varint, masked to 32 bits (uint32) */
    PBW_ACT_S32,        /* singular varint, zigzag-decoded to 32 bits */
    PBW_ACT_S64,        /* singular varint, zigzag-decoded to 64 bits */
    PBW_ACT_BOOL,       /* singular varint, normalized to 0/1 (bool) */
    PBW_ACT_F32,        /* singular 4 little-endian bytes */
    PBW_ACT_F64,        /* singular 8 little-endian bytes */
    PBW_ACT_STR,        /* singular string/bytes, first or later occurrence */
    PBW_ACT_MSG,        /* singular message, merge or replace */
    PBW_ACT_REP_VARINT, /* repeated unpacked varint scalar */
    PBW_ACT_REP_F32,    /* repeated unpacked 4-byte scalar */
    PBW_ACT_REP_F64,    /* repeated unpacked 8-byte scalar */
    PBW_ACT_REP_STR,    /* repeated string/bytes */
    PBW_ACT_REP_MSG     /* repeated message */
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
/* Locale-independent decimal conversions for the JSON and text formats:
 * pbw_locale_dot() is asked once per print/parse call, then passed down. The
 * C-locale path is inlined, so it costs exactly the libc call it replaced. */
int pbw_locale_dot(void);
double pbw_strtod_slow(const char *text, char **end, double value, char *stop);
int pbw_format_g_slow(char *buf, int n);

static inline double
pbw_strtod(const char *text, char **end, int dot)
{
    char *stop = NULL;
    double value = strtod(text, &stop);

    if (dot || stop == NULL || *stop != '.') {
        if (end != NULL) {
            *end = stop;
        }
        return value;
    }
    return pbw_strtod_slow(text, end, value, stop);
}

static inline int
pbw_format_g(char *buf, size_t cap, int precision, double value, int dot)
{
    int n = snprintf(buf, cap, "%.*g", precision, value);

    if (dot || n <= 0 || (size_t)n >= cap) {
        return n;
    }
    return pbw_format_g_slow(buf, n);
}
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
/* Wrap caller memory: reserve never reallocates, it fails once the buffer is
 * full. The memory stays the caller's. */
void pbw_buf_init_fixed(pbw_buf *buf, void *data, size_t cap);
void pbw_buf_free(pbw_buf *buf);
/* Grows buf so that `extra` more bytes fit; capacity survives across calls. */
int pbw_buf_reserve(pbw_buf *buf, size_t extra);
uint32_t pbw_read_le32(const uint8_t *in);

/* arena.c */
void *pbw_arena_alloc_slow(pbw_arena *arena, size_t size);

static inline void *
pbw_arena_alloc(pbw_arena *arena, size_t size)
{
    /* Sizes near SIZE_MAX would wrap the align_up below into a tiny request:
     * bounce them to the slow path, which rejects what it cannot hold. */
    pbw_arena_chunk *chunk;
    void *ptr;

    if (arena == NULL) {
        return NULL;
    }
    chunk = arena->head;
    if (PBW_EXPECT(size > SIZE_MAX - (size_t)7u, 0)) {
        return pbw_arena_alloc_slow(arena, size);
    }
    size = (size + 7u) & ~(size_t)7u;
    if (chunk == NULL || !pbw_cap_holds(chunk->cap, chunk->used) ||
        chunk->cap - chunk->used < size) {
        return pbw_arena_alloc_slow(arena, size);
    }
    ptr = (char *)chunk + PBW_CHUNK_HDR + chunk->used;
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
/* Fills value with a NUL-terminated copy of data; 0 on allocation failure. */
int pbw_value_init_bytes(pbw_value *value, pbw_arena *arena, const void *data,
                         size_t len);
/* Copies value into the field slot and marks it present (decoder storage). */
void pbw_message_put(pbw_message *msg, size_t index, const pbw_value *value);
pbw_value *pbw_message_list_for(pbw_message *msg, const pbw_field *field);
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

/* A MessageSet item on the encode path: a singular message extension of a
 * message_set_wire_format parent travels as one group item, not as its own
 * field number. */
static inline int
pbw_is_messageset_item(const pbw_field *field, const pbw_message *msg)
{
    return msg->desc->is_messageset && field->is_extension &&
           field->type == PBW_TYPE_MESSAGE &&
           field->label != PBW_LABEL_REPEATED;
}

/* A proto2 (closed) enum rejects unknown numbers: the reference parsers move
 * them into the unknown fields instead of the field. Editions may close one
 * enum field (or one enum type) in an otherwise open file; the per-field
 * override wins, otherwise the enum type's dialect decides. */
static inline int
pbw_enum_is_closed(const pbw_field *field)
{
    if (field->enum_open == 2) {
        return 1;
    }
    if (field->enum_open == 1) {
        return 0;
    }
    return field->type == PBW_TYPE_ENUM && field->sub != NULL &&
           !field->sub->proto3;
}

/* A field whose absence fails decoding (PBW_ERR_REQUIRED). Proto2 spells it
 * with the REQUIRED label; editions spell it with field_presence =
 * LEGACY_REQUIRED (the label stays OPTIONAL there, by spec). */
static inline int
pbw_field_is_required(const pbw_field *field, const pbw_descriptor *msg)
{
    if (field->label == PBW_LABEL_REQUIRED && !msg->proto3) {
        return 1;
    }
    return msg->edition != 0 && field->presence == 3 &&
           field->label != PBW_LABEL_REPEATED;
}

/* A plain proto3 field with neither a oneof nor explicit presence: the
 * JSON/text printers omit it when it holds the default value, like the
 * reference printers. `presence` 1 is EXPLICIT (editions, proto3 `optional`),
 * 3 is LEGACY_REQUIRED. */
static inline int
pbw_field_implicit_presence(const pbw_field *field, const pbw_descriptor *msg)
{
    if (!msg->proto3 || field->oneof_index != 0) {
        return 0;
    }
    return field->presence != 1 && field->presence != 3;
}

/* The default value of a present scalar, as the proto3 printers define it:
 * an empty string/bytes, or all-zero bits (`-0.0` has a non-zero sign bit and
 * prints, exactly like the reference). Messages and lists are never default. */
static inline int
pbw_value_is_default(const pbw_value *value)
{
    if (value->kind == PBW_VALUE_BYTES) {
        return value->as.bytes.len == 0;
    }
    if (value->kind == PBW_VALUE_UINT) {
        return value->as.u64 == 0;
    }
    return 0;
}

/* The tree builders below are inline: the decoder runs one per decoded value
 * (or nested message), and the call alone is a measurable slice of a decode.
 * One definition, no copy per caller; a translation unit that never calls one
 * emits nothing for it. They sit after arena.c because they allocate. */

/* A message is one allocation: the struct, its value slots, the presence bitmap
 * and the oneof cases follow each other in the same block. Only the bitmap and
 * the oneof cases are zeroed; a value slot is written before it is marked
 * present. */
PBW_INLINE pbw_message *
pbw_message_new_a(pbw_arena *arena, const pbw_descriptor *desc)
{
    pbw_message *msg;
    size_t values_off;
    size_t present_off;
    size_t oneof_off;
    size_t size;
    size_t words;
    uint8_t *block;

    if (desc == NULL || desc->is_enum) {
        return NULL;
    }
    if (desc->field_count > (SIZE_MAX - sizeof(pbw_message)) / sizeof(pbw_value)) {
        return NULL;
    }
    /* sizeof(pbw_message) is 44 bytes on ILP32, so a slot array that simply
     * follows it would put every 64-bit value 4 bytes off alignment where
     * 8-byte alignment is required (arm/aarch32). Round up: free on LP64. */
    values_off = (sizeof(pbw_message) + 7u) & ~(size_t)7u;
    present_off = values_off + desc->field_count * sizeof(pbw_value);
    words = (desc->field_count + 31u) / 32u;
    /* The bitmap plus alignment slack must fit past present_off: words * 4
     * cannot wrap on its own (field_count is bounded above), so this
     * subtraction sequence is well-defined and rejects the wrap. */
    if (present_off > SIZE_MAX - words * sizeof(uint32_t) - (size_t)3) {
        return NULL;
    }
    oneof_off = (present_off + words * sizeof(uint32_t) + 3u) & ~(size_t)3u;
    if ((size_t)desc->oneof_count + 1u >
        (SIZE_MAX - oneof_off) / sizeof(uint32_t)) {
        return NULL;
    }
    size = oneof_off + ((size_t)desc->oneof_count + 1u) * sizeof(uint32_t);

    if (arena != NULL) {
        block = (uint8_t *)pbw_arena_alloc(arena, size);
    } else {
        block = (uint8_t *)malloc(size);
    }
    if (block == NULL) {
        return NULL;
    }
    /* Every member is assigned below: the struct needs no blanket zeroing,
     * only the presence bitmap and the oneof cases start cleared. */
    msg = (pbw_message *)block;
    msg->desc = desc;
    msg->arena = arena;
    msg->own = NULL;
    msg->nfields = desc->field_count;
    msg->values = (pbw_value *)(void *)(block + values_off);
    msg->present = (uint32_t *)(void *)(block + present_off);
    msg->oneof_case = (uint32_t *)(void *)(block + oneof_off);
    msg->unknown = NULL;
    msg->unknown_len = 0;
    msg->unknown_cap = 0;
    msg->wire_len = 0;
    if (words > 0) {
        memset(msg->present, 0, words * sizeof(uint32_t));
    }
    if (desc->oneof_count > 0) {
        memset(msg->oneof_case, 0,
               ((size_t)desc->oneof_count + 1u) * sizeof(uint32_t));
    }
    return msg;
}

PBW_INLINE pbw_value *
pbw_value_new_a(pbw_arena *arena, pbw_value_kind kind)
{
    pbw_value *value;

    if (arena != NULL) {
        value = (pbw_value *)pbw_arena_alloc(arena, sizeof(pbw_value));
    } else {
        value = (pbw_value *)malloc(sizeof(pbw_value));
    }
    if (value != NULL) {
        value->kind = kind;
        value->flags = arena != NULL ? PBW_VF_ARENA : 0;
    }
    return value;
}

PBW_INLINE pbw_value *
pbw_value_new_uint_a(pbw_arena *arena, uint64_t bits)
{
    pbw_value *value = pbw_value_new_a(arena, PBW_VALUE_UINT);

    if (value != NULL) {
        value->as.u64 = bits;
    }
    return value;
}

PBW_INLINE pbw_value *
pbw_value_new_bytes_a(pbw_arena *arena, const void *data, size_t len)
{
    pbw_value *value;
    uint8_t *payload;

    if (arena == NULL) {
        value = pbw_value_new_a(NULL, PBW_VALUE_BYTES);
        if (value == NULL || !pbw_value_init_bytes(value, NULL, data, len)) {
            pbw_value_free(value);
            return NULL;
        }
        return value;
    }
    /* Arena: the value struct and its payload are one bump, so a decoded
     * string or bytes item costs one allocation instead of two. The payload
     * is a byte string, so it needs no stronger alignment than the arena
     * already provides. */
    if (len > SIZE_MAX - sizeof(*value) - 8) {
        return NULL;
    }
    value = (pbw_value *)pbw_arena_alloc(arena, sizeof(*value) + len + 1);
    if (value == NULL) {
        return NULL;
    }
    if (len > 0 && data == NULL) {
        data = pbw_empty_input;
    }
    payload = (uint8_t *)(value + 1);
    if (len > 0) {
        if (data != pbw_empty_input) {
            memcpy(payload, data, len);
        } else {
            memset(payload, 0, len);
        }
    }
    payload[len] = '\0';
    value->kind = PBW_VALUE_BYTES;
    value->flags = PBW_VF_ARENA;
    value->as.bytes.data = payload;
    value->as.bytes.len = len;
    return value;
}

PBW_INLINE pbw_value *
pbw_value_new_message_a(pbw_arena *arena, pbw_message *msg)
{
    pbw_value *value = pbw_value_new_a(arena, PBW_VALUE_MESSAGE);

    if (value == NULL) {
        return NULL;
    }
    value->as.msg = msg;
    return value;
}

PBW_INLINE int
pbw_value_list_push(pbw_value *list, pbw_value *item, pbw_arena *arena)
{
    pbw_value **items;

    if (list == NULL || item == NULL || list->kind != PBW_VALUE_LIST ||
        list->as.list.len == UINT32_MAX) {
        return 0;
    }
    if (list->as.list.len == list->as.list.cap) {
        uint32_t cap = list->as.list.cap ? list->as.list.cap * 2u : 8u;
        size_t bytes;

        if (cap < list->as.list.cap) {
            /* Doubling overflowed: the list cannot grow any further. */
            return 0;
        }
        bytes = (size_t)cap * sizeof(*items);
        if (bytes / sizeof(*items) != cap) {
            /* 32-bit size_t: the byte count wrapped. */
            return 0;
        }
        if (arena != NULL) {
            /* No realloc in an arena: move to a bigger block, the old one is
             * recycled by pbw_ctx_reset(). */
            items = (pbw_value **)pbw_arena_alloc(arena, bytes);
            if (items == NULL) {
                return 0;
            }
            if (list->as.list.len > 0) {
                memcpy(items, list->as.list.items,
                       list->as.list.len * sizeof(*items));
            }
        } else {
            items = (pbw_value **)realloc(list->as.list.items, bytes);
            if (items == NULL) {
                return 0;
            }
        }
        list->as.list.items = items;
        list->as.list.cap = cap;
    }
    /* The list owns the wrapper from here on: pbw_value_free() on an item
     * handed out by pbw_value_at() is the documented no-op, and the list
     * destroy path releases the wrapper itself. */
    item->flags |= PBW_VF_EMBEDDED;
    list->as.list.items[list->as.list.len++] = item;
    return 1;
}

#endif /* PBWIRE_INTERNAL_H */
