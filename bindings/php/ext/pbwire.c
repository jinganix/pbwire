/* PHP C extension: native binding over the pbwire C core.
 *
 * Same API as runtime/pbwire.php (the ext-ffi wrapper): Pbwire\Schema,
 * Pbwire\Type, Pbwire\Field, Pbwire\Message and Pbwire\Value. The core is
 * compiled in statically (see config.m4), so the extension needs no library
 * path at run time, exactly like the Ruby C extension.
 *
 * Lifetime rule mirrors the PHP version: a Type keeps its Schema alive, a
 * Field keeps its Type, a Message keeps its Type (plus the parent Message for
 * nested views), and a Value keeps its owner Message (plus the nested Type
 * for message items). The schema therefore outlives every object built from
 * it, whichever side is released first.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <php.h>
#include <ext/standard/info.h>
#include <zend_exceptions.h>
#include <ext/spl/spl_exceptions.h>

#include <pbwire.h>
#include "internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static zend_class_entry *pbwire_error_ce;
static zend_class_entry *pbwire_schema_ce;
static zend_class_entry *pbwire_type_ce;
static zend_class_entry *pbwire_field_ce;
static zend_class_entry *pbwire_message_ce;
static zend_class_entry *pbwire_value_ce;

/* ---------------------------------------------------------------- objects */

typedef struct {
    pbw_schema *schema;
    /* The Schema object plus every live Type. `Schema::close` frees the C
     * schema at once only when it is the last reference; otherwise the last
     * Type releases it, so no Type/Field/Message ever points into freed
     * memory. */
    size_t refs;
} schema_holder;

typedef struct {
    schema_holder *holder;
    bool closed;
    zend_object std;
} schema_obj;

typedef struct {
    zval schema_zv; /* Pbwire\Schema, kept alive */
    const pbw_descriptor *desc;
    zval fields;    /* php array: name -> Pbwire\Field, resolved once */
    pbw_ctx *array_ctx; /* scratch arena/buffer for the one-call array entry */
    zend_object std;
} type_obj;

typedef struct {
    zval schema_zv; /* Pbwire\Schema, kept alive (a holder reference too) */
    const pbw_field *field;
    zend_object std;
} field_obj;

typedef struct {
    zval type_zv;   /* Pbwire\Type, kept alive */
    zval parent_zv; /* Pbwire\Message or IS_UNDEF for roots */
    pbw_message *msg;
    pbw_ctx *ctx;   /* non-NULL: root built here, tree lives in it */
    pbw_ctx *arena; /* borrowed: context a nested object builds values into */
    bool owned;
    bool closed;
    size_t encode_hint; /* last encoded size: next encode_to cap, 0 = none */
    zend_object std;
} message_obj;

typedef struct {
    zval owner_zv; /* Pbwire\Message, kept alive */
    zval type_zv;  /* Pbwire\Type of a message item, or IS_UNDEF */
    pbw_value *val;
    zend_object std;
} value_obj;

static inline schema_obj *schema_from(zend_object *o) {
    return (schema_obj *)((char *)o - XtOffsetOf(schema_obj, std));
}
static inline type_obj *type_from(zend_object *o) {
    return (type_obj *)((char *)o - XtOffsetOf(type_obj, std));
}
static inline field_obj *field_from(zend_object *o) {
    return (field_obj *)((char *)o - XtOffsetOf(field_obj, std));
}
static inline message_obj *message_from(zend_object *o) {
    return (message_obj *)((char *)o - XtOffsetOf(message_obj, std));
}
static inline value_obj *value_from(zend_object *o) {
    return (value_obj *)((char *)o - XtOffsetOf(value_obj, std));
}

static zend_object_handlers schema_handlers;
static zend_object_handlers type_handlers;
static zend_object_handlers field_handlers;
static zend_object_handlers message_handlers;
static zend_object_handlers value_handlers;

/* ---------------------------------------------------------------- helpers */

static void throw_error(const char *msg) {
    zend_throw_exception(pbwire_error_ce, msg, 0);
}

static void throw_error_status(const char *what, pbw_status status) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s: %s", what, pbw_strerror(status));
    zend_throw_exception(pbwire_error_ce, buf, 0);
}

/* Fetch a Field handle; throws when the zval is not a Pbwire\Field. */
static field_obj *fetch_field(zval *zv) {
    if (Z_TYPE_P(zv) != IS_OBJECT ||
        !instanceof_function(Z_OBJCE_P(zv), pbwire_field_ce)) {
        throw_error("expected a Pbwire\\Field");
        return NULL;
    }
    return field_from(Z_OBJ_P(zv));
}

/* Forward declaration: defined with the Type methods below. */
static const pbw_field *type_field_cached(zval *type_zv, const char *name,
                                         size_t len);

/* Resolve a Field|string argument against a message's type. String names go
 * through the type's Field cache; Field handles are used directly. */
static const pbw_field *resolve_field(zval *type_zv, zval *zv) {
    if (Z_TYPE_P(zv) == IS_STRING) {
        const pbw_field *f = type_field_cached(
            type_zv, Z_STRVAL_P(zv), Z_STRLEN_P(zv));
        if (f == NULL) {
            type_obj *t = type_from(Z_OBJ_P(type_zv));
            char buf[256];
            snprintf(buf, sizeof buf, "%s: no such field: %s",
                     pbw_descriptor_name(t->desc), Z_STRVAL_P(zv));
            throw_error(buf);
            return NULL;
        }
        return f;
    }
    if (Z_TYPE_P(zv) == IS_OBJECT &&
        instanceof_function(Z_OBJCE_P(zv), pbwire_field_ce)) {
        return field_from(Z_OBJ_P(zv))->field;
    }
    throw_error("field must be a name or a Pbwire\\Field");
    return NULL;
}

/* Forward declarations for cross-constructors. */
static int type_create(zval *out, zval *schema_zv, const pbw_descriptor *desc);
static int field_create(zval *out, zval *schema_zv, const pbw_field *field);
static int message_create(zval *out, zval *type_zv, zval *parent_zv,
                          pbw_message *msg, pbw_ctx *ctx, bool owned);

/* Optional `?array $limits` argument (['max_bytes' => n, 'max_depth' => n]):
 * NULL/absent means core defaults. A present array builds a stack struct;
 * absent keys keep the core defaults (no byte cap, PBW_MAX_DEPTH nesting),
 * and a negative value is rejected instead of wrapping into unsigned. */
static int parse_limits(zval *zv, pbw_limits *out, const pbw_limits **used) {
    zval *v;
    zend_long n;

    *used = NULL;
    if (zv == NULL || Z_TYPE_P(zv) == IS_NULL) {
        return 0;
    }
    if (Z_TYPE_P(zv) != IS_ARRAY) {
        throw_error("limits must be an array");
        return -1;
    }
    out->max_bytes = 0;
    out->max_depth = 0;
    v = zend_hash_str_find(Z_ARRVAL_P(zv), "max_bytes", 9);
    if (v != NULL) {
        if (Z_TYPE_P(v) != IS_LONG || (n = Z_LVAL_P(v)) < 0) {
            throw_error("limits['max_bytes'] must be a non-negative integer");
            return -1;
        }
        out->max_bytes = (size_t)n;
    }
    v = zend_hash_str_find(Z_ARRVAL_P(zv), "max_depth", 9);
    if (v != NULL) {
        if (Z_TYPE_P(v) != IS_LONG || (n = Z_LVAL_P(v)) < 0 ||
            (uint64_t)n > UINT32_MAX) {
            throw_error("limits['max_depth'] must be a non-negative integer");
            return -1;
        }
        out->max_depth = (uint32_t)n;
    }
    *used = out;
    return 0;
}

/* ---------------------------------------------------------------- Schema */

static zend_object *schema_create(zend_class_entry *ce) {
    schema_obj *o =
        ecalloc(1, sizeof(schema_obj) + zend_object_properties_size(ce));
    zend_object_std_init(&o->std, ce);
    object_properties_init(&o->std, ce);
    o->std.handlers = &schema_handlers;
    o->holder = NULL;
    o->closed = false;
    return &o->std;
}

/* Frees the C schema when its last reference goes away; the holder itself
 * lives as long as the Schema object that owns it. */
static void holder_release(schema_holder *holder) {
    if (holder == NULL) {
        return;
    }
    if (--holder->refs == 0 && holder->schema != NULL) {
        pbw_schema_free(holder->schema);
        holder->schema = NULL;
    }
}

static void schema_free(zend_object *o) {
    schema_obj *s = schema_from(o);
    if (s->holder != NULL) {
        /* No Type is alive here: a Type keeps the Schema object (and this
         * object cannot be destroyed while one does). Without close() the
         * holder still carries the Schema's own reference, which frees the
         * C schema; close() already released it. */
        if (!s->closed) {
            holder_release(s->holder);
        }
        s->holder->schema = NULL;
        efree(s->holder);
        s->holder = NULL;
    }
    zend_object_std_dtor(&s->std);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_schema_fromfile, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_schema_frombytes, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, bytes, IS_STRING, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limits, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_schema_service, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_schema_resolve, 0, 0, 2)
ZEND_ARG_TYPE_INFO(0, service, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, method, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_schema_message, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_noargs, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Schema, __construct) {
    throw_error("use Schema::fromFile() or Schema::fromBytes()");
}

PHP_METHOD(Schema, fromFile) {
    char *path;
    size_t path_len;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_PATH(path, path_len)
    ZEND_PARSE_PARAMETERS_END();

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        char buf[512];
        snprintf(buf, sizeof buf, "pbwire: cannot read %s", path);
        throw_error(buf);
        RETURN_THROWS();
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        throw_error("pbwire: cannot read descriptor set");
        RETURN_THROWS();
    }
    char *data = emalloc(size > 0 ? (size_t)size : 1);
    size_t got = size > 0 ? fread(data, 1, (size_t)size, f) : 0;
    fclose(f);
    if ((long)got != size) {
        efree(data);
        throw_error("pbwire: cannot read descriptor set");
        RETURN_THROWS();
    }

    pbw_schema *schema = NULL;
    pbw_status st =
        pbw_descriptor_set_load(data, (size_t)size, &schema);
    efree(data);
    if (st != PBW_OK) {
        char buf[1024];
        const char *why =
            schema != NULL ? pbw_schema_error(schema) : "";
        if (why == NULL) {
            why = "";
        }
        snprintf(buf, sizeof buf, "descriptor set: %s",
                 why[0] != '\0' ? why : pbw_strerror(st));
        if (schema != NULL) {
            pbw_schema_free(schema);
        }
        throw_error(buf);
        RETURN_THROWS();
    }
    if (schema == NULL) {
        throw_error("descriptor set: no schema returned");
        RETURN_THROWS();
    }

    object_init_ex(return_value, pbwire_schema_ce);
    {
        schema_obj *s = schema_from(Z_OBJ_P(return_value));

        s->holder = ecalloc(1, sizeof(*s->holder));
        s->holder->schema = schema;
        s->holder->refs = 1;
    }
}

PHP_METHOD(Schema, fromBytes) {
    char *bytes;
    size_t len;
    zval *limits_zv = NULL;
    pbw_limits lim;
    const pbw_limits *limp = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_STRING(bytes, len)
    Z_PARAM_OPTIONAL
    Z_PARAM_ZVAL(limits_zv)
    ZEND_PARSE_PARAMETERS_END();

    if (parse_limits(limits_zv, &lim, &limp) != 0) {
        RETURN_THROWS();
    }
    pbw_schema *schema = NULL;
    pbw_status st = limp == NULL ? pbw_descriptor_set_load(bytes, len, &schema)
                                  : pbw_descriptor_set_load_limits(bytes, len, limp, &schema);
    if (st != PBW_OK) {
        char buf[1024];
        const char *why =
            schema != NULL ? pbw_schema_error(schema) : "";
        if (why == NULL) {
            why = "";
        }
        snprintf(buf, sizeof buf, "descriptor set: %s",
                 why[0] != '\0' ? why : pbw_strerror(st));
        if (schema != NULL) {
            pbw_schema_free(schema);
        }
        throw_error(buf);
        RETURN_THROWS();
    }
    if (schema == NULL) {
        throw_error("descriptor set: no schema returned");
        RETURN_THROWS();
    }

    object_init_ex(return_value, pbwire_schema_ce);
    {
        schema_obj *s = schema_from(Z_OBJ_P(return_value));

        s->holder = ecalloc(1, sizeof(*s->holder));
        s->holder->schema = schema;
        s->holder->refs = 1;
    }
}

PHP_METHOD(Schema, message) {
    char *name;
    size_t name_len;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    schema_obj *s = schema_from(Z_OBJ_P(ZEND_THIS));
    if (s->holder == NULL || s->holder->schema == NULL || s->closed) {
        throw_error("schema is closed");
        RETURN_THROWS();
    }
    const pbw_descriptor *desc = pbw_schema_find(s->holder->schema, name);
    if (desc == NULL) {
        char buf[512];
        snprintf(buf, sizeof buf, "no such message type: %s", name);
        throw_error(buf);
        RETURN_THROWS();
    }
    if (type_create(return_value, ZEND_THIS, desc) != 0) {
        RETURN_THROWS();
    }
}

