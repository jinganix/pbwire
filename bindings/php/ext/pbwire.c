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
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(bytes, len)
    ZEND_PARSE_PARAMETERS_END();

    pbw_schema *schema = NULL;
    pbw_status st = pbw_descriptor_set_load(bytes, len, &schema);
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
    zend_object_std_dtor(&t->std);
}

/* The field cache lives in the struct, not in the properties table, so the
 * Type <-> Field reference cycle (Field keeps its Type alive) is invisible to
 * the Zend collector without this handler: an unreachable schema would then
 * never be freed. Reporting the array lets gc_collect_cycles walk it. */
static HashTable *type_get_gc(zend_object *o, zval **table, int *n) {
    type_obj *t = type_from(o);

    if (Z_TYPE(t->fields) == IS_ARRAY) {
        *table = Z_ARRVAL(t->fields);
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

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_field, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_fieldbynumber, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, number, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_decode, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, wire, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_type_nestedtype, 0, 0, 1)
ZEND_ARG_OBJ_INFO(0, field, Pbwire\\Field, 0)
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
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(wire, wire_len)
    ZEND_PARSE_PARAMETERS_END();

    type_obj *t = type_from(Z_OBJ_P(ZEND_THIS));
    pbw_message *msg = NULL;
    pbw_status st = pbw_decode(t->desc, wire, wire_len, &msg);
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

PHP_METHOD(Type, isMessageField) {
    zval *field_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(field_zv, pbwire_field_ce)
    ZEND_PARSE_PARAMETERS_END();

    field_obj *fo = field_from(Z_OBJ_P(field_zv));
    pbw_type ty = pbw_field_type(fo->field);
    RETURN_BOOL(ty == PBW_TYPE_MESSAGE || ty == PBW_TYPE_GROUP);
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
    m->encode_hint = len;
    RETURN_STR(str);
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
    if (type_create(&subtype, &m->type_zv, sub) != 0) {
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
    if (type_create(&subtype, &m->type_zv, subdesc) != 0) {
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
        if (type_create(&subtype, &m->type_zv, sub) != 0) {
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
    PHP_ME(Type, nestedType, arginfo_type_nestedtype, ZEND_ACC_PUBLIC)
    PHP_ME(Type, isMessageField, arginfo_type_nestedtype, ZEND_ACC_PUBLIC)
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
    "0.1.0",
    STANDARD_MODULE_PROPERTIES,
};

#ifdef COMPILE_DL_PBWIRE
ZEND_GET_MODULE(pbwire)
#endif
