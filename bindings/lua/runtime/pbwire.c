/* Lua binding: Lua tables <-> wire bytes over the pbwire C core.
 *
 * A FileDescriptorSet (protoc --descriptor_set_out) loaded at run time is the
 * schema, so field numbers, types and presence rules are the descriptor's,
 * never a hand-written copy. No plugin, no generated code, no second wire
 * implementation.
 *
 * Messages are plain Lua tables, which is what game code (skynet, cocos,
 * xLua) already passes around:
 *
 *   local pbwire = require("pbwire")
 *   local schema = pbwire.loadfile("sample.desc")
 *   local wire = schema:encode("pbwire.lua.Sample", { name = "alice" })
 *   local back = schema:decode("pbwire.lua.Sample", wire)
 *
 * Hot loops use a builder instead (build once, encode per send):
 *
 *   local m = schema:message("pbwire.lua.Sample")
 *   m:set({ name = "alice" })
 *   local w1 = m:encode()
 *
 * The schema owns one pbw_ctx (arena + encode buffer, reset per call), so
 * steady use stops calling malloc: table encode/decode converts through
 * the arena, builders own a plain tree each. A builder anchors its schema
 * with a registry reference, so collecting the schema never dangles it.
 *
 * Value mapping: signed integers, enums <-> Lua numbers (integer),
 * unsigned and fixed-width integers <-> Lua numbers, bool <-> boolean, float/double <->
 * number, string/bytes <-> Lua string, message <-> nested table, repeated <->
 * array table (1-based; assign {} to clear). An absent singular field
 * decodes as nil. Groups map like messages (START_GROUP framing on the
 * wire); maps are arrays of {key=, value=} entries.
 *
 * shortcut: 64-bit integers need Lua 5.3+ integers for exactness; on
 * Lua 5.1/LuaJIT (all numbers are doubles) values past 2^53 round-trip
 * approximately, the same caveat lua-protobuf documents. Decode pushes a
 * uint64 past LUA_MAXINTEGER as the nearest double.
 */
#include <lua.h>
#include <stdint.h>
#include <lauxlib.h>

#if defined(__GNUC__) || defined(__clang__)
/* The installed headers do not mark luaL_error() noreturn, so the analyzer
 * explores the fall-through path and reports every guarded NULL use below
 * as a defect. It never returns (it longjmps); redeclare it so the analyzer
 * and the optimizer know the error paths do not fall through. */
int luaL_error(lua_State *L, const char *fmt, ...) __attribute__((noreturn));
#endif

#include <pbwire.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PBWLUA_SCHEMA_MT "PbwireSchema"
#define PBWLUA_MESSAGE_MT "PbwireMessage"

/* == PBW_MAX_DEPTH (include/pbwire.h): nesting cap the decoder enforces. */
#define PBWLUA_MAX_DEPTH 256

#if LUA_VERSION_NUM >= 502
#define PBWLUA_LEN(L, i) lua_rawlen(L, i)
#else
#define PBWLUA_LEN(L, i) lua_objlen(L, i)
static int
pbwlua_absindex(lua_State *L, int index)
{
    return index > 0 ? index : lua_gettop(L) + index + 1;
}
#define lua_absindex pbwlua_absindex
#endif

typedef struct {
    pbw_schema *schema;
    /* One arena + encode buffer for every call. Lua states are
     * single-threaded and fill/push run no metamethods (raw table access
     * only), so no reentrant call can reset it mid-use. */
    pbw_ctx *ctx;
    /* Reusable wire buffer for direct table encodes: reset per call, grows
     * once to the working size, then stays malloc-free. */
    uint8_t *wdata;
    size_t wlen;
    size_t wcap;
    /* Field-name string cache: one registry reference per field per
     * descriptor, so per-encode rawgets push a cached string (array access)
     * instead of interning a C string (hash). */
    struct pbwlua_namecache *ncache;
} pbwlua_schema;

/* One prepared field (the Erlang binding's "spot"): every wire fact the
 * encode loop needs, resolved once per descriptor, so a per-encode loop
 * costs no pbw_field_* calls at all. */
struct pbwlua_fentry {
    const pbw_field *field;
    const pbw_descriptor *sub; /* message/group type, NULL otherwise */
    const void *ptr;           /* interned key pointer, for the key check */
    int ref;                   /* registry ref to the interned key string */
    uint32_t number;
    unsigned char type;  /* pbw_type */
    unsigned char wt;    /* wire type for the tag */
    unsigned char repeated;
    unsigned char packed;
};

struct pbwlua_namecache {
    const pbw_descriptor *desc;
    struct pbwlua_fentry *f;
    size_t n;
    /* Interned-key pointer -> field index, open addressing: the unknown-key
     * check is one probe per key instead of a linear scan over n. */
    int *idx;
    size_t mask;
    struct pbwlua_namecache *next;
};

/* A builder owns a plain malloc tree. It anchors its schema with a registry
 * reference (released by __gc): pbw_message_free() reads the descriptor, so
 * the schema must outlive every builder.
 *
 * A context decode borrows the context's arena instead (readonly = 1): the
 * tree dies with the context's reset, so the message anchors the context
 * too and every access checks the generation the decode saw. */
/* A context: reusable arena + encode buffer for a decode/encode loop, so
 * steady use stops calling malloc. Decoded trees borrow it: they die with
 * reset/release, writes to them fail, and use after reset raises. */
struct pbwlua_context {
    pbw_ctx *ctx;
    pbw_pool *pool; /* NULL for standalone contexts */
    int pool_ref;   /* registry anchor when pooled */
    int released;
    unsigned gen; /* bumped by reset/release; messages hold their own */
    size_t retain_cap; /* tracked here; the core only takes a setter */
};

typedef struct pbwlua_context pbwlua_context;

typedef struct {
    const pbw_descriptor *desc;
    pbw_message *tree;
    int schema_ref;
    pbwlua_schema *s; /* for the schema's reusable encode buffer */
    int readonly; /* context borrow: set() fails, __gc skips the free */
    int ctx_ref; /* registry anchor of the borrowed context */
    pbwlua_context *c;
    unsigned gen; /* context generation seen at decode */
} pbwlua_message;

static pbw_value *
new_uint_bits(pbw_ctx *ctx, uint64_t bits)
{
    if (ctx != NULL) {
        return pbw_value_new_uint_ctx(ctx, bits);
    }
    return pbw_value_new_uint(bits);
}

static pbw_value *
new_bytes_copy(pbw_ctx *ctx, const void *data, size_t len)
{
    if (ctx != NULL) {
        return pbw_value_new_bytes_ctx(ctx, data, len);
    }
    return pbw_value_new_bytes(data, len);
}

static pbwlua_message *
pbwlua_check_message(lua_State *L, int index)
{
    pbwlua_message *m = (pbwlua_message *)luaL_checkudata(
        L, index, PBWLUA_MESSAGE_MT);

    if (m->tree == NULL) {
        luaL_error(L, "pbwire: message is closed");
    }
    if (m->c != NULL && m->gen != m->c->gen) {
        luaL_error(L, "pbwire: context was reset");
    }
    return m;
}

static pbwlua_schema *
pbwlua_check_schema(lua_State *L, int index)
{
    pbwlua_schema *s = (pbwlua_schema *)luaL_checkudata(L, index,
                                                        PBWLUA_SCHEMA_MT);

    if (s->schema == NULL) {
        luaL_error(L, "pbwire: schema is closed");
    }
    return s;
}

static const pbw_descriptor *
pbwlua_find(lua_State *L, pbw_schema *schema, const char *name)
{
    const pbw_descriptor *desc = pbw_schema_find(schema, name);

    if (desc == NULL || pbw_descriptor_is_enum(desc)) {
        luaL_error(L, "pbwire: no message %s in the schema", name);
    }
    return desc;
}

/* ------------------------------------------------------------ table -> C */

static void pbwlua_fill(lua_State *L, pbw_ctx *ctx,
                    const pbw_descriptor *desc, pbw_message *msg,
                    int index, unsigned depth);

/* Optional limits table at `idx` (absent/nil = core defaults). */
static const pbw_limits *pbwlua_limits_opt(lua_State *L, int idx,
                                           pbw_limits *storage);

static pbw_value *
new_scalar(lua_State *L, pbw_ctx *ctx, const pbw_field *field, int index)
{
    /* ctx != NULL (table API): arena, no per-value malloc, everything dies
     * with the next ctx reset. ctx == NULL (builder): plain malloc values
     * owned by the tree. Bit patterns match the plain constructors. */
    switch (pbw_field_type(field)) {
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES: {
        size_t len = 0;
        const char *data = luaL_checklstring(L, index, &len);

        return new_bytes_copy(ctx, data, len);
    }
    case PBW_TYPE_FLOAT: {
        float number = (float)luaL_checknumber(L, index);
        uint32_t bits = 0;

        memcpy(&bits, &number, sizeof(bits));
        return new_uint_bits(ctx, bits);
    }
    case PBW_TYPE_DOUBLE: {
        double number = luaL_checknumber(L, index);
        uint64_t bits = 0;

        memcpy(&bits, &number, sizeof(bits));
        return new_uint_bits(ctx, bits);
    }
    case PBW_TYPE_BOOL:
        if (!lua_isboolean(L, index)) {
            luaL_error(L, "pbwire: field %s expects a boolean",
                       pbw_field_name(field));
        }
        return new_uint_bits(ctx, lua_toboolean(L, index) ? 1u : 0u);
    case PBW_TYPE_INT32:
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_SFIXED64:
    case PBW_TYPE_ENUM:
        if (lua_type(L, index) != LUA_TNUMBER) {
            luaL_error(L, "pbwire: field %s expects a number",
                       pbw_field_name(field));
        }
        return new_uint_bits(ctx, (uint64_t)lua_tointeger(L, index));
    default: /* unsigned and fixed-width integers: raw bit pattern */
        if (lua_type(L, index) != LUA_TNUMBER) {
            luaL_error(L, "pbwire: field %s expects a number",
                       pbw_field_name(field));
        }
        return new_uint_bits(ctx, (uint64_t)lua_tointeger(L, index));
    }
}