PHP_METHOD(Schema, services) {
    ZEND_PARSE_PARAMETERS_NONE();
    schema_obj *s = schema_from(Z_OBJ_P(ZEND_THIS));
    if (s->holder == NULL || s->holder->schema == NULL || s->closed) {
        throw_error("schema is closed");
        RETURN_THROWS();
    }
    size_t count = pbw_schema_service_count(s->holder->schema);
    array_init_size(return_value, count);
    for (size_t i = 0; i < count; i++) {
        const pbw_service *svc =
            pbw_schema_service_at(s->holder->schema, i);
        if (svc == NULL) {
            throw_error("services: no service returned");
            RETURN_THROWS();
        }
        add_next_index_string(return_value, pbw_service_name(svc));
    }
}

PHP_METHOD(Schema, service) {
    char *name;
    size_t name_len;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    schema_obj *s = schema_from(Z_OBJ_P(ZEND_THIS));
    if (s->holder == NULL || s->holder->schema == NULL || s->closed) {
        throw_error("schema is closed");
        RETURN_THROWS();
    }
    const pbw_service *svc =
        pbw_schema_service_find(s->holder->schema, name);
    if (svc == NULL) {
        char buf[512];
        snprintf(buf, sizeof buf, "no such service: %s", name);
        throw_error(buf);
        RETURN_THROWS();
    }
    size_t count = pbw_service_method_count(svc);
    array_init(return_value);
    add_assoc_string(return_value, "name", pbw_service_name(svc));
    zval methods;
    array_init_size(&methods, count);
    for (size_t i = 0; i < count; i++) {
        const pbw_method *m = pbw_service_method_at(svc, i);
        zval entry;
        if (m == NULL) {
            zval_ptr_dtor(&methods);
            throw_error("service: no method returned");
            RETURN_THROWS();
        }
        array_init(&entry);
        add_assoc_string(&entry, "name", pbw_method_name(m));
        add_assoc_string(&entry, "input_type", pbw_method_input_type(m));
        add_assoc_string(&entry, "output_type", pbw_method_output_type(m));
        add_assoc_bool(&entry, "client_streaming",
                       pbw_method_client_streaming(m) != 0);
        add_assoc_bool(&entry, "server_streaming",
                       pbw_method_server_streaming(m) != 0);
        add_next_index_zval(&methods, &entry);
    }
    add_assoc_zval(return_value, "methods", &methods);
}

PHP_METHOD(Schema, resolve) {
    char *service;
    size_t service_len;
    char *method;
    size_t method_len;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_STRING(service, service_len)
    Z_PARAM_STRING(method, method_len)
    ZEND_PARSE_PARAMETERS_END();

    schema_obj *s = schema_from(Z_OBJ_P(ZEND_THIS));
    if (s->holder == NULL || s->holder->schema == NULL || s->closed) {
        throw_error("schema is closed");
        RETURN_THROWS();
    }
    const pbw_descriptor *in = NULL;
    const pbw_descriptor *out = NULL;
    pbw_status st = pbw_service_resolve(s->holder->schema, service, method,
                                        &in, &out);
    if (st != PBW_OK) {
        char buf[512];
        snprintf(buf, sizeof buf, "resolve %s/%s: %s", service, method,
                 pbw_strerror(st));
        throw_error(buf);
        RETURN_THROWS();
    }
    if (in == NULL || out == NULL) {
        throw_error("resolve: no type returned");
        RETURN_THROWS();
    }
    array_init(return_value);
    zval tin, tout;
    if (type_create(&tin, ZEND_THIS, in) != 0) {
        zval_ptr_dtor(return_value);
        RETURN_THROWS();
    }
    if (type_create(&tout, ZEND_THIS, out) != 0) {
        zval_ptr_dtor(&tin);
        zval_ptr_dtor(return_value);
        RETURN_THROWS();
    }
    add_assoc_zval(return_value, "input", &tin);
    add_assoc_zval(return_value, "output", &tout);
}

PHP_METHOD(Schema, close) {
    ZEND_PARSE_PARAMETERS_NONE();
    schema_obj *s = schema_from(Z_OBJ_P(ZEND_THIS));
    if (s->holder != NULL && !s->closed) {
        /* The last side releases the C schema: with a Type (or a Message
         * through its Type) still alive, closing only drains the Schema's own
         * reference so those objects keep valid descriptor pointers. */
        holder_release(s->holder);
    }
    s->closed = true;
}

/* ---------------------------------------------------------------- Type */

static zend_object *type_create_obj(zend_class_entry *ce) {
    type_obj *o =
        ecalloc(1, sizeof(type_obj) + zend_object_properties_size(ce));
    zend_object_std_init(&o->std, ce);
    object_properties_init(&o->std, ce);
    o->std.handlers = &type_handlers;
    ZVAL_UNDEF(&o->schema_zv);
    ZVAL_UNDEF(&o->fields);
    o->desc = NULL;
    o->array_ctx = NULL;
    return &o->std;
}

static void type_free(zend_object *o) {
    type_obj *t = type_from(o);
    if (Z_TYPE(t->schema_zv) != IS_UNDEF) {
        schema_obj *s = schema_from(Z_OBJ(t->schema_zv));

        holder_release(s->holder);
        zval_ptr_dtor(&t->schema_zv);
    }
    if (Z_TYPE(t->fields) == IS_ARRAY) {
        zval_ptr_dtor(&t->fields);
    }
    if (t->array_ctx != NULL) {
        pbw_ctx_free(t->array_ctx);
        t->array_ctx = NULL;
    }
    zend_object_std_dtor(&t->std);
}

/* The field cache lives in the struct, not in the properties table, so the
 * Type <-> Field reference cycle (Field keeps its Type alive) is invisible to
 * the Zend collector without this handler: an unreachable schema would then
 * never be freed. Reporting the array lets gc_collect_cycles walk it. */
static HashTable *type_get_gc(zend_object *o, zval **table, int *n) {
    type_obj *t = type_from(o);

    if (Z_TYPE(t->fields) == IS_ARRAY) {
        *table = &t->fields;
        *n = 1;
    } else {
        *n = 0;
    }
    return zend_std_get_properties(o);
}

static int type_create(zval *out, zval *schema_zv, const pbw_descriptor *desc) {
    object_init_ex(out, pbwire_type_ce);
    type_obj *t = type_from(Z_OBJ_P(out));
    ZVAL_COPY(&t->schema_zv, schema_zv);
    schema_from(Z_OBJ_P(schema_zv))->holder->refs++;
    array_init(&t->fields);
    t->desc = desc;
    return 0;
}

/* Resolves a field name against a type, caching the Field handle in the
 * type's array: a set/get by name then costs one hashtable lookup instead
 * of a descriptor scan per call. Returns a borrowed pointer; the cache
 * keeps the handle (and through it the type) alive. */
static const pbw_field *type_field_cached(zval *type_zv, const char *name,
                                         size_t len) {
    type_obj *t = type_from(Z_OBJ_P(type_zv));
    zval *found =
        Z_TYPE(t->fields) == IS_ARRAY
            ? zend_hash_str_find(Z_ARRVAL(t->fields), name, len)
            : NULL;
    const pbw_field *f;
    zval handle;

    if (found != NULL) {
        return field_from(Z_OBJ_P(found))->field;
    }
    f = pbw_descriptor_find_field_name(t->desc, name);
    if (f == NULL) {
        return NULL;
    }
    if (Z_TYPE(t->fields) != IS_ARRAY) {
        array_init(&t->fields);
    }
    field_create(&handle, &t->schema_zv, f);
    if (zend_hash_str_update(Z_ARRVAL(t->fields), name, len, &handle) ==
        NULL) {
        zval_ptr_dtor(&handle);
    }
    return f;
}

/* ------------------------------------------------------- array entry walk */

/* One PHP array in the generated model's shape -> a C tree, all inside one
 * extension call. The array is not a flat memory image: each descriptor field
 * is looked up with zend_hash_str_find and the nested message / repeated list
 * is walked recursively in C. A string value reuses the zend_string the array
 * already holds; nothing is copied into a temporary PHP string. */
#define PBWIRE_ARRAY_MAX_DEPTH 64

static int array_is_message_type(pbw_type type) {
    return type == PBW_TYPE_MESSAGE || type == PBW_TYPE_GROUP;
}

/* One scratch context per Type: reset per call, so steady use stops paying a
 * context malloc/free and keeps the arena chunks and encode buffer. */
static pbw_ctx *type_array_ctx(type_obj *t) {
    if (t->array_ctx == NULL) {
        t->array_ctx = pbw_ctx_new();
    }
    return t->array_ctx;
}

static int array_bad_field(const pbw_field *field, const char *what) {
    char buf[320];

    snprintf(buf, sizeof buf, "field %s: %s", pbw_field_name(field), what);
    throw_error(buf);
    return -1;
}

/* uint64/fixed64: a PHP int up to PHP_INT_MAX, or the full decimal string.
 * A negative int would be a bit pattern, not the value, so it is rejected
 * instead of truncated. */
static int array_uint64(zval *zv, uint64_t *out) {
    if (Z_TYPE_P(zv) == IS_LONG) {
        zend_long n = Z_LVAL_P(zv);

        if (n < 0) {
            throw_error("uint64 must be a non-negative integer or a decimal string");
            return -1;
        }
        *out = (uint64_t)n;
        return 0;
    }
    if (Z_TYPE_P(zv) == IS_STRING) {
        const char *s = Z_STRVAL_P(zv);
        size_t len = Z_STRLEN_P(zv);
        uint64_t acc = 0;

        if (len == 0) {
            throw_error("uint64 decimal string is empty");
            return -1;
        }
        for (size_t i = 0; i < len; i++) {
            unsigned digit;

            if (s[i] < '0' || s[i] > '9') {
                throw_error("uint64 must be a decimal string");
                return -1;
            }
            digit = (unsigned)(s[i] - '0');
            if (acc > (UINT64_MAX - digit) / 10) {
                throw_error("uint64 decimal string overflows");
                return -1;
            }
            acc = acc * 10 + digit;
        }
        *out = acc;
        return 0;
    }
    throw_error("uint64 must be a non-negative integer or a decimal string");
    return -1;
}

static int array_scalar_value(zval *zv, const pbw_field *field, pbw_ctx *ctx,
                              pbw_value **out) {
    pbw_type type = pbw_field_type(field);
    uint64_t bits = 0;

    *out = NULL;
    switch (type) {
    case PBW_TYPE_DOUBLE: {
        double d;

        if (Z_TYPE_P(zv) != IS_DOUBLE && Z_TYPE_P(zv) != IS_LONG) {
            return array_bad_field(field, "expected a number");
        }
        d = zval_get_double(zv);
        memcpy(&bits, &d, sizeof bits);
        break;
    }
    case PBW_TYPE_FLOAT: {
        float f;
        uint32_t ubits;

        if (Z_TYPE_P(zv) != IS_DOUBLE && Z_TYPE_P(zv) != IS_LONG) {
            return array_bad_field(field, "expected a number");
        }
        f = (float)zval_get_double(zv);
        memcpy(&ubits, &f, sizeof ubits);
        bits = ubits;
        break;
    }
    case PBW_TYPE_BOOL:
        if (Z_TYPE_P(zv) != IS_TRUE && Z_TYPE_P(zv) != IS_FALSE) {
            return array_bad_field(field, "expected a bool");
        }
        bits = Z_TYPE_P(zv) == IS_TRUE ? 1u : 0u;
        break;
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
        if (Z_TYPE_P(zv) != IS_STRING) {
            return array_bad_field(field, "expected a string");
        }
        *out = pbw_value_new_bytes_ctx(ctx, Z_STRVAL_P(zv), Z_STRLEN_P(zv));
        if (*out == NULL) {
            throw_error("out of memory");
            return -1;
        }
        return 0;
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64:
        if (array_uint64(zv, &bits) != 0) {
            return -1;
        }
        break;
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32:
        if (Z_TYPE_P(zv) != IS_LONG || Z_LVAL_P(zv) < 0 ||
            (uint64_t)Z_LVAL_P(zv) > UINT32_MAX) {
            return array_bad_field(field, "expected a uint32 integer");
        }
        bits = (uint64_t)Z_LVAL_P(zv);
        break;
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_ENUM:
        if (Z_TYPE_P(zv) != IS_LONG || Z_LVAL_P(zv) < INT32_MIN ||
            Z_LVAL_P(zv) > INT32_MAX) {
            return array_bad_field(field, "expected an int32 integer");
        }
        bits = (uint64_t)(int64_t)Z_LVAL_P(zv);
        break;
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
        if (Z_TYPE_P(zv) != IS_LONG) {
            return array_bad_field(field, "expected an int64 integer");
        }
        bits = (uint64_t)Z_LVAL_P(zv);
        break;
    default:
        return array_bad_field(field, "unsupported array scalar type");
    }
    *out = pbw_value_new_uint_ctx(ctx, bits);
    if (*out == NULL) {
        throw_error("out of memory");
        return -1;
    }
    return 0;
}

