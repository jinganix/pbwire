/* Shim: re-export the C core header as module CPbwire.
 *
 * The wire format lives in the C core; this binding owns no wire code. The
 * relative include keeps working wherever the checkout is, because this
 * package always sits at bindings/swift (five levels below the repo root).
 *
 * The binding also compiles the C binding's layout codec
 * (`bindings/c/runtime/pbw_struct.c`, symlinked next to this file) into this
 * target, so the Swift `shim` arm reaches `pbw_struct_codec_*` in one call.
 * pbw_struct.h includes "pbwire.h" by name; the symlink in this directory is
 * what resolves it, so no header search path is needed. */
#include "../../../../../include/pbwire.h"
#include "../../../../c/runtime/pbw_struct.h"

/* One layout row as Swift passes it (all 64-bit, so the mirror table needs no
 * padding rules of its own). `has`/`count`/`cs` are 0 when the message has no
 * such slot; a MESSAGE row's child layout is resolved from the schema, so the
 * row carries no name. Declared here rather than pulled from Swift memory so
 * the two sides cannot disagree about the struct. */
typedef struct pbw_swift_row {
    uint64_t number;
    uint64_t kind; /* pbw_struct_kind_t */
    uint64_t repeated;
    uint64_t offset;
    uint64_t has;
    uint64_t count;
    uint64_t elem; /* MESSAGE: array stride of the child mirror; 0 = the child's size */
    uint64_t type; /* pbw_type, 0 = let the codec ask the schema */
    uint64_t wire;
    uint64_t packed;
    uint64_t required;
    uint64_t oneof;
    uint64_t cs;
} pbw_swift_row;

typedef struct pbw_swift_type pbw_swift_type;
typedef struct pbw_swift_schema pbw_swift_schema;

#ifdef __cplusplus
extern "C" {
#endif

/* Compile check that the re-exported header parses as C. */
const char *cpbwire_header_check(void);

/* Wraps a core schema (created by libpbwire and kept alive by Swift) in the
 * registry that resolves child layouts. *out is NULL on failure. */
pbw_status pbw_swift_schema_new(const void *raw, pbw_swift_schema **out);

/* Frees every registered type (codecs before the rows they reference); the
 * core schema belongs to the caller and is left untouched. Safe on NULL. */
void pbw_swift_schema_free(pbw_swift_schema *schema);

/* Registers one message mirror: `rows` is the generated table, `size` the
 * mirror's byte size (MemoryLayout<R>.size). Children must be registered
 * before their parent. *out is NULL on failure. */
pbw_status pbw_swift_type_new(pbw_swift_schema *schema, const char *message,
                              const pbw_swift_row *rows, size_t nrows,
                              size_t size, pbw_swift_type **out);

void pbw_swift_type_free(pbw_swift_type *type);

/* Mirror -> wire bytes in the type's reusable buffer (valid until the next
 * encode of the same type). One call; the image is borrowed. */
pbw_status pbw_swift_encode(pbw_swift_type *type, const void *base,
                            uint8_t **out, size_t *out_len);

/* Wire bytes -> zeroed mirror. Strings, arrays and sub-messages are
 * bump-allocated from the type's arena, so the image is a view of memory the
 * type owns: the next decode (or the type's release) invalidates it. One
 * call; the steady state allocates nothing. */
pbw_status pbw_swift_decode(pbw_swift_type *type, const uint8_t *wire,
                            size_t wire_len, void *base);

#ifdef __cplusplus
} /* extern "C" */
#endif