static void
pbwlua_set_one(lua_State *L, pbw_ctx *ctx, pbw_message *msg,
               const pbw_field *field, int index, unsigned depth)
{
    pbw_type type = pbw_field_type(field);
    pbw_status status;

    if (type == PBW_TYPE_MESSAGE || type == PBW_TYPE_GROUP) {
        pbw_message *child;

        if (pbw_field_message_type(field) == NULL) {
            luaL_error(L, "pbwire: unresolved message type for field %s",
                       pbw_field_name(field));
        }
        luaL_checktype(L, index, LUA_TTABLE);
        /* set() replaces: drop the previous submessage first (a no-op on a
         * fresh tree). */
        if (pbw_field_label(field) != PBW_LABEL_REPEATED) {
            pbw_message_clear(msg, pbw_field_number(field));
        }
        child = pbw_message_set_message(msg, pbw_field_number(field));
        if (child == NULL) {
            luaL_error(L, "pbwire: cannot set field %s",
                       pbw_field_name(field));
        }
        pbwlua_fill(L, ctx, pbw_field_message_type(field), child, index,
                    depth + 1);
        return;
    }
    if (type == PBW_TYPE_ENUM && lua_type(L, index) == LUA_TSTRING) {
        /* Numbers round-trip as numbers; names ("COLOR3_GREEN", what
         * lua-protobuf tables carry) resolve through the enum. */
        const pbw_descriptor *enum_desc = pbw_field_message_type(field);
        const char *name = lua_tostring(L, index);
        int32_t number = 0;
        pbw_value *value;

        if (enum_desc == NULL ||
            !pbw_enum_find_value(enum_desc, name, &number)) {
            luaL_error(L, "pbwire: field %s: unknown enum name %s",
                       pbw_field_name(field), name);
        }
        value = new_uint_bits(ctx, (uint64_t)(int64_t)number);
        if (value == NULL) {
            luaL_error(L, "pbwire: out of memory");
        }
        status = pbw_message_set_field(msg, field, value);
        if (status != PBW_OK) {
            pbw_value_free(value);
            luaL_error(L, "pbwire: field %s: %s", pbw_field_name(field),
                       pbw_strerror(status));
        }
        return;
    }
    {
        /* Ownership transfers only on success. */
        pbw_value *value = new_scalar(L, ctx, field, index);

        if (value == NULL) {
            luaL_error(L, "pbwire: out of memory");
        }
        status = pbw_message_set_field(msg, field, value);
        if (status != PBW_OK) {
            pbw_value_free(value);
            luaL_error(L, "pbwire: field %s: %s", pbw_field_name(field),
                       pbw_strerror(status));
        }
    }
}

/* One lua_next pass fills and validates: the core emits fields in
 * descriptor order no matter the insertion order, so equal tables encode to
 * equal bytes without a second pass. lua_next is raw (no __index/__pairs
 * metamethods run), which also keeps the shared ctx safe from reentrant
 * calls: with raw access nothing below can reset it mid-use. */