static int array_encode_message(zval *arr, pbw_ctx *ctx,
                                const pbw_descriptor *desc, pbw_message *msg,
                                unsigned depth) {
    size_t nfields;
    size_t found = 0;
    size_t i;

    if (depth > PBWIRE_ARRAY_MAX_DEPTH) {
        throw_error("nesting limit exceeded");
        return -1;
    }
    if (Z_TYPE_P(arr) != IS_ARRAY) {
        throw_error("a message must be a PHP array");
        return -1;
    }
    nfields = pbw_descriptor_field_count(desc);
    for (i = 0; i < nfields; i++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, i);
        const char *name = pbw_field_name(field);
        pbw_type type = pbw_field_type(field);
        zval *value = zend_hash_str_find(Z_ARRVAL_P(arr), name, strlen(name));

        if (value == NULL) {
            continue; /* absent, like an unset optional */
        }
        found++;
        if (Z_TYPE_P(value) == IS_NULL) {
            continue;
        }
        if (pbw_field_label(field) == PBW_LABEL_REPEATED) {
            zval *item;

            if (Z_TYPE_P(value) != IS_ARRAY) {
                return array_bad_field(field, "expected a list");
            }
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(value), item) {
                if (Z_TYPE_P(item) == IS_NULL) {
                    return array_bad_field(field, "NULL is not a value");
                }
                if (array_is_message_type(type)) {
                    const pbw_descriptor *sub =
                        pbw_field_message_type(field);
                    pbw_message *child;

                    if (sub == NULL) {
                        return array_bad_field(field, "unresolved message type");
                    }
                    if (Z_TYPE_P(item) != IS_ARRAY) {
                        return array_bad_field(field, "expected a nested array");
                    }
                    child = pbw_message_set_message(msg,
                                                    pbw_field_number(field));
                    if (child == NULL) {
                        return array_bad_field(field, "cannot set message");
                    }
                    if (array_encode_message(item, ctx, sub, child,
                                             depth + 1) != 0) {
                        return -1;
                    }
                } else {
                    pbw_value *val = NULL;
                    pbw_status st;

                    if (array_scalar_value(item, field, ctx, &val) != 0) {
                        return -1;
                    }
                    st = pbw_message_set_field(msg, field, val);
                    if (st != PBW_OK) {
                        pbw_value_free(val);
                        throw_error_status("set", st);
                        return -1;
                    }
                }
            }
            ZEND_HASH_FOREACH_END();
        } else if (array_is_message_type(type)) {
            const pbw_descriptor *sub = pbw_field_message_type(field);
            pbw_message *child;

            if (sub == NULL) {
                return array_bad_field(field, "unresolved message type");
            }
            if (Z_TYPE_P(value) != IS_ARRAY) {
                return array_bad_field(field, "expected a nested array");
            }
            child = pbw_message_set_message(msg, pbw_field_number(field));
            if (child == NULL) {
                return array_bad_field(field, "cannot set message");
            }
            if (array_encode_message(value, ctx, sub, child, depth + 1) != 0) {
                return -1;
            }
        } else {
            pbw_value *val = NULL;
            pbw_status st;

            if (array_scalar_value(value, field, ctx, &val) != 0) {
                return -1;
            }
            st = pbw_message_set_field(msg, field, val);
            if (st != PBW_OK) {
                pbw_value_free(val);
                throw_error_status("set", st);
                return -1;
            }
        }
    }
    /* Every array key either matched a descriptor field above or is a typo:
     * a unique key cannot be consumed twice, so the counts decide. */
    if (found != zend_hash_num_elements(Z_ARRVAL_P(arr))) {
        char buf[512];

        snprintf(buf, sizeof buf, "%s: no such field in the array",
                 pbw_descriptor_name(desc));
        throw_error(buf);
        return -1;
    }
    return 0;
}

/* ------------------------- direct wire->array decode ------------------------- */

/* decodeArray without the tree: one pbw_visit tag scan per message level
 * builds the PHP array straight from the wire. The descriptor column table
 * is the layout: field pointer -> wire facts -> array slot, resolved once
 * per level while the visit callback only converts raw wire bits and records
 * sub-message ranges. Singular scalars land in their slot as they parse
 * (last wins, the merge the tree used to do); repeated values append
 * straight into the list created up front; singular sub-messages remember
 * their payload range and decode once at the end of their level, so split
 * occurrences still merge into one message. Oneof losers are reset after the
 * scan. Every gate-checked field is materialized before return, so the timer
 * needs no PHP-side fold. Raises Pbwire::Error with the same status text
 * pbw_decode reported on malformed bytes, like the tree path did.
 *
 * GC rooting: every zval the scan creates is owned by a frame slot from the
 * moment it is built (owned refcounts never reach zero through later
 * allocations), and lists are appended to in place. Singular sub-message
 * payloads stay as (offset, length) integers into the input buffer while seen
 * once, so the common case copies nothing; only a repeated singular
 * allocates an emalloc accumulator, which is C memory the collector never
 * sees. The input wire string is held by the call frame for the whole call,
 * so range pointers into it stay valid; no Z_STRVAL_P outlives a zval
 * allocation or a recursive decode. Plain C scratch (slots/seen/ranges)
 * lives in emalloc frames that unwind on error with nothing leaked.
 *
 * Unknown fields are skipped exactly like the tree path (the reference
 * decoders keep them, the array has no slot for them); re-encoding the array
 * drops them while Message re-encode preserves them, see pbench/README.md.
 * Packed runs are accepted for repeated packable fields regardless of the
 * declared packed flag, like the tree path; any other wire-type mismatch is
 * skipped as unknown. A closed enum's unknown value leaves the field absent,
 * like the reference parsers. The expected wire type comes from the linked
 * field (field->wire_type), so an Editions DELIMITED-encoded message rides
 * the group path exactly like the decoder. */
typedef struct {
    const pbw_field *field;
    const char *name;
    size_t name_len;
    pbw_type type;
    uint32_t wire;
    uint32_t oneof;
    unsigned repeated : 1;
    unsigned is_msg : 1;
    unsigned group_wire : 1;
    unsigned is_str : 1;
    unsigned is_enum : 1;
    unsigned check_utf8 : 1;
} arr_col;

/* One occurrence of a singular sub-message payload. The first occurrence only
 * records its range; a repeat concatenates into an emalloc accumulator, so
 * the end-of-level decode reads memory the collector never sees. `base` is
 * the current level's buffer; ranges are frame-relative integers. */
typedef struct {
    size_t off;
    size_t len;
    uint8_t *buf;
    size_t use;
    size_t cap;
} arr_sub;

typedef struct arr_frame arr_frame;

/* One column table per descriptor, built once per top-level call and shared
 * by every nested message of that type: a 3000-deep payload names two
 * types, so this replaces 3000 rebuilds with two. Descriptors outlive the
 * call (the Type keeps its Schema alive), so borrowing is safe. */
typedef struct {
    const pbw_descriptor *desc;
    arr_col *cols;
    size_t nfields;
    uint32_t max_oneof;
} arr_layout;

typedef struct {
    uint32_t depth_cap; /* min(user max_depth, PBWIRE_ARRAY_MAX_DEPTH) */
    uint32_t user_dlim; /* effective user max_depth for nested visits */
    int nesting;        /* the cap hit is the 64 default, not the user */
    arr_layout *layouts;
    size_t nlayouts;
    size_t layouts_cap;
} arr_opts;

struct arr_frame {
    const pbw_descriptor *desc;
    const uint8_t *base;
    arr_opts *opts;
    arr_col *cols;      /* borrowed from the per-call layout cache */
    zval *slots;        /* singular values (owned) / repeated lists */
    unsigned char *seen; /* singular presence */
    arr_sub *subs;       /* singular sub-message ranges */
    uint32_t *winner;    /* oneof winners: field index + 1 */
    size_t nfields;
    uint32_t depth;
    void *block; /* one allocation holding slots+seen+subs+winner */
};

static const arr_layout *arr_layout_for(arr_opts *opts,
                                        const pbw_descriptor *desc) {
    size_t i;

    for (i = 0; i < opts->nlayouts; i++) {
        if (opts->layouts[i].desc == desc) {
            return &opts->layouts[i];
        }
    }
    if (opts->nlayouts == opts->layouts_cap) {
        size_t cap = opts->layouts_cap == 0 ? 4 : opts->layouts_cap * 2;

        opts->layouts = erealloc(opts->layouts, cap * sizeof *opts->layouts);
        opts->layouts_cap = cap;
    }
    {
        arr_layout *layout = &opts->layouts[opts->nlayouts];
        size_t nfields = pbw_descriptor_field_count(desc);
        size_t k;

        layout->desc = desc;
        layout->cols =
            nfields == 0 ? NULL : ecalloc(nfields, sizeof *layout->cols);
        layout->nfields = nfields;
        layout->max_oneof = 0;
        for (k = 0; k < nfields; k++) {
            const pbw_field *field = pbw_descriptor_field_at(desc, k);
            arr_col *c = &layout->cols[k];

            c->field = field;
            c->name = pbw_field_name(field);
            c->name_len = strlen(c->name);
            c->type = pbw_field_type(field);
            c->wire = field->wire_type;
            c->oneof = pbw_field_oneof_index(field);
            c->repeated = pbw_field_label(field) == PBW_LABEL_REPEATED;
            c->is_msg =
                c->type == PBW_TYPE_MESSAGE || c->type == PBW_TYPE_GROUP;
            c->group_wire = c->wire == PBW_WIRE_START_GROUP;
            c->is_str = c->type == PBW_TYPE_STRING ||
                        c->type == PBW_TYPE_BYTES;
            c->is_enum = c->type == PBW_TYPE_ENUM;
            c->check_utf8 = c->type == PBW_TYPE_STRING &&
                            pbw_field_requires_utf8_validation(field);
            if (c->oneof > layout->max_oneof) {
                layout->max_oneof = c->oneof;
            }
        }
        opts->nlayouts++;
        return layout;
    }
}

static void arr_opts_free(arr_opts *opts) {
    size_t i;

    for (i = 0; i < opts->nlayouts; i++) {
        efree(opts->layouts[i].cols);
    }
    efree(opts->layouts);
    opts->layouts = NULL;
    opts->nlayouts = opts->layouts_cap = 0;
}

static uint64_t arr_zigzag(uint64_t value, unsigned width) {
    uint64_t mask = width == 32 ? 0xffffffffull : ~0ull;
    uint64_t u = value & mask;

    return ((u >> 1) ^ (0ull - (u & 1))) & mask;
}