static void
pbwlua_fill(lua_State *L, pbw_ctx *ctx, const pbw_descriptor *desc,
            pbw_message *msg, int index, unsigned depth)
{
    int table;

    if (depth > PBWLUA_MAX_DEPTH) {
        luaL_error(L, "pbwire: nesting limit exceeded");
    }
    /* The C API guarantees only LUA_MINSTACK free slots: a deep message
     * would otherwise write past the value stack (lua_next pushes two).
     * Growing here keeps every level safe; the depth guard above still
     * caps C recursion first. */
    luaL_checkstack(L, 6, "pbwire: message too deep");
    luaL_checktype(L, index, LUA_TTABLE);
    table = lua_absindex(L, index);

    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
        const pbw_field *field = NULL;

        /* Key at -2, value at -1. */
        if (lua_type(L, -2) != LUA_TSTRING ||
            (field = pbw_descriptor_find_field_name(
                 desc, lua_tostring(L, -2))) == NULL) {
            luaL_error(L, "pbwire: %s has no field %s",
                       pbw_descriptor_name(desc),
                       lua_type(L, -2) == LUA_TSTRING
                           ? lua_tostring(L, -2)
                           : "?");
        }
        if (pbw_field_label(field) != PBW_LABEL_REPEATED) {
            pbwlua_set_one(L, ctx, msg, field, lua_gettop(L), depth);
            lua_pop(L, 1); /* value, keep key for lua_next */
            continue;
        }
        {
            size_t count;
            size_t k;

            luaL_checktype(L, -1, LUA_TTABLE);
            /* Assigning a repeated field replaces its contents (a no-op
             * clear on a fresh tree). */
            pbw_message_clear(msg, pbw_field_number(field));
            count = PBWLUA_LEN(L, lua_gettop(L));
            for (k = 1; k <= count; k++) {
                lua_rawgeti(L, -1, (int)k);
                if (pbw_field_type(field) == PBW_TYPE_MESSAGE ||
                    pbw_field_type(field) == PBW_TYPE_GROUP) {
                    pbw_message *child;

                    if (pbw_field_message_type(field) == NULL) {
                        luaL_error(L, "pbwire: unresolved message type "
                                      "for field %s",
                                   pbw_field_name(field));
                    }
                    luaL_checktype(L, -1, LUA_TTABLE);
                    /* Arena trees append in place on repeated fields. */
                    child = pbw_message_set_message(
                        msg, pbw_field_number(field));
                    if (child == NULL) {
                        luaL_error(L, "pbwire: cannot set field %s",
                                   pbw_field_name(field));
                    }
                    pbwlua_fill(L, ctx, pbw_field_message_type(field),
                                child, lua_gettop(L), depth + 1);
                } else {
                    pbwlua_set_one(L, ctx, msg, field, lua_gettop(L),
                                   depth);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1); /* the array, keep key for lua_next */
    }
}

/* ------------------------------------------------------------ C -> table */

static void pbwlua_push_value(lua_State *L, pbwlua_schema *s,
                              const pbw_field *field, const pbw_value *value,
                              unsigned depth);
static struct pbwlua_namecache *pbwlua_name_cache(lua_State *L,
                                                  pbwlua_schema *s,
                                                  const pbw_descriptor *desc,
                                                  size_t n);

static void
pbwlua_push_uint(lua_State *L, uint64_t bits)
{
#if LUA_VERSION_NUM >= 503
    if (bits <= (uint64_t)LUA_MAXINTEGER) {
        lua_pushinteger(L, (lua_Integer)bits);
    } else {
        /* Past the integer range: nearest double (documented). */
        lua_pushnumber(L, (lua_Number)bits);
    }
#else
    /* No integers below 5.3: every number is a double (documented). */
    lua_pushnumber(L, (lua_Number)bits);
#endif
}

static void
pbwlua_push_scalar(lua_State *L, pbw_type type, const pbw_value *value)
{
    switch (type) {
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES: {
        size_t len = 0;
        const void *data = pbw_value_get_bytes(value, &len);

        lua_pushlstring(L, (const char *)data, len);
        break;
    }
    case PBW_TYPE_FLOAT:
        lua_pushnumber(L, (lua_Number)pbw_value_get_float(value));
        break;
    case PBW_TYPE_DOUBLE:
        lua_pushnumber(L, (lua_Number)pbw_value_get_double(value));
        break;
    case PBW_TYPE_BOOL:
        lua_pushboolean(L, pbw_value_get_bool(value));
        break;
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_ENUM:
        /* get_int32: get_int() reinterprets the raw bits without
         * extending 32-bit negatives. */
        lua_pushinteger(L, (lua_Integer)pbw_value_get_int32(value));
        break;
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
        lua_pushinteger(L, (lua_Integer)pbw_value_get_int(value));
        break;
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32:
        lua_pushinteger(L, (lua_Integer)pbw_value_get_uint32(value));
        break;
    default:
        pbwlua_push_uint(L, pbw_value_get_uint(value));
        break;
    }
}

/* Wire tree -> Lua table. Walks the descriptor through the same prepared
 * cache the encode side uses, so a decoded table costs no string interning,
 * no descriptor lookup and no hash growth per field: name, type and
 * repetition are already resolved, and presence is one O(1) check. */
static void
pbwlua_push_message(lua_State *L, pbwlua_schema *s, const pbw_message *msg,
                    unsigned depth)
{
    const pbw_descriptor *desc;
    struct pbwlua_namecache *nc;
    size_t nfields;
    size_t i;
    size_t nkeys = 0;

    if (depth > PBWLUA_MAX_DEPTH) {
        luaL_error(L, "pbwire: nesting limit exceeded");
    }
    /* Same guarantee as the fill side: one table plus one value per level
     * with room for the nested call beneath. */
    luaL_checkstack(L, 6, "pbwire: message too deep");
    desc = pbw_message_descriptor(msg);
    nfields = pbw_descriptor_field_count(desc);
    nc = nfields > 0 ? pbwlua_name_cache(L, s, desc, nfields) : NULL;
    /* Exact hash part: present fields plus the empty table an absent
     * repeated field still owes. No rehash while filling, no over-
     * allocation either (a sparse message keeps a small hash). */
    for (i = 0; i < nfields; i++) {
        const struct pbwlua_fentry *f = &nc->f[i];

        nkeys += (size_t)(f->repeated ||
                          pbw_message_get_field(msg, f->field) != NULL);
    }
    lua_createtable(L, 0, (int)nkeys);
    for (i = 0; i < nfields; i++) {
        const struct pbwlua_fentry *f = &nc->f[i];
        pbw_value *value = pbw_message_get_field(msg, f->field);

        if (value == NULL) {
            if (!f->repeated) {
                continue;
            }
            /* An absent repeated field reads as an empty table, like the
             * other bindings (Ruby reads []): game code can always
             * #/ipairs it. */
            lua_createtable(L, 0, 0);
        } else if (pbw_value_get_kind(value) == PBW_VALUE_LIST) {
            size_t count = pbw_value_count(value);
            size_t k;

            /* Exact array size: no rehash while filling. */
            lua_createtable(L, (int)count, 0);
            for (k = 0; k < count; k++) {
                pbwlua_push_value(L, s, f->field, pbw_value_at(value, k),
                                  depth + 1);
                lua_rawseti(L, -2, (int)k + 1);
            }
        } else {
            pbwlua_push_value(L, s, f->field, value, depth + 1);
        }
        /* Cached key under the value: one rawset, no interning. */
        lua_rawgeti(L, LUA_REGISTRYINDEX, f->ref);
        lua_insert(L, -2);
        lua_rawset(L, -3);
    }
}

static void
pbwlua_push_value(lua_State *L, pbwlua_schema *s, const pbw_field *field,
                  const pbw_value *value, unsigned depth)
{
    if (pbw_value_get_kind(value) == PBW_VALUE_MESSAGE) {
        pbwlua_push_message(L, s, pbw_value_get_message(value), depth);
    } else {
        pbwlua_push_scalar(L, pbw_field_type(field), value);
    }
}

/* ------------------------------------------------------------ methods */

static int
pbwlua_load_bytes(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    pbw_schema *schema = NULL;
    pbw_limits storage;
    pbw_status status;
    pbwlua_schema *s;

    status = pbw_descriptor_set_load_limits(data, len, pbwlua_limits_opt(L, 2, &storage), &schema);
    if (status != PBW_OK) {
        /* The reason lives inside the schema: copy it before freeing it
         * (luaL_error longjmps after the free, so a pointer would be stale). */
        char reason[256];
        const char *why = schema != NULL ? pbw_schema_error(schema) : NULL;

        snprintf(reason, sizeof(reason), "%s", why != NULL ? why : "");
        pbw_schema_free(schema);
        luaL_error(L, "pbwire: descriptor set rejected: %s",
                   reason[0] != '\0' ? reason : pbw_strerror(status));
    }
    s = (pbwlua_schema *)lua_newuserdata(L, sizeof(*s));
    s->schema = schema;
    s->wdata = NULL;
    s->wlen = 0;
    s->wcap = 0;
    s->ncache = NULL;
    s->ctx = pbw_ctx_new();
    if (s->ctx == NULL) {
        pbw_schema_free(schema);
        luaL_error(L, "pbwire: out of memory");
    }
    luaL_getmetatable(L, PBWLUA_SCHEMA_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int
pbwlua_load_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    FILE *file = fopen(path, "rb");
    uint8_t *data = NULL;
    size_t len = 0;
    size_t cap = 0;

    if (file == NULL) {
        luaL_error(L, "pbwire: cannot open %s", path);
    }
    for (;;) {
        size_t want;
        size_t got;

        if (len == cap) {
            size_t grown = cap == 0 ? 8192 : cap * 2;
            uint8_t *next = (uint8_t *)realloc(data, grown);

            if (next == NULL) {
                free(data);
                fclose(file);
                luaL_error(L, "pbwire: out of memory");
            }
            data = next;
            cap = grown;
        }
        want = cap - len;
        got = fread(data + len, 1, want, file);
        len += got;
        if (got < want) {
            break;
        }
    }
    fclose(file);
    lua_pushlstring(L, (const char *)data, len);
    free(data);
    /* Reuse the bytes path so every failure reports the same text. */
    lua_replace(L, 1);
    return pbwlua_load_bytes(L);
}

/* Direct table -> wire encoder for schema:encode(). The tree path
 * (fill + encode) costs two passes plus per-value arena bookkeeping; the
 * benchmark table path is fill-dominated (builder encode alone runs at the
 * core's ~1.5 GB/s, the table path at ~0.15 GB/s). Writing straight from
 * the Lua table into the wire buffer removes the tree entirely, like the
 * Python binding's dict path: one pass over schema fields in descriptor
 * order (so bytes stay canonical regardless of table insertion order),
 * one raw hash lookup per field, no per-value allocation.
 * Presence matches the tree path: a key present in the table (even 0,
 * false or "") encodes; an absent key (nil) skips. An unknown key is an
 * error, never silence. */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    int borrowed; /* data is schema reuse buffer: grow by copy, not realloc */
    /* Non-NULL for the table encoder: every growth is recorded in the schema
     * userdata, so a luaL_error raised later frees it in the __gc instead of
     * leaking one grown buffer per malformed request. */
    pbwlua_schema *owner;
} pbwlua_buf;

/* Slow path of pbwlua_buf_reserve(): the buffer has to grow, or the request
 * cannot be represented. Out of line so the per-write check is one compare
 * and one branch (the same split the core writer uses). */
static void
pbwlua_buf_grow(lua_State *L, pbwlua_buf *b, size_t extra)
{
    size_t need = b->len + extra;
    size_t cap;
    uint8_t *data;

    if (need < b->len) {
        /* Size arithmetic wrapped: no buffer can hold this, and returning
         * would let the caller write past the capacity it has. */
        luaL_error(L, "pbwire: string too long to encode");
    }
    if (need <= b->cap) {
        return;
    }
    cap = b->cap == 0 ? 256 : b->cap;
    while (cap < need) {
        if (cap > SIZE_MAX - cap / 2) {
            cap = need;
            break;
        }
        cap += cap / 2;
    }
    if (b->borrowed) {
        /* First growth while borrowing the schema buffer: copy, do not
         * realloc, so a later luaL_error leaves s->wdata valid. The copy
         * leaks on the error path (rare); success frees the old below. */
        data = (uint8_t *)malloc(cap);
        if (data == NULL) {
            luaL_error(L, "pbwire: out of memory");
        }
        if (b->len > 0) {
            memcpy(data, b->data, b->len);
        }
        b->data = data;
        b->cap = cap;
        b->borrowed = 0;
        if (b->owner != NULL) {
            free(b->owner->wdata); /* the borrowed buffer this copy replaces */
            b->owner->wdata = data;
            b->owner->wcap = cap;
        }
        return;
    }
    data = (uint8_t *)realloc(b->data, cap);
    if (data == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    b->data = data;
    b->cap = cap;
    if (b->owner != NULL) {
        b->owner->wdata = data;
        b->owner->wcap = cap;
    }
}

static inline void
pbwlua_buf_reserve(lua_State *L, pbwlua_buf *b, size_t extra)
{
    if (b->len <= b->cap && extra <= b->cap - b->len) {
        return;
    }
    pbwlua_buf_grow(L, b, extra);
}

static inline void
pbwlua_buf_varint(lua_State *L, pbwlua_buf *b, uint64_t v)
{
    pbwlua_buf_reserve(L, b, 10);
    while (v >= 0x80) {
        b->data[b->len++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    b->data[b->len++] = (uint8_t)v;
}

static inline void
pbwlua_buf_tag(lua_State *L, pbwlua_buf *b, uint32_t fnum, unsigned wt)
{
    pbwlua_buf_varint(L, b, ((uint64_t)fnum << 3) | wt);
}

static inline void
pbwlua_buf_fixed32(lua_State *L, pbwlua_buf *b, uint32_t v)
{
    pbwlua_buf_reserve(L, b, 4);
    b->data[b->len++] = (uint8_t)v;
    b->data[b->len++] = (uint8_t)(v >> 8);
    b->data[b->len++] = (uint8_t)(v >> 16);
    b->data[b->len++] = (uint8_t)(v >> 24);
}

static inline void
pbwlua_buf_fixed64(lua_State *L, pbwlua_buf *b, uint64_t v)
{
    pbwlua_buf_reserve(L, b, 8);
    b->data[b->len++] = (uint8_t)v;
    b->data[b->len++] = (uint8_t)(v >> 8);
    b->data[b->len++] = (uint8_t)(v >> 16);
    b->data[b->len++] = (uint8_t)(v >> 24);
    b->data[b->len++] = (uint8_t)(v >> 32);
    b->data[b->len++] = (uint8_t)(v >> 40);
    b->data[b->len++] = (uint8_t)(v >> 48);
    b->data[b->len++] = (uint8_t)(v >> 56);
}

static inline void
pbwlua_buf_bytes(lua_State *L, pbwlua_buf *b, const char *data, size_t len)
{
    if (len == 0) {
        return;
    }
    pbwlua_buf_reserve(L, b, len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

/* Length placeholder patch: nested messages and packed runs write one zero
 * byte, encode the payload, then fix the length. Short payloads (the common
 * case) patch in place; longer ones shift the payload forward. */
static void
pbwlua_buf_patch(lua_State *L, pbwlua_buf *b, size_t lp, size_t len)
{
    if (len < 128) {
        b->data[lp] = (uint8_t)len;
        return;
    }
    {
        uint8_t tmp[10];
        size_t llen = 0;
        size_t v = len;

        while (v >= 0x80) {
            tmp[llen++] = (uint8_t)(v | 0x80);
            v >>= 7;
        }
        tmp[llen++] = (uint8_t)v;
        if (llen > 1) {
            pbwlua_buf_reserve(L, b, llen - 1);
            memmove(b->data + lp + llen, b->data + lp + 1, len);
        }
        memcpy(b->data + lp, tmp, llen);
        b->len += llen - 1;
    }
}

static uint64_t
pbwlua_zig32(uint64_t v)
{
    return ((uint64_t)(int64_t)(int32_t)v << 1) ^
           (uint64_t)((int32_t)v >> 31);
}

static uint64_t
pbwlua_zig64(uint64_t v)
{
    return (v << 1) ^ (uint64_t)((int64_t)v >> 63);
}

/* Wire value for one scalar bit pattern (sign extension, zigzag, bool
 * normalization), mirroring the Python binding's wire_value. */
static uint64_t
pbwlua_wire_value(pbw_type type, uint64_t bits)
{
    switch (type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_ENUM:
        return (uint64_t)(int64_t)(int32_t)bits;
    case PBW_TYPE_UINT32:
        return (uint32_t)bits;
    case PBW_TYPE_SINT32:
        return pbwlua_zig32(bits);
    case PBW_TYPE_SINT64:
        return pbwlua_zig64(bits);
    case PBW_TYPE_BOOL:
        return bits != 0 ? 1u : 0u;
    default:
        return bits;
    }
}

static unsigned
pbwlua_wire_type(pbw_type type)
{
    switch (type) {
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        return 1;
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        return 5;
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
    case PBW_TYPE_MESSAGE:
        return 2;
    case PBW_TYPE_GROUP:
        return 3;
    default:
        return 0;
    }
}

static int
pbwlua_is_packable(pbw_type type)
{
    return type != PBW_TYPE_STRING && type != PBW_TYPE_BYTES &&
           type != PBW_TYPE_MESSAGE && type != PBW_TYPE_GROUP;
}

static void
pbwlua_put_scalar(lua_State *L, pbwlua_buf *b, pbw_type type, uint64_t bits)
{
    switch (type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_ENUM: {
        int32_t v = (int32_t)bits;

        if (v >= 0) {
            pbwlua_buf_varint(L, b, (uint64_t)v);
        } else {
            pbwlua_buf_varint(L, b, (uint64_t)(int64_t)v);
        }
        return;
    }
    case PBW_TYPE_INT64:
    case PBW_TYPE_UINT64:
    case PBW_TYPE_UINT32:
    case PBW_TYPE_BOOL:
        pbwlua_buf_varint(L, b, pbwlua_wire_value(type, bits));
        return;
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SINT64:
        pbwlua_buf_varint(L, b, pbwlua_wire_value(type, bits));
        return;
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_FLOAT:
        pbwlua_buf_fixed32(L, b, (uint32_t)bits);
        return;
    default:
        pbwlua_buf_fixed64(L, b, bits);
        return;
    }
}

/* Lua value at index -> 64-bit bit pattern for one scalar. Errors on type
 * mismatch, like new_scalar, including enum names. */
static uint64_t
pbwlua_to_bits(lua_State *L, const pbw_field *field, int index)
{
    pbw_type type = pbw_field_type(field);

    switch (type) {
    case PBW_TYPE_FLOAT: {
        float number = (float)luaL_checknumber(L, index);
        uint32_t bits = 0;

        memcpy(&bits, &number, sizeof(bits));
        return bits;
    }
    case PBW_TYPE_DOUBLE: {
        double number = luaL_checknumber(L, index);
        uint64_t bits = 0;

        memcpy(&bits, &number, sizeof(bits));
        return bits;
    }
    case PBW_TYPE_BOOL:
        if (!lua_isboolean(L, index)) {
            luaL_error(L, "pbwire: field %s expects a boolean",
                       pbw_field_name(field));
        }
        return lua_toboolean(L, index) ? 1u : 0u;
    case PBW_TYPE_INT32:
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_SFIXED64:
    case PBW_TYPE_ENUM:
        if (type == PBW_TYPE_ENUM && lua_type(L, index) == LUA_TSTRING) {
            const pbw_descriptor *enum_desc =
                pbw_field_message_type(field);
            int32_t number = 0;

            if (enum_desc == NULL || !pbw_enum_find_value(
                    enum_desc, lua_tostring(L, index), &number)) {
                luaL_error(L, "pbwire: field %s: unknown enum name %s",
                           pbw_field_name(field), lua_tostring(L, index));
            }
            return (uint64_t)(int64_t)number;
        }
        if (lua_type(L, index) != LUA_TNUMBER) {
            luaL_error(L, "pbwire: field %s expects a number",
                       pbw_field_name(field));
        }
        return (uint64_t)lua_tointeger(L, index);
    default:
        if (lua_type(L, index) != LUA_TNUMBER) {
            luaL_error(L, "pbwire: field %s expects a number",
                       pbw_field_name(field));
        }
        return (uint64_t)lua_tointeger(L, index);
    }
}

static void pbwlua_encode_table(lua_State *L, pbwlua_buf *b,
                                pbwlua_schema *s,
                                const pbw_descriptor *desc, int index,
                                unsigned depth);
static struct pbwlua_namecache *
pbwlua_name_cache(lua_State *L, pbwlua_schema *s,
                  const pbw_descriptor *desc, size_t n)
{
    struct pbwlua_namecache *c;
    size_t i;

    for (c = s->ncache; c != NULL; c = c->next) {
        if (c->desc == desc) {
            return c;
        }
    }
    {
        struct pbwlua_fentry *f =
            (struct pbwlua_fentry *)malloc(n == 0 ? 1 : n * sizeof(*f));
        size_t done = 0;
        if (f == NULL) {
            luaL_error(L, "pbwire: out of memory");
        }
        for (i = 0; i < n; i++) {
            const pbw_field *field = pbw_descriptor_field_at(desc, i);
            pbw_type type;

            /* Zero first: the loops below read type/repeated even for an
             * entry whose field lookup came back NULL. */
            memset(&f[i], 0, sizeof(f[i]));
            f[i].field = field;
            f[i].ref = LUA_NOREF;
            if (field == NULL) {
                continue;
            }
            type = pbw_field_type(field);
            lua_pushstring(L, pbw_field_name(field));
            f[i].ptr = lua_topointer(L, -1);
            f[i].ref = luaL_ref(L, LUA_REGISTRYINDEX);
            f[i].number = pbw_field_number(field);
            f[i].type = (unsigned char)type;
            f[i].wt = (unsigned char)pbwlua_wire_type(type);
            f[i].repeated = pbw_field_label(field) == PBW_LABEL_REPEATED;
            f[i].sub = pbw_field_message_type(field);
            f[i].packed = pbw_field_is_packed(field) &&
                                  pbwlua_is_packable(type)
                              ? 1
                              : 0;
            done = i + 1;
        }
        {
            struct pbwlua_namecache *nc =
                (struct pbwlua_namecache *)malloc(sizeof(*nc));
            size_t cap = 2;
            int *idx;

            while (cap < n * 2) {
                cap *= 2;
            }
            idx = (int *)malloc(cap * sizeof(*idx));
            if (nc == NULL || idx == NULL) {
                for (i = 0; i < done; i++) {
                    if (f[i].ref != LUA_NOREF) {
                        luaL_unref(L, LUA_REGISTRYINDEX, f[i].ref);
                    }
                }
                free(f);
                free(nc);
                free(idx);
                luaL_error(L, "pbwire: out of memory");
            }
            for (i = 0; i < cap; i++) {
                idx[i] = -1;
            }
            for (i = 0; i < n; i++) {
                if (f[i].ptr != NULL) {
                    size_t j = ((size_t)(uintptr_t)f[i].ptr >> 4) & (cap - 1);

                    while (idx[j] >= 0) {
                        j = (j + 1) & (cap - 1);
                    }
                    idx[j] = (int)i;
                }
            }
            nc->desc = desc;
            nc->f = f;
            nc->n = n;
            nc->idx = idx;
            nc->mask = cap - 1;
            nc->next = s->ncache;
            s->ncache = nc;
            return nc;
        }
    }
}

static void
pbwlua_write_message(lua_State *L, pbwlua_buf *b, pbwlua_schema *s,
                     const struct pbwlua_fentry *f, int index, unsigned depth)
{
    const pbw_descriptor *sub = f->sub;

    if (sub == NULL) {
        luaL_error(L, "pbwire: unresolved message type for field %s",
                   pbw_field_name(f->field));
    }
    luaL_checktype(L, index, LUA_TTABLE);
    if (f->type == PBW_TYPE_GROUP) {
        pbwlua_buf_tag(L, b, f->number, 3);
        pbwlua_encode_table(L, b, s, sub, index, depth + 1);
        pbwlua_buf_tag(L, b, f->number, 4);
        return;
    }
    pbwlua_buf_tag(L, b, f->number, 2);
    {
        size_t lp;
        size_t base;

        pbwlua_buf_reserve(L, b, 1);
        lp = b->len;
        b->data[b->len++] = 0;
        base = b->len;
        pbwlua_encode_table(L, b, s, sub, index, depth + 1);
        pbwlua_buf_patch(L, b, lp, b->len - base);
    }
}

static void
pbwlua_encode_table(lua_State *L, pbwlua_buf *b, pbwlua_schema *s,
                    const pbw_descriptor *desc, int index, unsigned depth)
{
    size_t nfields;
    size_t i;
    int table;
    struct pbwlua_namecache *nc = NULL;

    if (depth > PBWLUA_MAX_DEPTH) {
        luaL_error(L, "pbwire: nesting limit exceeded");
    }
    luaL_checkstack(L, 6, "pbwire: message too deep");
    luaL_checktype(L, index, LUA_TTABLE);
    table = lua_absindex(L, index);
    nfields = pbw_descriptor_field_count(desc);
    if (nfields > 0) {
        nc = pbwlua_name_cache(L, s, desc, nfields);
    }
    for (i = 0; i < nfields; i++) {
        const struct pbwlua_fentry *f = &nc->f[i];
        const pbw_field *field = f->field;
        pbw_type type = (pbw_type)f->type;
        int is_repeated = f->repeated;

        if (field == NULL) {
            continue;
        }
        /* Raw access via a cached string: lua_getfield would consult
         * __index, letting a metamethod inject values mid-encode (and
         * reenter the schema). The tree path uses lua_next (raw) for the
         * same reason. Pushing the cached string avoids re-interning. */
        lua_rawgeti(L, LUA_REGISTRYINDEX, f->ref);
        lua_rawget(L, table);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        if (!is_repeated) {
            if (type == PBW_TYPE_MESSAGE || type == PBW_TYPE_GROUP) {
                pbwlua_write_message(L, b, s, f, lua_gettop(L), depth);
            } else if (type == PBW_TYPE_STRING ||
                       type == PBW_TYPE_BYTES) {
                size_t len = 0;
                const char *data = luaL_checklstring(L, -1, &len);

                pbwlua_buf_tag(L, b, f->number, 2);
                pbwlua_buf_varint(L, b, (uint64_t)len);
                pbwlua_buf_bytes(L, b, data, len);
            } else {
                uint64_t bits = pbwlua_to_bits(L, field, lua_gettop(L));

                pbwlua_buf_tag(L, b, f->number, f->wt);
                pbwlua_put_scalar(L, b, type, bits);
            }
            lua_pop(L, 1);
            continue;
        }
        /* Repeated: a table array, empty encodes nothing. */
        if (!lua_istable(L, -1)) {
            luaL_error(L, "pbwire: field %s needs an array table",
                       pbw_field_name(field));
        }
        {
            size_t count = PBWLUA_LEN(L, lua_gettop(L));
            size_t k;

            if (count == 0) {
                lua_pop(L, 1);
                continue;
            }
            if (type == PBW_TYPE_MESSAGE || type == PBW_TYPE_GROUP) {
                for (k = 1; k <= count; k++) {
                    lua_rawgeti(L, -1, (int)k);
                    pbwlua_write_message(L, b, s, f, lua_gettop(L), depth);
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
                continue;
            }
            if (type == PBW_TYPE_STRING || type == PBW_TYPE_BYTES) {
                for (k = 1; k <= count; k++) {
                    size_t len = 0;
                    const char *data;

                    lua_rawgeti(L, -1, (int)k);
                    data = luaL_checklstring(L, -1, &len);
                    pbwlua_buf_tag(L, b, pbw_field_number(field), 2);
                    pbwlua_buf_varint(L, b, (uint64_t)len);
                    pbwlua_buf_bytes(L, b, data, len);
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
                continue;
            }
            if (f->packed) {
                pbwlua_buf_tag(L, b, f->number, 2);
                {
                    size_t lp;
                    size_t base;

                    pbwlua_buf_reserve(L, b, 1);
                    lp = b->len;
                    b->data[b->len++] = 0;
                    base = b->len;
                    for (k = 1; k <= count; k++) {
                        uint64_t bits;

                        lua_rawgeti(L, -1, (int)k);
                        bits = pbwlua_to_bits(L, field, lua_gettop(L));
                        lua_pop(L, 1);
                        if (type == PBW_TYPE_BOOL) {
                            pbwlua_buf_reserve(L, b, 1);
                            b->data[b->len++] =
                                (uint8_t)(bits != 0 ? 1 : 0);
                        } else if (type == PBW_TYPE_FIXED32 ||
                                   type == PBW_TYPE_SFIXED32 ||
                                   type == PBW_TYPE_FLOAT) {
                            pbwlua_buf_fixed32(L, b, (uint32_t)bits);
                        } else if (type == PBW_TYPE_FIXED64 ||
                                   type == PBW_TYPE_SFIXED64 ||
                                   type == PBW_TYPE_DOUBLE) {
                            pbwlua_buf_fixed64(L, b, bits);
                        } else {
                            pbwlua_buf_varint(
                                L, b, pbwlua_wire_value(type, bits));
                        }
                    }
                    pbwlua_buf_patch(L, b, lp, b->len - base);
                }
                lua_pop(L, 1);
                continue;
            }
            for (k = 1; k <= count; k++) {
                uint64_t bits;

                lua_rawgeti(L, -1, (int)k);
                bits = pbwlua_to_bits(L, field, lua_gettop(L));
                lua_pop(L, 1);
                pbwlua_buf_tag(L, b, f->number, f->wt);
                pbwlua_put_scalar(L, b, type, bits);
            }
            lua_pop(L, 1);
        }
    }
    /* Unknown keys are an error, never silence: one lua_next pass, each key
     * compared by interned-string pointer against the cached field strings
     * (no hash, no strcmp). */
    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "pbwire: %s has no field %s",
                       pbw_descriptor_name(desc), "?");
        }
        if (nc == NULL) {
            /* A message with no fields: every key is unknown. */
            luaL_error(L, "pbwire: %s has no field %s",
                       pbw_descriptor_name(desc), lua_tostring(L, -2));
        }
        {
            const void *kp = lua_topointer(L, -2);
            size_t j = ((size_t)(uintptr_t)kp >> 4) & nc->mask;
            int known = 0;

            while (nc->idx[j] >= 0) {
                if (nc->f[nc->idx[j]].ptr == kp) {
                    known = 1;
                    break;
                }
                j = (j + 1) & nc->mask;
            }
            if (!known) {
                /* Lua does not intern strings longer than LUAI_MAXSHORTLEN, so
                 * the pointer probe misses a key that really is a field:
                 * recheck by text (cold path, once per rejected key). */
                const char *name = lua_tostring(L, -2);
                size_t k;

                for (k = 0; name != NULL && k < nc->n; k++) {
                    if (strcmp(pbw_field_name(nc->f[k].field), name) == 0) {
                        known = 1;
                        break;
                    }
                }
            }
            if (!known) {
                luaL_error(L, "pbwire: %s has no field %s",
                           pbw_descriptor_name(desc), lua_tostring(L, -2));
            }
        }
        lua_pop(L, 1);
    }
}

static int
pbwlua_encode(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    pbwlua_buf b;

    b.data = s->wdata;
    b.len = 0;
    /* A NULL reuse buffer has no capacity even if a past path left one
     * behind: every writer below assumes reserve() grows a NULL buffer. */
    b.cap = s->wdata != NULL ? s->wcap : 0;
    b.borrowed = s->wdata != NULL;
    b.owner = s;
    luaL_checktype(L, 3, LUA_TTABLE);
    pbwlua_encode_table(L, &b, s, desc, 3, 0);
    if (b.borrowed) {
        /* No growth: still borrowing, nothing to save. */
        s->wlen = b.len;
    } else {
        /* Grew (or started fresh): take ownership, free the old reuse buffer. */
        if (s->wdata != b.data) {
            free(s->wdata);
        }
        s->wdata = b.data;
        s->wcap = b.cap;
        s->wlen = b.len;
    }
    lua_pushlstring(L, b.data != NULL ? (const char *)b.data : "", b.len);
    return 1;
}

/* Gateway passthrough (and the conformance binary path): decode and
 * re-encode through the core, so unknown fields survive. Known fields
 * come back canonical; unknown bytes are appended in input order, exactly
 * like the reference parsers. A request is accepted exactly when the core
 * accepts it, which is what the suite tests. */
static int
pbwlua_roundtrip(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    pbw_message *msg = NULL;
    const uint8_t *out = NULL;
    size_t outlen = 0;
    pbw_status status;

    pbw_ctx_reset(s->ctx);
    status = pbw_decode_ctx(s->ctx, desc, data, len, &msg);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: decode %s: %s", name, pbw_strerror(status));
    }
    status = pbw_encode_ctx(s->ctx, msg, &out, &outlen);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: encode %s: %s", name, pbw_strerror(status));
    }
    lua_pushlstring(L, (const char *)out, outlen);
    return 1;
}

static int
pbwlua_decode(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    pbw_message *msg = NULL;
    pbw_status status;

    pbw_limits storage;
    const pbw_limits *limits = pbwlua_limits_opt(L, 4, &storage);

    /* Arena decode: one bump allocator, no per-value malloc. The tree is
     * read-only and dies with the next reset; conversion below only reads. */
    pbw_ctx_reset(s->ctx);
    status = pbw_decode_ctx_limits(s->ctx, desc, data, len, limits, &msg);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: decode %s: %s", name, pbw_strerror(status));
    }
    pbwlua_push_message(L, s, msg, 0);
    return 1;
}

/* JSON mapping text of a table (built and printed through one reset). */
static int
pbwlua_to_json(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    pbw_message *msg;
    char *text = NULL;
    size_t len = 0;
    pbw_status status;

    pbw_ctx_reset(s->ctx);
    msg = pbw_message_new_ctx(s->ctx, desc);
    if (msg == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    luaL_checktype(L, 3, LUA_TTABLE);
    pbwlua_fill(L, s->ctx, desc, msg, 3, 0);
    status = pbw_json_print(msg, &text, &len);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: to_json %s: %s", name, pbw_strerror(status));
    }
    lua_pushlstring(L, text, len);
    pbw_free(text);
    return 1;
}

/* Parses JSON mapping text into a table; an optional third argument skips
 * unknown field names instead of failing (the conformance ignore-unknown
 * category). */
static int
pbwlua_from_json(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    pbw_message *msg = NULL;
    pbw_limits storage;
    pbw_status status;
    pbw_json_parse_options opts;

    opts.ignore_unknown_fields = lua_toboolean(L, 4);
    status = pbw_json_parse_limits(desc, data, len, pbwlua_limits_opt(L, 5, &storage), &opts, &msg);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: from_json %s: %s", name, pbw_strerror(status));
    }
    pbwlua_push_message(L, s, msg, 0);
    pbw_message_free(msg);
    return 1;
}

/* Text format of a table. */
static int
pbwlua_to_text(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    pbw_message *msg;
    char *text = NULL;
    size_t len = 0;
    pbw_status status;

    pbw_ctx_reset(s->ctx);
    msg = pbw_message_new_ctx(s->ctx, desc);
    if (msg == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    luaL_checktype(L, 3, LUA_TTABLE);
    pbwlua_fill(L, s->ctx, desc, msg, 3, 0);
    status = pbw_text_print(msg, &text, &len);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: to_text %s: %s", name, pbw_strerror(status));
    }
    lua_pushlstring(L, text, len);
    pbw_free(text);
    return 1;
}

/* Parses text format into a table. */
static int
pbwlua_from_text(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    pbw_message *msg = NULL;
    pbw_limits storage;
    pbw_status status;

    status = pbw_text_parse_limits(desc, data, len, pbwlua_limits_opt(L, 4, &storage), &msg);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: from_text %s: %s", name, pbw_strerror(status));
    }
    pbwlua_push_message(L, s, msg, 0);
    pbw_message_free(msg);
    return 1;
}

/* Canonical encoding: map entries sorted by key, like the reference. */
static int
pbwlua_encode_deterministic(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    pbw_message *msg;
    uint8_t *out = NULL;
    size_t len = 0;
    pbw_status status;

    pbw_ctx_reset(s->ctx);
    msg = pbw_message_new_ctx(s->ctx, desc);
    if (msg == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    luaL_checktype(L, 3, LUA_TTABLE);
    pbwlua_fill(L, s->ctx, desc, msg, 3, 0);
    status = pbw_encode_deterministic(msg, &out, &len);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: encode_deterministic %s: %s", name,
                   pbw_strerror(status));
    }
    lua_pushlstring(L, (const char *)out, len);
    pbw_free(out);
    return 1;
}

/* One length-delimited frame (varint length + message). */
static int
pbwlua_encode_delimited(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    pbw_message *msg;
    uint8_t *out = NULL;
    size_t len = 0;
    pbw_status status;

    pbw_ctx_reset(s->ctx);
    msg = pbw_message_new_ctx(s->ctx, desc);
    if (msg == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    luaL_checktype(L, 3, LUA_TTABLE);
    pbwlua_fill(L, s->ctx, desc, msg, 3, 0);
    status = pbw_encode_delimited(msg, &out, &len);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: encode_delimited %s: %s", name,
                   pbw_strerror(status));
    }
    lua_pushlstring(L, (const char *)out, len);
    pbw_free(out);
    return 1;
}