static int arr_varint(const uint8_t **pp, const uint8_t *end,
                      uint64_t *out) {
    const uint8_t *p = *pp;
    uint64_t value = 0;
    unsigned shift = 0;

    while (p < end && shift < 64) {
        uint8_t byte = *p++;

        value |= (uint64_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            *pp = p;
            *out = value;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

static uint32_t arr_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t arr_le64(const uint8_t *p) {
    return (uint64_t)arr_le32(p) | ((uint64_t)arr_le32(p + 4) << 32);
}

/* Raw wire bits -> PHP value: the same mapping the tree materialization used
 * (sign extension, zigzag, decimal strings past LONG_MAX). */
static void arr_scalar_zval(pbw_type type, uint64_t bits, zval *out) {
    switch (type) {
    case PBW_TYPE_DOUBLE: {
        double d;

        memcpy(&d, &bits, sizeof d);
        ZVAL_DOUBLE(out, d);
        return;
    }
    case PBW_TYPE_FLOAT: {
        float f;
        uint32_t b = (uint32_t)bits;

        memcpy(&f, &b, sizeof f);
        ZVAL_DOUBLE(out, (double)f);
        return;
    }
    case PBW_TYPE_BOOL:
        ZVAL_BOOL(out, bits != 0);
        return;
    case PBW_TYPE_INT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_ENUM:
        ZVAL_LONG(out, (zend_long)(int64_t)(int32_t)bits);
        return;
    case PBW_TYPE_SINT32:
        ZVAL_LONG(out, (zend_long)(int64_t)(int32_t)arr_zigzag(bits, 32));
        return;
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32:
        ZVAL_LONG(out, (zend_long)(uint32_t)bits);
        return;
    case PBW_TYPE_INT64:
    case PBW_TYPE_SFIXED64:
        ZVAL_LONG(out, (zend_long)(int64_t)bits);
        return;
    case PBW_TYPE_SINT64:
        ZVAL_LONG(out, (zend_long)(int64_t)arr_zigzag(bits, 64));
        return;
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64:
        if (bits > (uint64_t)ZEND_LONG_MAX) {
            char buf[32];

            snprintf(buf, sizeof buf, "%llu", (unsigned long long)bits);
            ZVAL_STRING(out, buf);
        } else {
            ZVAL_LONG(out, (zend_long)bits);
        }
        return;
    default:
        ZVAL_NULL(out);
        return;
    }
}

static void arr_frame_free(arr_frame *f) {
    if (f->block != NULL) {
        size_t i;

        for (i = 0; i < f->nfields; i++) {
            if (f->subs[i].buf != NULL) {
                efree(f->subs[i].buf);
            }
            if (Z_TYPE(f->slots[i]) != IS_UNDEF) {
                zval_ptr_dtor(&f->slots[i]);
            }
        }
        efree(f->block);
        f->block = NULL;
    }
}

/* Closed enums divert unknown numbers to the unknown fields: the value leaves
 * the field absent instead of landing in the array. */
static int arr_enum_kept(const arr_col *c, uint64_t bits) {
    if (!c->is_enum || !pbw_enum_is_closed(c->field)) {
        return 1;
    }
    return pbw_enum_find_name(pbw_field_message_type(c->field),
                               (int32_t)bits, NULL) != 0;
}

static void arr_note_singular(arr_frame *f, arr_col *c, size_t idx) {
    f->seen[idx] = 1;
    if (c->oneof != 0) {
        f->winner[c->oneof] = (uint32_t)idx + 1;
    }
}

static void arr_store_scalar(arr_frame *f, arr_col *c, size_t idx,
                             uint64_t bits) {
    if (!arr_enum_kept(c, bits)) {
        return;
    }
    if (c->repeated) {
        zval v;

        arr_scalar_zval(c->type, bits, &v);
        add_next_index_zval(&f->slots[idx], &v);
        return;
    }
    if (f->seen[idx]) {
        zval_ptr_dtor(&f->slots[idx]);
    }
    arr_scalar_zval(c->type, bits, &f->slots[idx]);
    arr_note_singular(f, c, idx);
}

static pbw_status arr_store_string(arr_frame *f, arr_col *c, size_t idx,
                                   const uint8_t *data, size_t len) {
    if (c->check_utf8 && !pbw_utf8_ok(data, len)) {
        return PBW_ERR_INVALID;
    }
    if (c->repeated) {
        zval v;

        ZVAL_STRINGL(&v, (const char *)data, len);
        add_next_index_zval(&f->slots[idx], &v);
        return PBW_OK;
    }
    if (f->seen[idx]) {
        zval_ptr_dtor(&f->slots[idx]);
    }
    ZVAL_STRINGL(&f->slots[idx], (const char *)data, len);
    arr_note_singular(f, c, idx);
    return PBW_OK;
}

static void arr_sub_record(arr_frame *f, arr_col *c, size_t idx,
                           const uint8_t *body, size_t body_len) {
    arr_sub *s = &f->subs[idx];

    if (c->oneof != 0) {
        if (f->seen[idx] && f->winner[c->oneof] != 0 &&
            f->winner[c->oneof] != (uint32_t)idx + 1) {
            /* Another member intervened: the pending bytes are dropped,
             * like the tree that the oneof switch cleared. */
            if (s->buf != NULL) {
                efree(s->buf);
                s->buf = NULL;
                s->use = s->cap = 0;
            }
            f->seen[idx] = 0;
        }
        f->winner[c->oneof] = (uint32_t)idx + 1;
    }
    if (!f->seen[idx]) {
        s->off = (size_t)(body - f->base);
        s->len = body_len;
        s->buf = NULL;
        s->use = s->cap = 0;
    } else if (s->buf == NULL) {
        s->cap = s->len + body_len;
        s->buf = emalloc(s->cap);
        memcpy(s->buf, f->base + s->off, s->len);
        memcpy(s->buf + s->len, body, body_len);
        s->use = s->cap;
    } else {
        if (s->use + body_len > s->cap) {
            s->cap = s->use + body_len;
            s->buf = erealloc(s->buf, s->cap);
        }
        memcpy(s->buf + s->use, body, body_len);
        s->use += body_len;
    }
    f->seen[idx] = 1;
}

static pbw_status arr_decode(const pbw_descriptor *desc, const uint8_t *wire,
                             size_t wire_len, const pbw_limits *limits,
                             uint32_t depth, arr_opts *opts, zval *out);

static pbw_status arr_message_item(arr_frame *f, arr_col *c, size_t idx,
                                   const uint8_t *body, size_t body_len) {
    const pbw_descriptor *sub = pbw_field_message_type(c->field);
    pbw_limits nested;
    zval child;
    pbw_status st;

    if (sub == NULL) {
        return PBW_ERR_INVALID;
    }
    ZVAL_UNDEF(&child);
    nested.max_bytes = 0;
    nested.max_depth = f->opts->user_dlim;
    st = arr_decode(sub, body, body_len, &nested, f->depth + 1, f->opts,
                     &child);
    if (st != PBW_OK) {
        return st;
    }
    add_next_index_zval(&f->slots[idx], &child);
    return PBW_OK;
}

/* A packed run of scalars: whole values only, like the tree path, so a
 * truncated tail fails instead of silently dropping bytes. */
static pbw_status arr_packed(arr_frame *f, arr_col *c, size_t idx,
                             const uint8_t *data, size_t len) {
    pbw_type type = c->type;

    if (len == 0) {
        return PBW_OK;
    }
    if (type == PBW_TYPE_FIXED32 || type == PBW_TYPE_SFIXED32 ||
        type == PBW_TYPE_FLOAT) {
        const uint8_t *p = data;
        const uint8_t *end = data + len;

        if (len % 4 != 0) {
            return PBW_ERR_INVALID;
        }
        while (p < end) {
            arr_store_scalar(f, c, idx, arr_le32(p));
            p += 4;
        }
        return PBW_OK;
    }
    if (type == PBW_TYPE_FIXED64 || type == PBW_TYPE_SFIXED64 ||
        type == PBW_TYPE_DOUBLE) {
        const uint8_t *p = data;
        const uint8_t *end = data + len;

        if (len % 8 != 0) {
            return PBW_ERR_INVALID;
        }
        while (p < end) {
            arr_store_scalar(f, c, idx, arr_le64(p));
            p += 8;
        }
        return PBW_OK;
    }
    {
        const uint8_t *p = data;
        const uint8_t *end = data + len;
        uint64_t raw;

        while (p < end) {
            if (!arr_varint(&p, end, &raw)) {
                return PBW_ERR_INVALID;
            }
            arr_store_scalar(f, c, idx, raw);
        }
        return PBW_OK;
    }
}

static size_t arr_tag_len(uint32_t number, uint32_t wire) {
    uint64_t tag = ((uint64_t)number << 3) | wire;
    size_t len = 1;

    while (tag >= 0x80) {
        tag >>= 7;
        len++;
    }
    return len;
}

static pbw_status arr_visit(void *ud, const pbw_field *field, uint32_t number,
                            uint32_t wire_type,
                            const pbw_visit_value *value) {
    arr_frame *f = (arr_frame *)ud;
    size_t idx;
    arr_col *c;
    (void)number;

    if (field == NULL) {
        return PBW_OK; /* unknown: already skipped by the scan */
    }
    for (idx = 0; idx < f->nfields; idx++) {
        if (f->cols[idx].field == field) {
            break;
        }
    }
    if (idx == f->nfields) {
        return PBW_OK; /* extension or otherwise not materialized */
    }
    c = &f->cols[idx];
    if (wire_type != c->wire) {
        /* A repeated numeric field may arrive packed regardless of the
         * declared packed flag; anything else mismatched rides as
         * unknown, like the decoder. */
        if (wire_type == PBW_WIRE_LEN && c->repeated &&
            pbw_is_packable(c->type)) {
            return arr_packed(f, c, idx, value->data, value->len);
        }
        return PBW_OK;
    }
    if (c->is_msg) {
        const uint8_t *body = value->data;
        size_t body_len = value->len;

        if (c->group_wire) {
            size_t tl =
                arr_tag_len(pbw_field_number(c->field), PBW_WIRE_START_GROUP);

            if (value->len < 2 * tl) {
                return PBW_ERR_INVALID;
            }
            body += tl;
            body_len -= 2 * tl;
        }
        if (c->repeated) {
            return arr_message_item(f, c, idx, body, body_len);
        }
        arr_sub_record(f, c, idx, body, body_len);
        return PBW_OK;
    }
    if (c->is_str) {
        return arr_store_string(f, c, idx, value->data, value->len);
    }
    arr_store_scalar(f, c, idx, value->u64);
    return PBW_OK;
}

/* Oneof losers go back to absent after the scan; singular sub-messages decode
 * once from their merged payloads; then the array is assembled in descriptor
 * order, which is the order the tree materialization produced, so strict
 * comparisons against it keep passing. */
static pbw_status arr_finish(arr_frame *f, arr_opts *opts, zval *out) {
    size_t i;

    for (i = 0; i < f->nfields; i++) {
        arr_col *c = &f->cols[i];

        if (!c->repeated && !f->seen[i] &&
            pbw_field_is_required(c->field, f->desc)) {
            return PBW_ERR_REQUIRED;
        }
    }
    for (i = 0; i < f->nfields; i++) {
        arr_col *c = &f->cols[i];

        if (c->oneof != 0 && f->seen[i] &&
            f->winner[c->oneof] != (uint32_t)i + 1) {
            if (f->subs[i].buf != NULL) {
                efree(f->subs[i].buf);
                f->subs[i].buf = NULL;
                f->subs[i].use = f->subs[i].cap = 0;
            }
            if (Z_TYPE(f->slots[i]) != IS_UNDEF) {
                zval_ptr_dtor(&f->slots[i]);
                ZVAL_UNDEF(&f->slots[i]);
            }
            f->seen[i] = 0;
        }
    }
    for (i = 0; i < f->nfields; i++) {
        arr_col *c = &f->cols[i];

        if (!c->is_msg || c->repeated || !f->seen[i]) {
            continue;
        }
        if (c->oneof != 0 && f->winner[c->oneof] != (uint32_t)i + 1) {
            continue;
        }
        {
            const pbw_descriptor *sub =
                pbw_field_message_type(c->field);
            const uint8_t *ptr;
            size_t len;
            pbw_limits nested;
            zval child;
            pbw_status st;

            if (sub == NULL) {
                return PBW_ERR_INVALID;
            }
            if (f->subs[i].buf != NULL) {
                ptr = f->subs[i].buf;
                len = f->subs[i].use;
            } else {
                ptr = f->base + f->subs[i].off;
                len = f->subs[i].len;
            }
            ZVAL_UNDEF(&child);
            nested.max_bytes = 0;
            nested.max_depth = opts->user_dlim;
            st = arr_decode(sub, ptr, len, &nested, f->depth + 1, opts,
                             &child);
            if (st != PBW_OK) {
                return st;
            }
            f->slots[i] = child;
        }
    }
    array_init_size(out, (uint32_t)f->nfields);
    for (i = 0; i < f->nfields; i++) {
        arr_col *c = &f->cols[i];

        if (c->repeated) {
            add_assoc_zval_ex(out, c->name, c->name_len, &f->slots[i]);
            ZVAL_UNDEF(&f->slots[i]);
            continue;
        }
        if (f->seen[i]) {
            add_assoc_zval_ex(out, c->name, c->name_len, &f->slots[i]);
            ZVAL_UNDEF(&f->slots[i]);
        }
    }
    return PBW_OK;
}

static pbw_status arr_decode(const pbw_descriptor *desc, const uint8_t *wire,
                             size_t wire_len, const pbw_limits *limits,
                             uint32_t depth, arr_opts *opts, zval *out) {
    const arr_layout *layout;
    arr_frame f;
    pbw_status st;
    size_t i;
    size_t seen_off;
    size_t subs_off;
    size_t winner_off;
    size_t total;

    /* Core allows nesting down to the cap (`depth >= dlim` is checked before
     * descending, so a frame at exactly the cap still decodes); deeper fails
     * the same way the tree walk did. */
    if (depth > opts->depth_cap) {
        if (opts->depth_cap == PBWIRE_ARRAY_MAX_DEPTH) {
            opts->nesting = 1;
        }
        return PBW_ERR_LIMIT;
    }
    layout = arr_layout_for(opts, desc);
    memset(&f, 0, sizeof f);
    f.desc = desc;
    f.base = wire;
    f.opts = opts;
    f.depth = depth;
    f.cols = layout->cols;
    f.nfields = layout->nfields;
    /* One zeroed block per level: IS_UNDEF slots, clear presence, empty
     * ranges and oneof winners. The layout columns stay cached. */
    seen_off = f.nfields * sizeof *f.slots;
    subs_off = (seen_off + f.nfields + 7) & ~(size_t)7;
    winner_off = subs_off + f.nfields * sizeof *f.subs;
    total = winner_off + (layout->max_oneof + 1) * sizeof *f.winner;
    f.block = emalloc(total == 0 ? 1 : total);
    memset(f.block, 0, total == 0 ? 1 : total);
    f.slots = (zval *)f.block;
    f.seen = (unsigned char *)((char *)f.block + seen_off);
    f.subs = (arr_sub *)((char *)f.block + subs_off);
    f.winner = (uint32_t *)((char *)f.block + winner_off);
    for (i = 0; i < f.nfields; i++) {
        if (f.cols[i].repeated) {
            array_init(&f.slots[i]);
        }
    }
    st = pbw_visit_limits(desc, wire, wire_len, limits, arr_visit, &f, NULL);
    if (st == PBW_OK) {
        st = arr_finish(&f, opts, out);
    }
    arr_frame_free(&f);
    return st;
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_field, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_fieldbynumber, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, number, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_decode, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, wire, IS_STRING, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limits, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_decode_delimited, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, wire, IS_STRING, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limits, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_fromjson, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, json, IS_STRING, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ignore, _IS_BOOL, 0, "false")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limits, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_fromtext, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, text, IS_STRING, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limits, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_anypack, 0, 0, 2)
ZEND_ARG_TYPE_INFO(0, type_url, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, payload, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_frommillis, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, millis, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_nestedtype, 0, 0, 1)
ZEND_ARG_OBJ_INFO(0, field, Pbwire\\Field, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_encodearray, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, message, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_decodearray, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, wire, IS_STRING, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limits, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

PHP_METHOD(Type, __construct) {
    throw_error("use Schema::message()");
}

PHP_METHOD(Type, name) {
    ZEND_PARSE_PARAMETERS_NONE();
    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    RETURN_STRING(pbw_descriptor_name(t->desc));
}

PHP_METHOD(Type, field) {
    zend_string *name;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    {
        const char *cname = ZSTR_VAL(name);
        size_t len = ZSTR_LEN(name);
        zval *found = Z_TYPE(t->fields) == IS_ARRAY
                          ? zend_hash_str_find(Z_ARRVAL(t->fields), cname, len)
                          : NULL;
        if (found == NULL) {
            if (type_field_cached(ZEND_THIS, cname, len) == NULL) {
                char buf[512];
                snprintf(buf, sizeof buf, "%s: no such field: %s",
                         pbw_descriptor_name(t->desc), cname);
                throw_error(buf);
                RETURN_THROWS();
            }
            found = zend_hash_str_find(Z_ARRVAL(t->fields), cname, len);
            if (found == NULL) {
                throw_error("out of memory");
                RETURN_THROWS();
            }
        }
        RETURN_COPY(found);
    }
}

PHP_METHOD(Type, fieldByNumber) {
    zend_long number;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(number)
    ZEND_PARSE_PARAMETERS_END();

    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    const pbw_field *f =
        pbw_descriptor_find_field(t->desc, (uint32_t)number);
    if (f == NULL) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s: no field %ld",
                 pbw_descriptor_name(t->desc), (long)number);
        throw_error(buf);
        RETURN_THROWS();
    }
    if (field_create(return_value, ZEND_THIS, f) != 0) {
        RETURN_THROWS();
    }
}

PHP_METHOD(Type, newMessage) {
    ZEND_PARSE_PARAMETERS_NONE();
    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    /* Built trees live in their own context arena: one bump allocator instead
     * of a malloc per value, sequential layout for the encoder, and a single
     * free on close. The context is never decoded into, so the tree stays
     * mutable for its whole life. */
    pbw_ctx *ctx = pbw_ctx_new();
    pbw_message *msg;
    zval undef;
    if (ctx == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    msg = pbw_message_new_ctx(ctx, t->desc);
    if (msg == NULL) {
        pbw_ctx_free(ctx);
        throw_error("out of memory");
        RETURN_THROWS();
    }
    ZVAL_UNDEF(&undef);
    if (message_create(return_value, ZEND_THIS, &undef, msg, ctx, true) != 0) {
        pbw_ctx_free(ctx);
        RETURN_THROWS();
    }
}

PHP_METHOD(Type, decode) {
    char *wire;
    size_t wire_len;
    zval *limits_zv = NULL;
    pbw_limits lim;
    const pbw_limits *limp = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_STRING(wire, wire_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_ZVAL(limits_zv)
    ZEND_PARSE_PARAMETERS_END();

    if (parse_limits(limits_zv, &lim, &limp) != 0) {
        RETURN_THROWS();
    }
    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    pbw_message *msg = NULL;
    pbw_status st = limp == NULL ? pbw_decode(t->desc, wire, wire_len, &msg)
                                  : pbw_decode_limits(t->desc, wire, wire_len, limp, &msg);
    if (st != PBW_OK) {
        throw_error_status("decode", st);
        RETURN_THROWS();
    }
    if (msg == NULL) {
        throw_error("decode: no message returned");
        RETURN_THROWS();
    }
    zval undef;
    ZVAL_UNDEF(&undef);
    if (message_create(return_value, ZEND_THIS, &undef, msg, NULL, true) !=
        0) {
        pbw_message_free(msg);
        RETURN_THROWS();
    }
}

PHP_METHOD(Type, nestedType) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(field_zv, pbwire_field_ce)
    ZEND_PARSE_PARAMETERS_END();

    field_obj *fo = field_from(Z_OBJ_P(field_zv));
    const pbw_descriptor *sub = pbw_field_message_type(fo->field);
    if (sub == NULL) {
        RETURN_NULL();
    }
    /* The Field keeps the Schema alive; reuse that reference. */
    if (type_create(return_value, &fo->schema_zv, sub) != 0) {
        RETURN_THROWS();
    }
}

PHP_METHOD(Type, decodeDelimited) {
    char *wire;
    size_t wire_len;
    zval *limits_zv = NULL;
    pbw_limits lim;
    const pbw_limits *limp = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_STRING(wire, wire_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_ZVAL(limits_zv)
    ZEND_PARSE_PARAMETERS_END();

    if (parse_limits(limits_zv, &lim, &limp) != 0) {
        RETURN_THROWS();
    }
    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    pbw_message *msg = NULL;
    size_t consumed = 0;
    pbw_status st = limp == NULL
                        ? pbw_decode_delimited(t->desc, wire, wire_len, &msg,
                                              &consumed)
                        : pbw_decode_delimited_limits(t->desc, wire, wire_len,
                                                     limp, &msg, &consumed);
    if (st != PBW_OK) {
        throw_error_status("decodeDelimited", st);
        RETURN_THROWS();
    }
    if (msg == NULL) {
        throw_error("decodeDelimited: no message returned");
        RETURN_THROWS();
    }
    array_init(return_value);
    {
        zval m;
        zval undef;
        ZVAL_UNDEF(&undef);
        if (message_create(&m, ZEND_THIS, &undef, msg, NULL, true) != 0) {
            pbw_message_free(msg);
            zval_ptr_dtor(return_value);
            RETURN_THROWS();
        }
        add_assoc_zval(return_value, "message", &m);
        add_assoc_long(return_value, "read", (zend_long)consumed);
    }
}

PHP_METHOD(Type, fromJson) {
    char *json;
    size_t json_len;
    bool ignore = false;
    zval *limits_zv = NULL;
    pbw_limits lim;
    const pbw_limits *limp = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 3)
    Z_PARAM_STRING(json, json_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_BOOL(ignore)
    Z_PARAM_ZVAL(limits_zv)
    ZEND_PARSE_PARAMETERS_END();

    if (parse_limits(limits_zv, &lim, &limp) != 0) {
        RETURN_THROWS();
    }
    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    pbw_message *msg = NULL;
    pbw_json_parse_options opts;
    pbw_status st;
    opts.ignore_unknown_fields = ignore ? 1 : 0;
    if (limp == NULL) {
        st = pbw_json_parse_opts(t->desc, json, json_len, &opts, &msg);
    } else {
        st = pbw_json_parse_limits(t->desc, json, json_len, limp, &opts,
                                    &msg);
    }
    if (st != PBW_OK) {
        throw_error_status("fromJson", st);
        RETURN_THROWS();
    }
    if (msg == NULL) {
        throw_error("fromJson: no message returned");
        RETURN_THROWS();
    }
    zval undef;
    ZVAL_UNDEF(&undef);
    if (message_create(return_value, ZEND_THIS, &undef, msg, NULL, true) !=
        0) {
        pbw_message_free(msg);
        RETURN_THROWS();
    }
}

PHP_METHOD(Type, fromText) {
    char *text;
    size_t text_len;
    zval *limits_zv = NULL;
    pbw_limits lim;
    const pbw_limits *limp = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_STRING(text, text_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_ZVAL(limits_zv)
    ZEND_PARSE_PARAMETERS_END();

    if (parse_limits(limits_zv, &lim, &limp) != 0) {
        RETURN_THROWS();
    }
    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    pbw_message *msg = NULL;
    pbw_status st = limp == NULL
                        ? pbw_text_parse(t->desc, text, text_len, &msg)
                        : pbw_text_parse_limits(t->desc, text, text_len, limp,
                                               &msg);
    if (st != PBW_OK) {
        throw_error_status("fromText", st);
        RETURN_THROWS();
    }
    if (msg == NULL) {
        throw_error("fromText: no message returned");
        RETURN_THROWS();
    }
    zval undef;
    ZVAL_UNDEF(&undef);
    if (message_create(return_value, ZEND_THIS, &undef, msg, NULL, true) !=
        0) {
        pbw_message_free(msg);
        RETURN_THROWS();
    }
}

PHP_METHOD(Type, anyPack) {
    char *url;
    size_t url_len;
    char *payload;
    size_t payload_len;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_STRING(url, url_len)
    Z_PARAM_STRING(payload, payload_len)
    ZEND_PARSE_PARAMETERS_END();

    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    pbw_message *msg = NULL;
    pbw_status st = pbw_any_pack(t->desc, url, payload, payload_len, &msg);
    if (st != PBW_OK) {
        throw_error_status("anyPack", st);
        RETURN_THROWS();
    }
    if (msg == NULL) {
        throw_error("anyPack: no message returned");
        RETURN_THROWS();
    }
    zval undef;
    ZVAL_UNDEF(&undef);
    if (message_create(return_value, ZEND_THIS, &undef, msg, NULL, true) !=
        0) {
        pbw_message_free(msg);
        RETURN_THROWS();
    }
}

PHP_METHOD(Type, timestampFromMillis) {
    zend_long ms;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(ms)
    ZEND_PARSE_PARAMETERS_END();

    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    pbw_message *msg = NULL;
    pbw_status st =
        pbw_timestamp_from_millis(t->desc, (int64_t)ms, &msg);
    if (st != PBW_OK) {
        throw_error_status("timestampFromMillis", st);
        RETURN_THROWS();
    }
    if (msg == NULL) {
        throw_error("timestampFromMillis: no message returned");
        RETURN_THROWS();
    }
    zval undef;
    ZVAL_UNDEF(&undef);
    if (message_create(return_value, ZEND_THIS, &undef, msg, NULL, true) !=
        0) {
        pbw_message_free(msg);
        RETURN_THROWS();
    }
}

PHP_METHOD(Type, durationFromMillis) {
    zend_long ms;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(ms)
    ZEND_PARSE_PARAMETERS_END();

    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    pbw_message *msg = NULL;
    pbw_status st =
        pbw_duration_from_millis(t->desc, (int64_t)ms, &msg);
    if (st != PBW_OK) {
        throw_error_status("durationFromMillis", st);
        RETURN_THROWS();
    }
    if (msg == NULL) {
        throw_error("durationFromMillis: no message returned");
        RETURN_THROWS();
    }
    zval undef;
    ZVAL_UNDEF(&undef);
    if (message_create(return_value, ZEND_THIS, &undef, msg, NULL, true) !=
        0) {
        pbw_message_free(msg);
        RETURN_THROWS();
    }
}

PHP_METHOD(Type, isMessageField) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(field_zv, pbwire_field_ce)
    ZEND_PARSE_PARAMETERS_END();

    field_obj *fo = field_from(Z_OBJ_P(field_zv));
    pbw_type ty = pbw_field_type(fo->field);
    RETURN_BOOL(ty == PBW_TYPE_MESSAGE || ty == PBW_TYPE_GROUP);
}

/* One extension entry: the PHP array is walked in C and the tree filled in C. */
PHP_METHOD(Type, encodeArray) {
    zval *message_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ARRAY(message_zv)
    ZEND_PARSE_PARAMETERS_END();

    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    pbw_ctx *ctx = type_array_ctx(t);
    pbw_message *msg;
    const uint8_t *out = NULL;
    size_t len = 0;
    pbw_status st;
    zend_string *str;

    if (ctx == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    pbw_ctx_reset(ctx);
    msg = pbw_message_new_ctx(ctx, t->desc);
    if (msg == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    if (array_encode_message(message_zv, ctx, t->desc, msg, 0) != 0) {
        RETURN_THROWS();
    }
    st = pbw_encode_ctx(ctx, msg, &out, &len);
    if (st != PBW_OK) {
        throw_error_status("encodeArray", st);
        RETURN_THROWS();
    }
    str = zend_string_init((const char *)out, len, 0);
    RETURN_STR(str);
}

/* The reverse entry: wire bytes in, the fully materialized PHP array out.
 * One C-side tag scan per message level builds the array straight from the
 * wire (see arr_decode above): no tree, no per-field API in the timer. */
PHP_METHOD(Type, decodeArray) {
    char *wire;
    size_t wire_len;
    zval *limits_zv = NULL;
    pbw_limits lim;
    const pbw_limits *limp = NULL;
    type_obj *t;
    arr_opts opts;
    pbw_status st;
    zval out;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_STRING(wire, wire_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_ZVAL(limits_zv)
    ZEND_PARSE_PARAMETERS_END();

    if (parse_limits(limits_zv, &lim, &limp) != 0) {
        RETURN_THROWS();
    }
    t = type_from(Z_OBJ_P(ZEND_THIS));
    opts.user_dlim = limp == NULL || lim.max_depth == 0 ||
                             lim.max_depth > PBW_MAX_DEPTH
                         ? PBW_MAX_DEPTH
                         : lim.max_depth;
    opts.depth_cap = opts.user_dlim < PBWIRE_ARRAY_MAX_DEPTH
                         ? opts.user_dlim
                         : PBWIRE_ARRAY_MAX_DEPTH;
    opts.nesting = 0;
    opts.layouts = NULL;
    opts.nlayouts = opts.layouts_cap = 0;
    ZVAL_UNDEF(&out);
    st = arr_decode(t->desc, (const uint8_t *)wire, wire_len, limp, 0, &opts,
                     &out);
    arr_opts_free(&opts);
    if (st != PBW_OK) {
        if (Z_TYPE(out) != IS_UNDEF) {
            zval_ptr_dtor(&out);
        }
        if (opts.nesting) {
            throw_error("nesting limit exceeded");
        } else {
            throw_error_status("decodeArray", st);
        }
        RETURN_THROWS();
    }
    RETURN_ZVAL(&out, 1, 1);
}

/* ---------------------------------------------------------------- Field */

static zend_object *field_create_obj(zend_class_entry *ce) {
    field_obj *o =
        ecalloc(1, sizeof(field_obj) + zend_object_properties_size(ce));
    zend_object_std_init(&o->std, ce);
    object_properties_init(&o->std, ce);
    o->std.handlers = &field_handlers;
    ZVAL_UNDEF(&o->schema_zv);
    o->field = NULL;
    return &o->std;
}

static void field_free(zend_object *o) {
    field_obj *f = field_from(o);
    if (Z_TYPE(f->schema_zv) != IS_UNDEF) {
        schema_obj *s = schema_from(Z_OBJ(f->schema_zv));

        holder_release(s->holder);
        zval_ptr_dtor(&f->schema_zv);
    }
    zend_object_std_dtor(&f->std);
}

/* A Field keeps the Schema (and one holder reference), not the Type: the Type
 * holds its field cache, so keeping the Type would form a reference cycle the
 * Zend collector has to chase. */
static int field_create(zval *out, zval *schema_zv, const pbw_field *field) {
    object_init_ex(out, pbwire_field_ce);
    field_obj *f = field_from(Z_OBJ_P(out));
    ZVAL_COPY(&f->schema_zv, schema_zv);
    schema_from(Z_OBJ_P(schema_zv))->holder->refs++;
    f->field = field;
    return 0;
}

PHP_METHOD(Field, __construct) {
    throw_error("use Type::field()");
}

PHP_METHOD(Field, name) {
    ZEND_PARSE_PARAMETERS_NONE();
    field_obj *f = field_from(Z_OBJ_P(ZEND_THIS));
    RETURN_STRING(pbw_field_name(f->field));
}

PHP_METHOD(Field, number) {
    ZEND_PARSE_PARAMETERS_NONE();
    field_obj *f = field_from(Z_OBJ_P(ZEND_THIS));
    RETURN_LONG((zend_long)pbw_field_number(f->field));
}

PHP_METHOD(Field, type) {
    ZEND_PARSE_PARAMETERS_NONE();
    field_obj *f = field_from(Z_OBJ_P(ZEND_THIS));
    RETURN_LONG((zend_long)pbw_field_type(f->field));
}

PHP_METHOD(Field, label) {
    ZEND_PARSE_PARAMETERS_NONE();
    field_obj *f = field_from(Z_OBJ_P(ZEND_THIS));
    RETURN_LONG((zend_long)pbw_field_label(f->field));
}

/* ---------------------------------------------------------------- Message */

static zend_object *message_create_obj(zend_class_entry *ce) {
    message_obj *o =
        ecalloc(1, sizeof(message_obj) + zend_object_properties_size(ce));
    zend_object_std_init(&o->std, ce);
    object_properties_init(&o->std, ce);
    o->std.handlers = &message_handlers;
    ZVAL_UNDEF(&o->type_zv);
    ZVAL_UNDEF(&o->parent_zv);
    o->msg = NULL;
    o->ctx = NULL;
    o->arena = NULL;
    o->owned = false;
    o->closed = false;
    o->encode_hint = 0;
    return &o->std;
}

static void message_free(zend_object *o) {
    message_obj *m = message_from(o);
    if (m->ctx != NULL) {
        /* Context-built root: the tree lives in the context's arena, so
         * freeing the context releases the whole tree at once. */
        pbw_ctx_free(m->ctx);
        m->ctx = NULL;
        m->msg = NULL;
    } else if (m->msg != NULL && m->owned && !m->closed) {
        pbw_message_free(m->msg);
        m->msg = NULL;
    }
    if (Z_TYPE(m->type_zv) != IS_UNDEF) {
        zval_ptr_dtor(&m->type_zv);
    }
    if (Z_TYPE(m->parent_zv) != IS_UNDEF) {
        zval_ptr_dtor(&m->parent_zv);
    }
    zend_object_std_dtor(&m->std);
}

static int message_create(zval *out, zval *type_zv, zval *parent_zv,
                          pbw_message *msg, pbw_ctx *ctx, bool owned) {
    object_init_ex(out, pbwire_message_ce);
    message_obj *m = message_from(Z_OBJ_P(out));
    ZVAL_COPY(&m->type_zv, type_zv);
    if (parent_zv != NULL && Z_TYPE_P(parent_zv) != IS_UNDEF) {
        ZVAL_COPY(&m->parent_zv, parent_zv);
    } else {
        ZVAL_UNDEF(&m->parent_zv);
    }
    m->msg = msg;
    m->ctx = ctx;
    /* A nested object never owns the context, but its setters still have to
     * build values in the same arena as the root: inherit it from the parent
     * (every nested object is created from its parent, so the chain holds).
     * An inherited arena is borrowed, never freed here. */
    m->arena = ctx;
    if (m->arena == NULL && parent_zv != NULL &&
        Z_TYPE_P(parent_zv) == IS_OBJECT &&
        instanceof_function(Z_OBJCE_P(parent_zv), pbwire_message_ce)) {
        m->arena = message_from(Z_OBJ_P(parent_zv))->arena;
    }
    m->owned = owned;
    m->closed = false;
    m->encode_hint = 0;
    return 0;
}

static message_obj *check_message(zval *self) {
    message_obj *m = message_from(Z_OBJ_P(self));
    zval *parent;

    if (m->msg == NULL || m->closed) {
        throw_error("message is closed");
        return NULL;
    }
    /* A nested view lives inside its root's arena: closing the root (or any
     * ancestor) frees the memory the child points into, so a child call must
     * refuse once an ancestor is closed. */
    parent = &m->parent_zv;
    while (Z_TYPE_P(parent) == IS_OBJECT) {
        message_obj *owner = message_from(Z_OBJ_P(parent));

        if (owner->msg == NULL || owner->closed) {
            throw_error("parent message is closed");
            return NULL;
        }
        parent = &owner->parent_zv;
    }
    return m;
}

/* Value constructors that build into the message's context arena when it has
 * one (context-built roots), or fall back to the plain heap constructors for
 * decoded trees. Bit patterns match the plain constructors exactly. */
static pbw_value *new_uint(message_obj *m, uint64_t bits) {
    if (m->arena != NULL) {
        return pbw_value_new_uint_ctx(m->arena, bits);
    }
    return pbw_value_new_uint(bits);
}

static pbw_value *new_bytes(message_obj *m, const char *data, size_t len) {
    if (m->arena != NULL) {
        return pbw_value_new_bytes_ctx(m->arena, data, len);
    }
    return pbw_value_new_bytes(data, len);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_message_field, 0, 0, 1)
ZEND_ARG_INFO(0, field)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_message_int, 0, 0, 2)
ZEND_ARG_INFO(0, field)
ZEND_ARG_TYPE_INFO(0, value, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_message_bool, 0, 0, 2)
ZEND_ARG_INFO(0, field)
ZEND_ARG_TYPE_INFO(0, value, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_message_float, 0, 0, 2)
ZEND_ARG_INFO(0, field)
ZEND_ARG_TYPE_INFO(0, value, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_message_string, 0, 0, 2)
ZEND_ARG_INFO(0, field)
ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_message_at, 0, 0, 2)
ZEND_ARG_INFO(0, field)
ZEND_ARG_TYPE_INFO(0, index, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_message_encode_to, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, cap, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_message_totext, 0, 0, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, printUnknown, _IS_BOOL, 0, "true")
ZEND_END_ARG_INFO()

PHP_METHOD(Message, __construct) {
    throw_error("use Type::newMessage() or Type::decode()");
}

PHP_METHOD(Message, close) {
    ZEND_PARSE_PARAMETERS_NONE();
    message_obj *m = message_from(Z_OBJ_P(ZEND_THIS));
    if (m->ctx != NULL) {
        pbw_ctx_free(m->ctx);
        m->ctx = NULL;
        m->msg = NULL;
    } else if (m->msg != NULL && m->owned && !m->closed) {
        pbw_message_free(m->msg);
        m->msg = NULL;
    }
    m->closed = true;
}

PHP_METHOD(Message, encode) {
    ZEND_PARSE_PARAMETERS_NONE();
    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    /* Fast path: the last encoded size is the next buffer hint, so a repeated
     * encode is one tree walk straight into the result string: no core malloc,
     * no second copy, no free (same trick as the Ruby binding). A message that
     * grew past the hint retries on the exact-size path below. */
    if (m->encode_hint != 0) {
        size_t cap = m->encode_hint + PBW_ENCODE_SLACK;
        zend_string *str = zend_string_alloc(cap, 0);
        size_t len = 0;
        pbw_status fast = pbw_encode_to(m->msg, ZSTR_VAL(str), cap, &len);
        if (fast == PBW_OK) {
            ZSTR_LEN(str) = len;
            ZSTR_VAL(str)[len] = '\0'; /* zend_string invariant */
            m->encode_hint = len;
            RETURN_STR(str);
        }
        zend_string_release(str);
        if (fast != PBW_ERR_LIMIT) {
            throw_error_status("encode", fast);
            RETURN_THROWS();
        }
    }
    size_t size = 0;
    pbw_status st = pbw_message_encoded_size(m->msg, &size);
    if (st != PBW_OK) {
        throw_error_status("encode", st);
        RETURN_THROWS();
    }
    zend_string *str = zend_string_alloc(size + PBW_ENCODE_SLACK, 0);
    size_t len = 0;
    st = pbw_encode_to(m->msg, ZSTR_VAL(str),
                        size + PBW_ENCODE_SLACK, &len);
    if (st != PBW_OK) {
        zend_string_release(str);
        throw_error_status("encode", st);
        RETURN_THROWS();
    }
    ZSTR_LEN(str) = len;
    ZSTR_VAL(str)[len] = '\0'; /* zend_string invariant */
    m->encode_hint = len;
    RETURN_STR(str);
}

PHP_METHOD(Message, encodeDeterministic) {
    ZEND_PARSE_PARAMETERS_NONE();
    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    uint8_t *out = NULL;
    size_t len = 0;
    pbw_status st = pbw_encode_deterministic(m->msg, &out, &len);
    if (st != PBW_OK) {
        throw_error_status("encodeDeterministic", st);
        RETURN_THROWS();
    }
    zend_string *str = zend_string_init((const char *)out, len, 0);
    pbw_free(out);
    if (str == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    RETURN_STR(str);
}

PHP_METHOD(Message, encodedSize) {
    ZEND_PARSE_PARAMETERS_NONE();
    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    size_t len = 0;
    pbw_status st = pbw_message_encoded_size(m->msg, &len);
    if (st != PBW_OK) {
        throw_error_status("encodedSize", st);
        RETURN_THROWS();
    }
    RETURN_LONG((zend_long)len);
}

PHP_METHOD(Message, encodeTo) {
    zend_long cap;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(cap)
    ZEND_PARSE_PARAMETERS_END();

    if (cap < 1) {
        throw_error("encodeTo: cap must be positive");
        RETURN_THROWS();
    }
    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    zend_string *str = zend_string_alloc((size_t)cap, 0);
    size_t len = 0;
    pbw_status st =
        pbw_encode_to(m->msg, ZSTR_VAL(str), (size_t)cap, &len);
    if (st != PBW_OK) {
        zend_string_release(str);
        throw_error_status("encodeTo", st);
        RETURN_THROWS();
    }
    ZSTR_LEN(str) = len;
    ZSTR_VAL(str)[len] = '\0'; /* zend_string invariant */
    RETURN_STR(str);
}

PHP_METHOD(Message, encodeDelimited) {
    ZEND_PARSE_PARAMETERS_NONE();
    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    uint8_t *out = NULL;
    size_t len = 0;
    pbw_status st = pbw_encode_delimited(m->msg, &out, &len);
    if (st != PBW_OK) {
        throw_error_status("encodeDelimited", st);
        RETURN_THROWS();
    }
    zend_string *str = zend_string_init((const char *)out, len, 0);
    pbw_free(out);
    if (str == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    RETURN_STR(str);
}

PHP_METHOD(Message, encodeDelimitedTo) {
    zend_long cap;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(cap)
    ZEND_PARSE_PARAMETERS_END();

    if (cap < 1) {
        throw_error("encodeDelimitedTo: cap must be positive");
        RETURN_THROWS();
    }
    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    zend_string *str = zend_string_alloc((size_t)cap, 0);
    size_t len = 0;
    pbw_status st =
        pbw_encode_delimited_to(m->msg, ZSTR_VAL(str), (size_t)cap, &len);
    if (st != PBW_OK) {
        zend_string_release(str);
        throw_error_status("encodeDelimitedTo", st);
        RETURN_THROWS();
    }
    ZSTR_LEN(str) = len;
    RETURN_STR(str);
}

PHP_METHOD(Message, toJson) {
    ZEND_PARSE_PARAMETERS_NONE();
    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    char *out = NULL;
    size_t len = 0;
    pbw_status st = pbw_json_print(m->msg, &out, &len);
    if (st != PBW_OK) {
        throw_error_status("toJson", st);
        RETURN_THROWS();
    }
    zend_string *str = zend_string_init(out, len, 0);
    pbw_free(out);
    if (str == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    RETURN_STR(str);
}

PHP_METHOD(Message, toText) {
    bool print_unknown = true;
    ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_BOOL(print_unknown)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    char *out = NULL;
    size_t len = 0;
    pbw_status st =
        pbw_text_print_opts(m->msg, print_unknown ? 1 : 0, &out, &len);
    if (st != PBW_OK) {
        throw_error_status("toText", st);
        RETURN_THROWS();
    }
    zend_string *str = zend_string_init(out, len, 0);
    pbw_free(out);
    if (str == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    RETURN_STR(str);
}

PHP_METHOD(Message, anyTypeUrl) {
    ZEND_PARSE_PARAMETERS_NONE();
    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const char *url = pbw_any_type_url(m->msg);
    if (url == NULL) {
        RETURN_NULL();
    }
    RETURN_STRING(url);
}

PHP_METHOD(Message, anyPayload) {
    ZEND_PARSE_PARAMETERS_NONE();
    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    size_t len = 0;
    const void *payload = pbw_any_payload(m->msg, &len);
    if (payload == NULL) {
        RETURN_NULL();
    }
    RETURN_STRINGL((const char *)payload, len);
}

PHP_METHOD(Message, timestampMillis) {
    ZEND_PARSE_PARAMETERS_NONE();
    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    int64_t ms = 0;
    pbw_status st = pbw_timestamp_to_millis(m->msg, &ms);
    if (st != PBW_OK) {
        throw_error_status("timestampMillis", st);
        RETURN_THROWS();
    }
    RETURN_LONG((zend_long)ms);
}

PHP_METHOD(Message, durationMillis) {
    ZEND_PARSE_PARAMETERS_NONE();
    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    int64_t ms = 0;
    pbw_status st = pbw_duration_to_millis(m->msg, &ms);
    if (st != PBW_OK) {
        throw_error_status("durationMillis", st);
        RETURN_THROWS();
    }
    RETURN_LONG((zend_long)ms);
}

PHP_METHOD(Message, fieldmaskPaths) {
    ZEND_PARSE_PARAMETERS_NONE();
    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    size_t count = pbw_fieldmask_path_count(m->msg);
    array_init_size(return_value, count);
    for (size_t i = 0; i < count; i++) {
        const char *path = pbw_fieldmask_path_at(m->msg, i);
        if (path == NULL) {
            throw_error("fieldmaskPaths: no path returned");
            RETURN_THROWS();
        }
        add_next_index_string(return_value, path);
    }
}

PHP_METHOD(Message, setInt) {
    zval *field_zv;
    zend_long v;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_ZVAL(field_zv)
    Z_PARAM_LONG(v)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_value *value = new_uint(m, (uint64_t)v);
    if (value == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    pbw_status st = pbw_message_set_field(m->msg, f, value);
    if (st != PBW_OK) {
        pbw_value_free(value);
        throw_error_status("set", st);
        RETURN_THROWS();
    }
    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Message, setBool) {
    zval *field_zv;
    bool v;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_ZVAL(field_zv)
    Z_PARAM_BOOL(v)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_value *value = new_uint(m, v ? 1u : 0u);
    if (value == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    pbw_status st = pbw_message_set_field(m->msg, f, value);
    if (st != PBW_OK) {
        pbw_value_free(value);
        throw_error_status("set", st);
        RETURN_THROWS();
    }
    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Message, setFloat) {
    zval *field_zv;
    double v;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_ZVAL(field_zv)
    Z_PARAM_DOUBLE(v)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    float fv = (float)v;
    uint32_t bits;
    memcpy(&bits, &fv, sizeof(bits));
    pbw_value *value = new_uint(m, bits);
    if (value == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    pbw_status st = pbw_message_set_field(m->msg, f, value);
    if (st != PBW_OK) {
        pbw_value_free(value);
        throw_error_status("set", st);
        RETURN_THROWS();
    }
    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Message, setDouble) {
    zval *field_zv;
    double v;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_ZVAL(field_zv)
    Z_PARAM_DOUBLE(v)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    pbw_value *value = new_uint(m, bits);
    if (value == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    pbw_status st = pbw_message_set_field(m->msg, f, value);
    if (st != PBW_OK) {
        pbw_value_free(value);
        throw_error_status("set", st);
        RETURN_THROWS();
    }
    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Message, setString) {
    zval *field_zv;
    char *v;
    size_t v_len;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_ZVAL(field_zv)
    Z_PARAM_STRING(v, v_len)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    /* Explicit length: embedded NUL bytes survive, unlike the old
     * NUL-terminated C string crossing. */
    pbw_value *value = new_bytes(m, v, v_len);
    if (value == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    pbw_status st = pbw_message_set_field(m->msg, f, value);
    if (st != PBW_OK) {
        pbw_value_free(value);
        throw_error_status("set", st);
        RETURN_THROWS();
    }
    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Message, setBytes) {
    zval *field_zv;
    char *v;
    size_t v_len;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_ZVAL(field_zv)
    Z_PARAM_STRING(v, v_len)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_value *value = new_bytes(m, v, v_len);
    if (value == NULL) {
        throw_error("out of memory");
        RETURN_THROWS();
    }
    pbw_status st = pbw_message_set_field(m->msg, f, value);
    if (st != PBW_OK) {
        pbw_value_free(value);
        throw_error_status("set", st);
        RETURN_THROWS();
    }
    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Message, setMessage) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(field_zv)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_message *child =
        pbw_message_set_message(m->msg, pbw_field_number(f));
    if (child == NULL) {
        char buf[256];
        snprintf(buf, sizeof buf, "set %s: not a message field",
                 pbw_field_name(f));
        throw_error(buf);
        RETURN_THROWS();
    }
    const pbw_descriptor *sub = pbw_field_message_type(f);
    if (sub == NULL) {
        char buf[256];
        snprintf(buf, sizeof buf, "set %s: no nested type",
                 pbw_field_name(f));
        throw_error(buf);
        RETURN_THROWS();
    }
    zval subtype;
    if (type_create(&subtype, &type_from(Z_OBJ(m->type_zv))->schema_zv, sub) != 0) {
        RETURN_THROWS();
    }
    int rc = message_create(return_value, &subtype, ZEND_THIS, child, NULL, false);
    zval_ptr_dtor(&subtype);
    if (rc != 0) {
        RETURN_THROWS();
    }
}

PHP_METHOD(Message, clear) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(field_zv)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_message_clear(m->msg, pbw_field_number(f));
}

PHP_METHOD(Message, has) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(field_zv)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    RETURN_BOOL(pbw_message_get_field(m->msg, f) != NULL);
}

PHP_METHOD(Message, getInt) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(field_zv)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_value *v = pbw_message_get_field(m->msg, f);
    if (v == NULL) {
        RETURN_LONG(0);
    }
    RETURN_LONG((zend_long)pbw_value_get_int(v));
}

PHP_METHOD(Message, getBool) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(field_zv)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_value *v = pbw_message_get_field(m->msg, f);
    RETURN_BOOL(v != NULL && pbw_value_get_bool(v) != 0);
}

PHP_METHOD(Message, getFloat) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(field_zv)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_value *v = pbw_message_get_field(m->msg, f);
    if (v == NULL) {
        RETURN_DOUBLE(0.0);
    }
    RETURN_DOUBLE((double)pbw_value_get_float(v));
}

PHP_METHOD(Message, getDouble) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(field_zv)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_value *v = pbw_message_get_field(m->msg, f);
    if (v == NULL) {
        RETURN_DOUBLE(0.0);
    }
    RETURN_DOUBLE(pbw_value_get_double(v));
}

PHP_METHOD(Message, getString) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(field_zv)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_value *v = pbw_message_get_field(m->msg, f);
    if (v == NULL) {
        RETURN_NULL();
    }
    /* Length-based: values with embedded NUL bytes round-trip intact. */
    size_t slen = 0;
    const void *sp = pbw_value_get_bytes(v, &slen);
    if (sp == NULL) {
        RETURN_NULL();
    }
    RETURN_STRINGL((const char *)sp, slen);
}

PHP_METHOD(Message, getBytes) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(field_zv)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_value *v = pbw_message_get_field(m->msg, f);
    if (v == NULL) {
        RETURN_NULL();
    }
    size_t len = 0;
    const void *p = pbw_value_get_bytes(v, &len);
    if (p == NULL) {
        RETURN_NULL();
    }
    RETURN_STRINGL((const char *)p, len);
}

PHP_METHOD(Message, getMessage) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(field_zv)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_value *v = pbw_message_get_field(m->msg, f);
    if (v == NULL) {
        RETURN_NULL();
    }
    pbw_message *sub = pbw_value_get_message(v);
    const pbw_descriptor *subdesc = pbw_field_message_type(f);
    if (sub == NULL || subdesc == NULL) {
        RETURN_NULL();
    }
    zval subtype;
    if (type_create(&subtype, &type_from(Z_OBJ(m->type_zv))->schema_zv, subdesc) != 0) {
        RETURN_THROWS();
    }
    int rc = message_create(return_value, &subtype, ZEND_THIS, sub, NULL, false);
    zval_ptr_dtor(&subtype);
    if (rc != 0) {
        RETURN_THROWS();
    }
}

PHP_METHOD(Message, count) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(field_zv)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_value *v = pbw_message_get_field(m->msg, f);
    if (v == NULL) {
        RETURN_LONG(0);
    }
    RETURN_LONG((zend_long)pbw_value_count(v));
}

/* Value constructors shared with Message::at. */
static zend_object *value_create_obj(zend_class_entry *ce) {
    value_obj *o =
        ecalloc(1, sizeof(value_obj) + zend_object_properties_size(ce));
    zend_object_std_init(&o->std, ce);
    object_properties_init(&o->std, ce);
    o->std.handlers = &value_handlers;
    ZVAL_UNDEF(&o->owner_zv);
    ZVAL_UNDEF(&o->type_zv);
    o->val = NULL;
    return &o->std;
}

static void value_free(zend_object *o) {
    value_obj *v = value_from(o);
    if (Z_TYPE(v->owner_zv) != IS_UNDEF) {
        zval_ptr_dtor(&v->owner_zv);
    }
    if (Z_TYPE(v->type_zv) != IS_UNDEF) {
        zval_ptr_dtor(&v->type_zv);
    }
    zend_object_std_dtor(&v->std);
}

static int value_create(zval *out, zval *owner_zv, zval *type_zv,
                        pbw_value *val) {
    object_init_ex(out, pbwire_value_ce);
    value_obj *v = value_from(Z_OBJ_P(out));
    ZVAL_COPY(&v->owner_zv, owner_zv);
    if (type_zv != NULL && Z_TYPE_P(type_zv) != IS_UNDEF) {
        ZVAL_COPY(&v->type_zv, type_zv);
    } else {
        ZVAL_UNDEF(&v->type_zv);
    }
    v->val = val;
    return 0;
}

PHP_METHOD(Message, at) {
    zval *field_zv;
    zend_long index;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_ZVAL(field_zv)
    Z_PARAM_LONG(index)
    ZEND_PARSE_PARAMETERS_END();

    message_obj *m = check_message(ZEND_THIS);
    if (m == NULL) {
        RETURN_THROWS();
    }
    const pbw_field *f = resolve_field(&m->type_zv, field_zv);
    if (f == NULL) {
        RETURN_THROWS();
    }
    pbw_value *v = pbw_message_get_field(m->msg, f);
    if (v == NULL) {
        RETURN_NULL();
    }
    pbw_value *item = pbw_value_at(v, (size_t)index);
    if (item == NULL) {
        RETURN_NULL();
    }
    const pbw_descriptor *sub = pbw_field_message_type(f);
    if (sub != NULL) {
        zval subtype;
        if (type_create(&subtype, &type_from(Z_OBJ(m->type_zv))->schema_zv, sub) != 0) {
            RETURN_THROWS();
        }
        int rc = value_create(return_value, ZEND_THIS, &subtype, item);
        zval_ptr_dtor(&subtype);
        if (rc != 0) {
            RETURN_THROWS();
        }
    } else {
        zval undef;
        ZVAL_UNDEF(&undef);
        if (value_create(return_value, ZEND_THIS, &undef, item) != 0) {
            RETURN_THROWS();
        }
    }
}

/* ---------------------------------------------------------------- Value */

PHP_METHOD(Value, __construct) {
    throw_error("use Message::at()");
}

/* A Value points into its owner message's arena: once that message is closed
 * the bytes are gone, so every getter goes through this check. */
static int value_live(value_obj *v) {
    return check_message(&v->owner_zv) != NULL;
}

PHP_METHOD(Value, asInt) {
    ZEND_PARSE_PARAMETERS_NONE();
    value_obj *v = value_from(Z_OBJ_P(ZEND_THIS));
    if (!value_live(v)) {
        RETURN_THROWS();
    }
    RETURN_LONG((zend_long)pbw_value_get_int(v->val));
}

PHP_METHOD(Value, asBool) {
    ZEND_PARSE_PARAMETERS_NONE();
    value_obj *v = value_from(Z_OBJ_P(ZEND_THIS));
    if (!value_live(v)) {
        RETURN_THROWS();
    }
    RETURN_BOOL(pbw_value_get_bool(v->val) != 0);
}

PHP_METHOD(Value, asFloat) {
    ZEND_PARSE_PARAMETERS_NONE();
    value_obj *v = value_from(Z_OBJ_P(ZEND_THIS));
    if (!value_live(v)) {
        RETURN_THROWS();
    }
    RETURN_DOUBLE((double)pbw_value_get_float(v->val));
}

PHP_METHOD(Value, asDouble) {
    ZEND_PARSE_PARAMETERS_NONE();
    value_obj *v = value_from(Z_OBJ_P(ZEND_THIS));
    if (!value_live(v)) {
        RETURN_THROWS();
    }
    RETURN_DOUBLE(pbw_value_get_double(v->val));
}

PHP_METHOD(Value, asString) {
    ZEND_PARSE_PARAMETERS_NONE();
    value_obj *v = value_from(Z_OBJ_P(ZEND_THIS));
    if (!value_live(v)) {
        RETURN_THROWS();
    }
    size_t len = 0;
    const void *p = pbw_value_get_bytes(v->val, &len);
    if (p == NULL) {
        RETURN_NULL();
    }
    RETURN_STRINGL((const char *)p, len);
}

PHP_METHOD(Value, asBytes) {
    ZEND_PARSE_PARAMETERS_NONE();
    value_obj *v = value_from(Z_OBJ_P(ZEND_THIS));
    if (!value_live(v)) {
        RETURN_THROWS();
    }
    size_t len = 0;
    const void *p = pbw_value_get_bytes(v->val, &len);
    if (p == NULL) {
        RETURN_NULL();
    }
    RETURN_STRINGL((const char *)p, len);
}

PHP_METHOD(Value, asMessage) {
    ZEND_PARSE_PARAMETERS_NONE();
    value_obj *v = value_from(Z_OBJ_P(ZEND_THIS));
    if (!value_live(v)) {
        RETURN_THROWS();
    }
    if (Z_TYPE(v->type_zv) == IS_UNDEF) {
        RETURN_NULL();
    }
    pbw_message *sub = pbw_value_get_message(v->val);
    if (sub == NULL) {
        RETURN_NULL();
    }
    if (message_create(return_value, &v->type_zv, &v->owner_zv, sub, NULL,
                       false) != 0) {
        RETURN_THROWS();
    }
}

/* ---------------------------------------------------------------- tables */

static const zend_function_entry schema_methods[] = {
    PHP_ME(Schema, __construct, arginfo_noargs,
           ZEND_ACC_PRIVATE | ZEND_ACC_CTOR)
    PHP_ME(Schema, fromFile, arginfo_schema_fromfile,
           ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Schema, fromBytes, arginfo_schema_frombytes,
           ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Schema, message, arginfo_schema_message, ZEND_ACC_PUBLIC)
    PHP_ME(Schema, services, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Schema, service, arginfo_schema_service, ZEND_ACC_PUBLIC)
    PHP_ME(Schema, resolve, arginfo_schema_resolve, ZEND_ACC_PUBLIC)
    PHP_ME(Schema, close, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_FE_END,
};

static const zend_function_entry type_methods[] = {
    PHP_ME(Type, __construct, arginfo_noargs,
           ZEND_ACC_PRIVATE | ZEND_ACC_CTOR)
    PHP_ME(Type, name, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Type, field, arginfo_type_field, ZEND_ACC_PUBLIC)
    PHP_ME(Type, fieldByNumber, arginfo_type_fieldbynumber, ZEND_ACC_PUBLIC)
    PHP_ME(Type, newMessage, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Type, decode, arginfo_type_decode, ZEND_ACC_PUBLIC)
    PHP_ME(Type, decodeDelimited, arginfo_type_decode_delimited, ZEND_ACC_PUBLIC)
    PHP_ME(Type, fromJson, arginfo_type_fromjson, ZEND_ACC_PUBLIC)
    PHP_ME(Type, fromText, arginfo_type_fromtext, ZEND_ACC_PUBLIC)
    PHP_ME(Type, anyPack, arginfo_type_anypack, ZEND_ACC_PUBLIC)
    PHP_ME(Type, timestampFromMillis, arginfo_type_frommillis, ZEND_ACC_PUBLIC)
    PHP_ME(Type, durationFromMillis, arginfo_type_frommillis, ZEND_ACC_PUBLIC)
    PHP_ME(Type, nestedType, arginfo_type_nestedtype, ZEND_ACC_PUBLIC)
    PHP_ME(Type, isMessageField, arginfo_type_nestedtype, ZEND_ACC_PUBLIC)
    PHP_ME(Type, encodeArray, arginfo_type_encodearray, ZEND_ACC_PUBLIC)
    PHP_ME(Type, decodeArray, arginfo_type_decodearray, ZEND_ACC_PUBLIC)
    PHP_FE_END,
};

static const zend_function_entry field_methods[] = {
    PHP_ME(Field, __construct, arginfo_noargs,
           ZEND_ACC_PRIVATE | ZEND_ACC_CTOR)
    PHP_ME(Field, name, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Field, number, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Field, type, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Field, label, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_FE_END,
};

static const zend_function_entry message_methods[] = {
    PHP_ME(Message, __construct, arginfo_noargs,
           ZEND_ACC_PRIVATE | ZEND_ACC_CTOR)
    PHP_ME(Message, setInt, arginfo_message_int, ZEND_ACC_PUBLIC)
    PHP_ME(Message, setBool, arginfo_message_bool, ZEND_ACC_PUBLIC)
    PHP_ME(Message, setFloat, arginfo_message_float, ZEND_ACC_PUBLIC)
    PHP_ME(Message, setDouble, arginfo_message_float, ZEND_ACC_PUBLIC)
    PHP_ME(Message, setString, arginfo_message_string, ZEND_ACC_PUBLIC)
    PHP_ME(Message, setBytes, arginfo_message_string, ZEND_ACC_PUBLIC)
    PHP_ME(Message, setMessage, arginfo_message_field, ZEND_ACC_PUBLIC)
    PHP_ME(Message, clear, arginfo_message_field, ZEND_ACC_PUBLIC)
    PHP_ME(Message, has, arginfo_message_field, ZEND_ACC_PUBLIC)
    PHP_ME(Message, getInt, arginfo_message_field, ZEND_ACC_PUBLIC)
    PHP_ME(Message, getBool, arginfo_message_field, ZEND_ACC_PUBLIC)
    PHP_ME(Message, getFloat, arginfo_message_field, ZEND_ACC_PUBLIC)
    PHP_ME(Message, getDouble, arginfo_message_field, ZEND_ACC_PUBLIC)
    PHP_ME(Message, getString, arginfo_message_field, ZEND_ACC_PUBLIC)
    PHP_ME(Message, getBytes, arginfo_message_field, ZEND_ACC_PUBLIC)
    PHP_ME(Message, getMessage, arginfo_message_field, ZEND_ACC_PUBLIC)
    PHP_ME(Message, count, arginfo_message_field, ZEND_ACC_PUBLIC)
    PHP_ME(Message, at, arginfo_message_at, ZEND_ACC_PUBLIC)
    PHP_ME(Message, encode, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Message, encodeDeterministic, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Message, encodedSize, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Message, encodeTo, arginfo_message_encode_to, ZEND_ACC_PUBLIC)
    PHP_ME(Message, encodeDelimited, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Message, encodeDelimitedTo, arginfo_message_encode_to, ZEND_ACC_PUBLIC)
    PHP_ME(Message, toJson, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Message, toText, arginfo_message_totext, ZEND_ACC_PUBLIC)
    PHP_ME(Message, anyTypeUrl, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Message, anyPayload, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Message, timestampMillis, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Message, durationMillis, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Message, fieldmaskPaths, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Message, close, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_FE_END,
};

static const zend_function_entry value_methods[] = {
    PHP_ME(Value, __construct, arginfo_noargs,
           ZEND_ACC_PRIVATE | ZEND_ACC_CTOR)
    PHP_ME(Value, asInt, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Value, asBool, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Value, asFloat, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Value, asDouble, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Value, asString, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Value, asBytes, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_ME(Value, asMessage, arginfo_noargs, ZEND_ACC_PUBLIC)
    PHP_FE_END,
};

/* ---------------------------------------------------------------- module */

PHP_MINIT_FUNCTION(pbwire) {
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "Pbwire\\Error", NULL);
    pbwire_error_ce =
        zend_register_internal_class_ex(&ce, spl_ce_RuntimeException);

    INIT_CLASS_ENTRY(ce, "Pbwire\\Schema", schema_methods);
    pbwire_schema_ce = zend_register_internal_class(&ce);
    pbwire_schema_ce->create_object = schema_create;
    memcpy(&schema_handlers, &std_object_handlers,
           sizeof(zend_object_handlers));
    schema_handlers.offset = XtOffsetOf(schema_obj, std);
    schema_handlers.free_obj = schema_free;

    INIT_CLASS_ENTRY(ce, "Pbwire\\Type", type_methods);
    pbwire_type_ce = zend_register_internal_class(&ce);
    pbwire_type_ce->create_object = type_create_obj;
    memcpy(&type_handlers, &std_object_handlers,
           sizeof(zend_object_handlers));
    type_handlers.offset = XtOffsetOf(type_obj, std);
    type_handlers.free_obj = type_free;
    type_handlers.get_gc = type_get_gc;

    INIT_CLASS_ENTRY(ce, "Pbwire\\Field", field_methods);
    pbwire_field_ce = zend_register_internal_class(&ce);
    pbwire_field_ce->create_object = field_create_obj;
    memcpy(&field_handlers, &std_object_handlers,
           sizeof(zend_object_handlers));
    field_handlers.offset = XtOffsetOf(field_obj, std);
    field_handlers.free_obj = field_free;


    INIT_CLASS_ENTRY(ce, "Pbwire\\Message", message_methods);
    pbwire_message_ce = zend_register_internal_class(&ce);
    pbwire_message_ce->create_object = message_create_obj;
    memcpy(&message_handlers, &std_object_handlers,
           sizeof(zend_object_handlers));
    message_handlers.offset = XtOffsetOf(message_obj, std);
    message_handlers.free_obj = message_free;

    INIT_CLASS_ENTRY(ce, "Pbwire\\Value", value_methods);
    pbwire_value_ce = zend_register_internal_class(&ce);
    pbwire_value_ce->create_object = value_create_obj;
    memcpy(&value_handlers, &std_object_handlers,
           sizeof(zend_object_handlers));
    value_handlers.offset = XtOffsetOf(value_obj, std);
    value_handlers.free_obj = value_free;

    return SUCCESS;
}

PHP_MINFO_FUNCTION(pbwire) {
    php_info_print_table_start();
    php_info_print_table_row(2, "pbwire support", "enabled");
    php_info_print_table_end();
}

zend_module_entry pbwire_module_entry = {
    STANDARD_MODULE_HEADER,
    "pbwire",
    NULL, /* functions */
    PHP_MINIT(pbwire),
    NULL, /* MSHUTDOWN */
    NULL, /* RINIT */
    NULL, /* RSHUTDOWN */
    PHP_MINFO(pbwire),
    "0.1.18",
    STANDARD_MODULE_PROPERTIES,
};

#ifdef COMPILE_DL_PBWIRE
ZEND_GET_MODULE(pbwire)
#endif