/* One frame from a string: the table plus bytes consumed, so concatenated
 * frames decode in a loop. */
static int
pbwlua_decode_delimited(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    pbw_message *msg = NULL;
    pbw_limits storage;
    size_t consumed = 0;
    pbw_status status;

    status = pbw_decode_delimited_limits(desc, data, len, pbwlua_limits_opt(L, 4, &storage), &msg,
                                         &consumed);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: decode_delimited %s: %s", name,
                   pbw_strerror(status));
    }
    pbwlua_push_message(L, s, msg, 0);
    pbw_message_free(msg);
    lua_pushinteger(L, (lua_Integer)consumed);
    return 2;
}

/* Pushes a builder holding tree (schema lives at index 1, anchored until
 * __gc). Plain trees only: arena trees die with the context's reset. */
static int
pbwlua_push_builder(lua_State *L, const pbw_descriptor *desc,
                    pbw_message *tree)
{
    pbwlua_message *m = (pbwlua_message *)lua_newuserdata(L, sizeof(*m));

    m->desc = desc;
    m->tree = tree;
    m->s = pbwlua_check_schema(L, 1);
    /* Anchor the schema: the tree reads its descriptor until __gc. */
    lua_pushvalue(L, 1);
    m->schema_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    m->readonly = 0;
    m->ctx_ref = LUA_NOREF;
    m->c = NULL;
    m->gen = 0;
    luaL_getmetatable(L, PBWLUA_MESSAGE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* A builder: an empty plain tree of the type, for hot loops that encode
 * far more often than they rebuild (game broadcasts). Build once with
 * set(), then encode() is one core walk plus one buffer malloc. */
static int
pbwlua_message_new(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    pbw_message *tree = pbw_message_new(desc);

    if (tree == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    return pbwlua_push_builder(L, desc, tree);
}

/* Parses input straight into a builder tree, skipping tables: exact for
 * 64-bit integers past 2^53, which tables carry as doubles. */
static int
pbwlua_parse_json(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    pbw_message *msg = NULL;
    pbw_status status;
    pbw_json_parse_options opts;
    pbw_limits storage;
    const pbw_limits *limits = pbwlua_limits_opt(L, 5, &storage);

    opts.ignore_unknown_fields = lua_toboolean(L, 4);
    status = pbw_json_parse_limits(desc, data, len, limits, &opts, &msg);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: parse_json %s: %s", name,
                   pbw_strerror(status));
    }
    return pbwlua_push_builder(L, desc, msg);
}

static int
pbwlua_parse_text(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    pbw_message *msg = NULL;
    pbw_limits storage;
    pbw_status status;

    status = pbw_text_parse_limits(desc, data, len, pbwlua_limits_opt(L, 4, &storage), &msg);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: parse_text %s: %s", name,
                   pbw_strerror(status));
    }
    return pbwlua_push_builder(L, desc, msg);
}

static int
pbwlua_parse_binary(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    pbw_message *msg = NULL;
    pbw_limits storage;
    const pbw_limits *limits = pbwlua_limits_opt(L, 4, &storage);
    pbw_status status;

    status = pbw_decode_limits(desc, data, len, limits, &msg);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: parse_binary %s: %s", name,
                   pbw_strerror(status));
    }
    return pbwlua_push_builder(L, desc, msg);
}

/* Prints a builder's own tree: exact, no table in between. An optional
 * second boolean drops unknown fields (the runner's print_unknown_fields
 * flag); by default they print numerically. */
static int
pbwlua_message_to_json(lua_State *L)
{
    pbwlua_message *m = pbwlua_check_message(L, 1);
    char *text = NULL;
    size_t len = 0;
    pbw_status status = pbw_json_print(m->tree, &text, &len);

    if (status != PBW_OK) {
        luaL_error(L, "pbwire: to_json: %s", pbw_strerror(status));
    }
    lua_pushlstring(L, text, len);
    pbw_free(text);
    return 1;
}

static int
pbwlua_message_to_text(lua_State *L)
{
    pbwlua_message *m = pbwlua_check_message(L, 1);
    /* Absent means print (the binding's standing behavior); the testee
     * passes the runner's print_unknown_fields flag explicitly. */
    int print_unknown = lua_gettop(L) < 2 || lua_toboolean(L, 2);
    char *text = NULL;
    size_t len = 0;
    pbw_status status =
        pbw_text_print_opts(m->tree, print_unknown, &text, &len);

    if (status != PBW_OK) {
        luaL_error(L, "pbwire: to_text: %s", pbw_strerror(status));
    }
    lua_pushlstring(L, text, len);
    pbw_free(text);
    return 1;
}

static int
pbwlua_message_set(lua_State *L)
{
    pbwlua_message *m = pbwlua_check_message(L, 1);

    if (m->readonly) {
        luaL_error(L, "pbwire: message is read-only (decoded into a context)");
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    /* Plain tree: NULL ctx builds malloc values owned by it. */
    pbwlua_fill(L, NULL, m->desc, m->tree, 2, 0);
    lua_pushvalue(L, 1); /* chainable */
    return 1;
}

static int
pbwlua_message_encode(lua_State *L)
{
    pbwlua_message *m = pbwlua_check_message(L, 1);
    const uint8_t *out = NULL;
    size_t len = 0;

    /* Same reused encode buffer as the table path: no malloc per send. */
    pbw_ctx_reset(m->s->ctx);
    {
        pbw_status status = pbw_encode_ctx(m->s->ctx, m->tree, &out, &len);

        if (status != PBW_OK) {
            luaL_error(L, "pbwire: encode: %s", pbw_strerror(status));
        }
    }
    lua_pushlstring(L, (const char *)out, len);
    return 1;
}

static int
pbwlua_message_gc(lua_State *L)
{
    pbwlua_message *m =
        (pbwlua_message *)luaL_checkudata(L, 1, PBWLUA_MESSAGE_MT);

    if (m->tree != NULL) {
        if (m->c == NULL) {
            pbw_message_free(m->tree);
        }
        m->tree = NULL;
    }
    if (m->schema_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, m->schema_ref);
        m->schema_ref = LUA_NOREF;
    }
    if (m->ctx_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, m->ctx_ref);
        m->ctx_ref = LUA_NOREF;
    }
    return 0;
}

/* Optional limits table at `idx` (absent/nil = core defaults): fields
 * max_bytes/max_depth, each 0 for the default. */
static const pbw_limits *
pbwlua_limits_opt(lua_State *L, int idx, pbw_limits *storage)
{
    if (lua_gettop(L) < idx || lua_isnoneornil(L, idx)) {
        return NULL;
    }
    luaL_checktype(L, idx, LUA_TTABLE);
    storage->max_bytes = 0;
    storage->max_depth = 0;
    lua_getfield(L, idx, "max_bytes");
    if (!lua_isnil(L, -1)) {
        lua_Integer n = luaL_checkinteger(L, -1);

        if (n < 0) {
            luaL_error(L, "pbwire: max_bytes must not be negative");
        }
        storage->max_bytes = (size_t)n;
    }
    lua_getfield(L, idx, "max_depth");
    if (!lua_isnil(L, -1)) {
        lua_Integer n = luaL_checkinteger(L, -1);

        if (n < 0) {
            luaL_error(L, "pbwire: max_depth must not be negative");
        }
        /* A width wrap would turn a huge value into a tiny cap; the core
         * clamps anything above PBW_MAX_DEPTH, so saturate there. */
        storage->max_depth = n > (lua_Integer)UINT32_MAX ? UINT32_MAX
                                                         : (uint32_t)n;
    }
    lua_pop(L, 2);
    return storage;
}

/* Builds the table at `idx` into the schema's arena. */
static pbw_message *
pbwlua_build_table(lua_State *L, pbwlua_schema *s, const pbw_descriptor *desc,
                   int idx)
{
    pbw_message *msg;

    pbw_ctx_reset(s->ctx);
    msg = pbw_message_new_ctx(s->ctx, desc);
    if (msg == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    luaL_checktype(L, idx, LUA_TTABLE);
    pbwlua_fill(L, s->ctx, desc, msg, idx, 0);
    return msg;
}

/* Exact number of bytes the wire encoding writes: for callers that must
 * know the length before encoding (a length-prefixed frame, encode_to). */
static int
pbwlua_encoded_size(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    pbw_message *msg = pbwlua_build_table(L, s, desc, 3);
    size_t n = 0;
    pbw_status status = pbw_message_encoded_size(msg, &n);

    if (status != PBW_OK) {
        luaL_error(L, "pbwire: encoded_size %s: %s", name,
                   pbw_strerror(status));
    }
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

/* Encode into a fixed cap: no growth, PBW_ERR_LIMIT when it does not fit. */
static int
pbwlua_encode_cap(lua_State *L, int delimited)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    lua_Integer cap = luaL_checkinteger(L, 4);
    pbw_message *msg = pbwlua_build_table(L, s, desc, 3);
    uint8_t *out = NULL;
    size_t len = 0;
    pbw_status status;

    if (cap < 0) {
        luaL_error(L, "pbwire: encode_to: negative cap");
    }
    out = (uint8_t *)malloc(cap > 0 ? (size_t)cap : 1);
    if (out == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    status = delimited
        ? pbw_encode_delimited_to(msg, out, (size_t)cap, &len)
        : pbw_encode_to(msg, out, (size_t)cap, &len);
    if (status != PBW_OK) {
        free(out);
        if (status == PBW_ERR_LIMIT) {
            luaL_error(L, "pbwire: encode%s %s: buffer too small", delimited ? "_delimited" : "_to", name);
        }
        luaL_error(L, "pbwire: encode%s %s: %s", delimited ? "_delimited" : "_to", name,
                   pbw_strerror(status));
    }
    lua_pushlstring(L, (const char *)out, len);
    free(out);
    return 1;
}

static int
pbwlua_encode_to(lua_State *L)
{
    return pbwlua_encode_cap(L, 0);
}

static int
pbwlua_encode_delimited_to(lua_State *L)
{
    return pbwlua_encode_cap(L, 1);
}

/* Encode and hand the bytes to a function in chunks (default 8 KiB): the
 * callback gets one string per chunk and aborts the encode by returning
 * false (or by raising). shortcut: chunks a full encode instead of
 * streaming from the core; fine until messages outgrow memory. */
static int
pbwlua_encode_sink(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    lua_Integer chunk = luaL_optinteger(L, 5, 8192);
    pbw_message *msg;
    uint8_t *out = NULL;
    size_t len = 0;
    size_t off = 0;
    pbw_status status;

    luaL_checktype(L, 3, LUA_TTABLE);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    if (chunk <= 0) {
        luaL_error(L, "pbwire: encode_sink: chunk must be positive");
    }
    msg = pbwlua_build_table(L, s, desc, 3);
    status = pbw_encode(msg, &out, &len);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: encode_sink %s: %s", name,
                   pbw_strerror(status));
    }
    for (off = 0; off < len || (len == 0 && off == 0); off += (size_t)chunk) {
        size_t n = len - off < (size_t)chunk ? len - off : (size_t)chunk;

        lua_pushvalue(L, 4);
        lua_pushlstring(L, (const char *)out + off, n);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            /* Keep the Lua error on top, drop the encode buffer. */
            free(out);
            out = NULL;
            lua_error(L);
        }
        if (!lua_toboolean(L, -1)) {
            lua_pop(L, 1);
            free(out);
            luaL_error(L, "pbwire: encode_sink: sink aborted");
        }
        lua_pop(L, 1);
        if (len == 0) {
            break;
        }
    }
    free(out);
    return 0;
}

/* One scanned field: raw wire content, no allocation, no value tree. */
struct pbwlua_event {
    uint32_t number;
    uint32_t wire_type;
    uint64_t u64;
    const char *data; /* into the input string; copied out before return */
    size_t len;
};

struct pbwlua_visit_state {
    struct pbwlua_event *events;
    size_t n;
    size_t cap;
    pbw_status status;
};

static pbw_status
pbwlua_visit_cb(void *ud, const pbw_field *field, uint32_t number,
                uint32_t wire_type, const pbw_visit_value *value)
{
    struct pbwlua_visit_state *st = (struct pbwlua_visit_state *)ud;
    struct pbwlua_event *ev;

    (void)field;
    if (st->status != PBW_OK) {
        return st->status;
    }
    if (st->n == st->cap) {
        size_t ncap = st->cap != 0 ? st->cap * 2 : 32;
        struct pbwlua_event *grown = (struct pbwlua_event *)realloc(
            st->events, ncap * sizeof(*grown));

        if (grown == NULL) {
            st->status = PBW_ERR_NOMEM;
            return PBW_ERR_NOMEM;
        }
        st->events = grown;
        st->cap = ncap;
    }
    ev = &st->events[st->n++];
    ev->number = number;
    ev->wire_type = wire_type;
    ev->u64 = value->u64;
    ev->data = (const char *)value->data;
    ev->len = value->len;
    return PBW_OK;
}

/* Single-level scan of wire bytes: an array of {number=, wire_type=,
 * value=} with no message tree. Scalars arrive as numbers (varints
 * unfolded), LEN payloads and whole group fields as strings. */
static int
pbwlua_visit(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    pbw_limits storage;
    const pbw_limits *limits = pbwlua_limits_opt(L, 4, &storage);
    struct pbwlua_visit_state st;
    size_t fail_off = 0;
    size_t i;
    pbw_status status;

    memset(&st, 0, sizeof(st));
    status = pbw_visit_limits(desc, data, len, limits, pbwlua_visit_cb, &st,
                              &fail_off);
    if (status != PBW_OK) {
        free(st.events);
        luaL_error(L, "pbwire: visit %s @%lu: %s", name,
                   (unsigned long)fail_off, pbw_strerror(status));
    }
    lua_createtable(L, (int)st.n, 0);
    for (i = 0; i < st.n; i++) {
        struct pbwlua_event *ev = &st.events[i];

        lua_createtable(L, 0, 3);
        lua_pushinteger(L, (lua_Integer)ev->number);
        lua_setfield(L, -2, "number");
        lua_pushinteger(L, (lua_Integer)ev->wire_type);
        lua_setfield(L, -2, "wire_type");
        if (ev->wire_type == 2 || ev->wire_type == 3) {
            lua_pushlstring(L, ev->data != NULL ? ev->data : "", ev->len);
        } else {
            pbwlua_push_uint(L, ev->u64);
        }
        lua_setfield(L, -2, "value");
        lua_rawseti(L, -2, (int)(i + 1));
    }
    free(st.events);
    return 1;
}

/* Loaded service names, for routing. */
static int
pbwlua_services(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    size_t n = pbw_schema_service_count(s->schema);
    size_t i;

    lua_createtable(L, (int)n, 0);
    for (i = 0; i < n; i++) {
        const pbw_service *svc = pbw_schema_service_at(s->schema, i);

        lua_pushstring(L, pbw_service_name(svc));
        lua_rawseti(L, -2, (int)(i + 1));
    }
    return 1;
}

static void
pbwlua_push_method(lua_State *L, const pbw_method *m)
{
    lua_createtable(L, 0, 5);
    lua_pushstring(L, pbw_method_name(m));
    lua_setfield(L, -2, "name");
    lua_pushstring(L, pbw_method_input_type(m));
    lua_setfield(L, -2, "input_type");
    lua_pushstring(L, pbw_method_output_type(m));
    lua_setfield(L, -2, "output_type");
    lua_pushboolean(L, pbw_method_client_streaming(m));
    lua_setfield(L, -2, "client_streaming");
    lua_pushboolean(L, pbw_method_server_streaming(m));
    lua_setfield(L, -2, "server_streaming");
}

/* One service and its methods. */
static int
pbwlua_service(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_service *svc = pbw_schema_service_find(s->schema, name);
    size_t n;
    size_t i;

    if (svc == NULL) {
        luaL_error(L, "pbwire: no service %s in the schema", name);
    }
    n = pbw_service_method_count(svc);
    lua_createtable(L, 0, 2);
    lua_pushstring(L, pbw_service_name(svc));
    lua_setfield(L, -2, "name");
    lua_createtable(L, (int)n, 0);
    for (i = 0; i < n; i++) {
        pbwlua_push_method(L, pbw_service_method_at(svc, i));
        lua_rawseti(L, -2, (int)(i + 1));
    }
    lua_setfield(L, -2, "methods");
    return 1;
}

/* Resolve a method to its input/output message names for routing. */
static int
pbwlua_resolve(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *svc = luaL_checkstring(L, 2);
    const char *method = luaL_checkstring(L, 3);
    const pbw_descriptor *in_desc = NULL;
    const pbw_descriptor *out_desc = NULL;
    pbw_status status =
        pbw_service_resolve(s->schema, svc, method, &in_desc, &out_desc);

    if (status != PBW_OK) {
        luaL_error(L, "pbwire: resolve %s/%s: %s", svc, method,
                   pbw_strerror(status));
    }
    lua_createtable(L, 0, 2);
    lua_pushstring(L, pbw_descriptor_name(in_desc));
    lua_setfield(L, -2, "input");
    lua_pushstring(L, pbw_descriptor_name(out_desc));
    lua_setfield(L, -2, "output");
    return 1;
}

/* Pack already-encoded payload into an Any tree. */
static int
pbwlua_any_pack(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    size_t url_len = 0;
    size_t pay_len = 0;
    const char *type_url = luaL_checklstring(L, 3, &url_len);
    const char *payload = luaL_checklstring(L, 4, &pay_len);
    pbw_message *msg = NULL;
    /* type_url is NUL-terminated by the push; an interior NUL would
     * truncate it on the way to C. */
    size_t i;
    pbw_status status;

    for (i = 0; i < url_len; i++) {
        if (type_url[i] == '\0') {
            luaL_error(L, "pbwire: any_pack: NUL in type_url");
        }
    }
    status = pbw_any_pack(desc, type_url, payload, pay_len, &msg);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: any_pack %s: %s", name,
                   pbw_strerror(status));
    }
    return pbwlua_push_builder(L, desc, msg);
}

/* A Timestamp/Duration tree holding ms. */
static int
pbwlua_time_from_millis(lua_State *L, int duration)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    int64_t ms = (int64_t)luaL_checkinteger(L, 3);
    pbw_message *msg = NULL;
    pbw_status status = duration
        ? pbw_duration_from_millis(desc, ms, &msg)
        : pbw_timestamp_from_millis(desc, ms, &msg);

    if (status != PBW_OK) {
        luaL_error(L, "pbwire: %s_from_millis %s: %s",
                   duration ? "duration" : "timestamp", name,
                   pbw_strerror(status));
    }
    return pbwlua_push_builder(L, desc, msg);
}

static int
pbwlua_timestamp_from_millis(lua_State *L)
{
    return pbwlua_time_from_millis(L, 0);
}

static int
pbwlua_duration_from_millis(lua_State *L)
{
    return pbwlua_time_from_millis(L, 1);
}

/* Exact bytes a builder encodes to. */
static int
pbwlua_message_encoded_size(lua_State *L)
{
    pbwlua_message *m = pbwlua_check_message(L, 1);
    size_t n = 0;
    pbw_status status = pbw_message_encoded_size(m->tree, &n);

    if (status != PBW_OK) {
        luaL_error(L, "pbwire: encoded_size: %s", pbw_strerror(status));
    }
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int
pbwlua_message_any_type_url(lua_State *L)
{
    pbwlua_message *m = pbwlua_check_message(L, 1);
    const char *url = pbw_any_type_url(m->tree);

    if (url == NULL) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, url);
    }
    return 1;
}

static int
pbwlua_message_any_payload(lua_State *L)
{
    pbwlua_message *m = pbwlua_check_message(L, 1);
    size_t len = 0;
    const void *data = pbw_any_payload(m->tree, &len);

    if (data == NULL) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, (const char *)data, len);
    }
    return 1;
}

static int
pbwlua_message_time_millis(lua_State *L, int duration)
{
    pbwlua_message *m = pbwlua_check_message(L, 1);
    int64_t ms = 0;
    pbw_status status = duration
        ? pbw_duration_to_millis(m->tree, &ms)
        : pbw_timestamp_to_millis(m->tree, &ms);

    if (status != PBW_OK) {
        luaL_error(L, "pbwire: %s_millis: %s",
                   duration ? "duration" : "timestamp",
                   pbw_strerror(status));
    }
    lua_pushinteger(L, (lua_Integer)ms);
    return 1;
}

static int
pbwlua_message_timestamp_millis(lua_State *L)
{
    return pbwlua_message_time_millis(L, 0);
}

static int
pbwlua_message_duration_millis(lua_State *L)
{
    return pbwlua_message_time_millis(L, 1);
}

/* A FieldMask's paths without materializing the list. */
static int
pbwlua_message_fieldmask_paths(lua_State *L)
{
    pbwlua_message *m = pbwlua_check_message(L, 1);
    size_t n = pbw_fieldmask_path_count(m->tree);
    size_t i;

    lua_createtable(L, (int)n, 0);
    for (i = 0; i < n; i++) {
        const char *path = pbw_fieldmask_path_at(m->tree, i);

        lua_pushstring(L, path != NULL ? path : "");
        lua_rawseti(L, -2, (int)(i + 1));
    }
    return 1;
}

#define PBWLUA_POOL_MT "PbwirePool"
#define PBWLUA_CONTEXT_MT "PbwireContext"

typedef struct {
    pbw_pool *pool;
} pbwlua_pool;

static pbwlua_context *
pbwlua_check_context(lua_State *L, int index)
{
    pbwlua_context *c =
        (pbwlua_context *)luaL_checkudata(L, index, PBWLUA_CONTEXT_MT);

    if (c->ctx == NULL || c->released) {
        luaL_error(L, "pbwire: context was released");
    }
    return c;
}

static int
pbwlua_new_pool(lua_State *L)
{
    size_t max_idle = (size_t)luaL_optinteger(L, 1, 0);
    size_t retain_cap = (size_t)luaL_optinteger(L, 2, 0);
    pbwlua_pool *p =
        (pbwlua_pool *)lua_newuserdata(L, sizeof(*p));

    p->pool = pbw_pool_new(max_idle, retain_cap);
    if (p->pool == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    luaL_getmetatable(L, PBWLUA_POOL_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int
pbwlua_pool_gc(lua_State *L)
{
    pbwlua_pool *p =
        (pbwlua_pool *)luaL_checkudata(L, 1, PBWLUA_POOL_MT);

    if (p->pool != NULL) {
        pbw_pool_free(p->pool);
        p->pool = NULL;
    }
    return 0;
}

/* Exclusive use until release; the context is already reset. */
static int
pbwlua_pool_acquire(lua_State *L)
{
    pbwlua_pool *p =
        (pbwlua_pool *)luaL_checkudata(L, 1, PBWLUA_POOL_MT);
    pbw_ctx *ctx;
    pbwlua_context *c;

    if (p->pool == NULL) {
        luaL_error(L, "pbwire: pool is closed");
    }
    ctx = pbw_pool_acquire(p->pool);
    if (ctx == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    c = (pbwlua_context *)lua_newuserdata(L, sizeof(*c));
    c->ctx = ctx;
    c->pool = p->pool;
    c->pool_ref = LUA_NOREF;
    c->released = 0;
    c->gen = 0;
    c->retain_cap = 0;
    lua_pushvalue(L, 1);
    c->pool_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    luaL_getmetatable(L, PBWLUA_CONTEXT_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int
pbwlua_pool_release(lua_State *L)
{
    pbwlua_pool *p =
        (pbwlua_pool *)luaL_checkudata(L, 1, PBWLUA_POOL_MT);
    pbwlua_context *c =
        (pbwlua_context *)luaL_checkudata(L, 2, PBWLUA_CONTEXT_MT);

    if (p->pool == NULL) {
        luaL_error(L, "pbwire: pool is closed");
    }
    if (c->released || c->pool != p->pool) {
        luaL_error(L, "pbwire: context does not belong to this pool");
    }
    pbw_pool_release(p->pool, c->ctx);
    c->released = 1;
    c->gen++;
    return 0;
}

static int
pbwlua_pool_idle(lua_State *L)
{
    pbwlua_pool *p =
        (pbwlua_pool *)luaL_checkudata(L, 1, PBWLUA_POOL_MT);

    if (p->pool == NULL) {
        luaL_error(L, "pbwire: pool is closed");
    }
    lua_pushinteger(L, (lua_Integer)pbw_pool_idle(p->pool));
    return 1;
}

static int
pbwlua_new_context(lua_State *L)
{
    pbwlua_context *c =
        (pbwlua_context *)lua_newuserdata(L, sizeof(*c));

    c->ctx = pbw_ctx_new();
    if (c->ctx == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    c->pool = NULL;
    c->pool_ref = LUA_NOREF;
    c->released = 0;
    c->gen = 0;
    c->retain_cap = 0;
    luaL_getmetatable(L, PBWLUA_CONTEXT_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int
pbwlua_context_gc(lua_State *L)
{
    pbwlua_context *c =
        (pbwlua_context *)luaL_checkudata(L, 1, PBWLUA_CONTEXT_MT);

    if (c->ctx != NULL) {
        if (c->pool != NULL && !c->released) {
            /* Collected while acquired: hand it back so the pool's
             * freelist never dangles. */
            pbw_pool_release(c->pool, c->ctx);
        } else if (c->pool == NULL) {
            pbw_ctx_free(c->ctx);
        }
        c->ctx = NULL;
    }
    if (c->pool_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, c->pool_ref);
        c->pool_ref = LUA_NOREF;
    }
    return 0;
}

/* Release every allocation since the last reset, keeping the buffers.
 * Bumps the generation: outstanding decoded trees raise on next use. */
static int
pbwlua_context_reset(lua_State *L)
{
    pbwlua_context *c = pbwlua_check_context(L, 1);

    pbw_ctx_reset(c->ctx);
    c->gen++;
    return 0;
}

static int
pbwlua_context_used(lua_State *L)
{
    pbwlua_context *c = pbwlua_check_context(L, 1);

    lua_pushinteger(L, (lua_Integer)pbw_ctx_used(c->ctx));
    return 1;
}

/* Retain cap in bytes (0 keeps everything, like the core default): the
 * core only takes a setter, so the getter reads back what was set. */
static int
pbwlua_context_retain_cap(lua_State *L)
{
    pbwlua_context *c = pbwlua_check_context(L, 1);

    if (lua_gettop(L) >= 2) {
        c->retain_cap = (size_t)luaL_checkinteger(L, 2);
        pbw_ctx_set_retain_cap(c->ctx, c->retain_cap);
        return 0;
    }
    lua_pushinteger(L, (lua_Integer)c->retain_cap);
    return 1;
}

/* Decode into the arena: the builder borrows the context (read-only, dies
 * with reset). The schema and the context stay anchored until __gc. */
static int
pbwlua_context_decode(lua_State *L)
{
    pbwlua_context *c = pbwlua_check_context(L, 1);
    pbwlua_schema *s = pbwlua_check_schema(L, 2);
    const char *name = luaL_checkstring(L, 3);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 4, &len);
    pbw_limits storage;
    const pbw_limits *limits = pbwlua_limits_opt(L, 5, &storage);
    pbw_message *msg = NULL;
    pbwlua_message *m;
    pbw_status status;

    status = pbw_decode_ctx_limits(c->ctx, desc, data, len, limits, &msg);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: decode %s: %s", name, pbw_strerror(status));
    }
    m = (pbwlua_message *)lua_newuserdata(L, sizeof(*m));
    m->desc = desc;
    m->tree = msg;
    m->s = s;
    m->schema_ref = LUA_NOREF;
    m->ctx_ref = LUA_NOREF;
    m->c = c;
    m->gen = c->gen;
    m->readonly = 1;
    lua_pushvalue(L, 2);
    m->schema_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, 1);
    m->ctx_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    luaL_getmetatable(L, PBWLUA_MESSAGE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* Encode a table through the context's buffer. */
static int
pbwlua_context_encode(lua_State *L)
{
    pbwlua_context *c = pbwlua_check_context(L, 1);
    pbwlua_schema *s = pbwlua_check_schema(L, 2);
    const char *name = luaL_checkstring(L, 3);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    pbw_message *msg;
    const uint8_t *out = NULL;
    size_t len = 0;
    pbw_status status;

    luaL_checktype(L, 4, LUA_TTABLE);
    msg = pbw_message_new_ctx(c->ctx, desc);
    if (msg == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    pbwlua_fill(L, c->ctx, desc, msg, 4, 0);
    status = pbw_encode_ctx(c->ctx, msg, &out, &len);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: encode %s: %s", name, pbw_strerror(status));
    }
    lua_pushlstring(L, (const char *)out, len);
    return 1;
}

static const luaL_Reg pbwlua_message_methods[] = {
    {"set", pbwlua_message_set},
    {"encode", pbwlua_message_encode},
    {"to_json", pbwlua_message_to_json},
    {"to_text", pbwlua_message_to_text},
    {"encoded_size", pbwlua_message_encoded_size},
    {"any_type_url", pbwlua_message_any_type_url},
    {"any_payload", pbwlua_message_any_payload},
    {"timestamp_millis", pbwlua_message_timestamp_millis},
    {"duration_millis", pbwlua_message_duration_millis},
    {"fieldmask_paths", pbwlua_message_fieldmask_paths},
    {NULL, NULL},
};

static int
pbwlua_gc(lua_State *L)
{
    pbwlua_schema *s =
        (pbwlua_schema *)luaL_checkudata(L, 1, PBWLUA_SCHEMA_MT);

    if (s->schema != NULL) {
        pbw_schema_free(s->schema);
        s->schema = NULL;
    }
    if (s->ctx != NULL) {
        pbw_ctx_free(s->ctx);
        s->ctx = NULL;
    }
    free(s->wdata);
    s->wdata = NULL;
    s->wlen = 0;
    s->wcap = 0;
    {
        struct pbwlua_namecache *c = s->ncache;
        while (c != NULL) {
            struct pbwlua_namecache *next = c->next;
            size_t k;
            for (k = 0; k < c->n; k++) {
                if (c->f[k].ref != LUA_NOREF) {
                    luaL_unref(L, LUA_REGISTRYINDEX, c->f[k].ref);
                }
            }
            free(c->f);
            free(c->idx);
            free(c);
            c = next;
        }
        s->ncache = NULL;
    }
    return 0;
}

static const luaL_Reg pbwlua_schema_methods[] = {
    {"encode", pbwlua_encode},
    {"decode", pbwlua_decode},
    {"roundtrip", pbwlua_roundtrip},
    {"message", pbwlua_message_new},
    {"to_json", pbwlua_to_json},
    {"from_json", pbwlua_from_json},
    {"to_text", pbwlua_to_text},
    {"from_text", pbwlua_from_text},
    {"encode_deterministic", pbwlua_encode_deterministic},
    {"encode_delimited", pbwlua_encode_delimited},
    {"encode_delimited_to", pbwlua_encode_delimited_to},
    {"decode_delimited", pbwlua_decode_delimited},
    {"encoded_size", pbwlua_encoded_size},
    {"encode_to", pbwlua_encode_to},
    {"encode_sink", pbwlua_encode_sink},
    {"visit", pbwlua_visit},
    {"services", pbwlua_services},
    {"service", pbwlua_service},
    {"resolve", pbwlua_resolve},
    {"any_pack", pbwlua_any_pack},
    {"timestamp_from_millis", pbwlua_timestamp_from_millis},
    {"duration_from_millis", pbwlua_duration_from_millis},
    {"parse_json", pbwlua_parse_json},
    {"parse_text", pbwlua_parse_text},
    {"parse_binary", pbwlua_parse_binary},
    {NULL, NULL},
};

static const luaL_Reg pbwlua_pool_methods[] = {
    {"acquire", pbwlua_pool_acquire},
    {"release", pbwlua_pool_release},
    {"idle", pbwlua_pool_idle},
    {NULL, NULL},
};

static const luaL_Reg pbwlua_context_methods[] = {
    {"decode", pbwlua_context_decode},
    {"encode", pbwlua_context_encode},
    {"reset", pbwlua_context_reset},
    {"used", pbwlua_context_used},
    {"retain_cap", pbwlua_context_retain_cap},
    {NULL, NULL},
};

static const luaL_Reg pbwlua_functions[] = {
    {"load", pbwlua_load_bytes},
    {"loadfile", pbwlua_load_file},
    {"new_pool", pbwlua_new_pool},
    {"new_context", pbwlua_new_context},
    {NULL, NULL},
};

#if LUA_VERSION_NUM >= 502
int
luaopen_pbwire(lua_State *L)
{
    luaL_newmetatable(L, PBWLUA_SCHEMA_MT);
    lua_pushcfunction(L, pbwlua_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, pbwlua_schema_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_newmetatable(L, PBWLUA_MESSAGE_MT);
    lua_pushcfunction(L, pbwlua_message_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, pbwlua_message_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_newmetatable(L, PBWLUA_POOL_MT);
    lua_pushcfunction(L, pbwlua_pool_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, pbwlua_pool_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_newmetatable(L, PBWLUA_CONTEXT_MT);
    lua_pushcfunction(L, pbwlua_context_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, pbwlua_context_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_newlib(L, pbwlua_functions);
    return 1;
}
#else
int
luaopen_pbwire(lua_State *L)
{
    luaL_newmetatable(L, PBWLUA_SCHEMA_MT);
    lua_pushcfunction(L, pbwlua_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_register(L, NULL, pbwlua_schema_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_newmetatable(L, PBWLUA_MESSAGE_MT);
    lua_pushcfunction(L, pbwlua_message_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_register(L, NULL, pbwlua_message_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_newmetatable(L, PBWLUA_POOL_MT);
    lua_pushcfunction(L, pbwlua_pool_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_register(L, NULL, pbwlua_pool_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_newmetatable(L, PBWLUA_CONTEXT_MT);
    lua_pushcfunction(L, pbwlua_context_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_register(L, NULL, pbwlua_context_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_register(L, "pbwire", pbwlua_functions);
    return 1;
}
#endif
