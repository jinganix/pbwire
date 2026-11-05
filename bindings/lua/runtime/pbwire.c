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
} pbwlua_schema;

/* A builder owns a plain malloc tree. It anchors its schema with a registry
 * reference (released by __gc): pbw_message_free() reads the descriptor, so
 * the schema must outlive every builder. */
typedef struct {
    const pbw_descriptor *desc;
    pbw_message *tree;
    int schema_ref;
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

static void pbwlua_push_value(lua_State *L, const pbw_field *field,
                              const pbw_value *value, unsigned depth);

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

static void
pbwlua_push_message(lua_State *L, const pbw_message *msg, unsigned depth)
{
    size_t i;

    if (depth > PBWLUA_MAX_DEPTH) {
        luaL_error(L, "pbwire: nesting limit exceeded");
    }
    /* Same guarantee as the fill side: one table plus one value per level
     * with room for the nested call beneath. */
    luaL_checkstack(L, 6, "pbwire: message too deep");
    lua_newtable(L);
    for (i = 0;; i++) {
        pbw_value *value = NULL;
        const pbw_field *field = pbw_message_field_at(msg, i, &value);

        if (field == NULL) {
            break;
        }
        if (pbw_value_get_kind(value) == PBW_VALUE_LIST) {
            size_t count = pbw_value_count(value);
            size_t k;

            /* Exact array size: no rehash while filling. */
            lua_createtable(L, (int)count, 0);
            for (k = 0; k < count; k++) {
                pbwlua_push_value(L, field, pbw_value_at(value, k),
                                  depth + 1);
                lua_rawseti(L, -2, (int)k + 1);
            }
        } else {
            pbwlua_push_value(L, field, value, depth + 1);
        }
        lua_setfield(L, -2, pbw_field_name(field));
    }
    /* An absent repeated field reads as an empty table, like the other
     * bindings (Ruby reads []): game code can always #/ipairs it. */
    {
        const pbw_descriptor *desc = pbw_message_descriptor(msg);
        size_t nfields = pbw_descriptor_field_count(desc);
        size_t f;

        for (f = 0; f < nfields; f++) {
            const pbw_field *field = pbw_descriptor_field_at(desc, f);

            if (pbw_field_label(field) == PBW_LABEL_REPEATED &&
                pbw_message_get(msg, pbw_field_number(field)) == NULL) {
                lua_newtable(L);
                lua_setfield(L, -2, pbw_field_name(field));
            }
        }
    }
}

static void
pbwlua_push_value(lua_State *L, const pbw_field *field,
                  const pbw_value *value, unsigned depth)
{
    if (pbw_value_get_kind(value) == PBW_VALUE_MESSAGE) {
        pbwlua_push_message(L, pbw_value_get_message(value), depth);
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
    pbw_status status;
    pbwlua_schema *s;

    status = pbw_descriptor_set_load(data, len, &schema);
    if (status != PBW_OK) {
        const char *reason = schema != NULL ? pbw_schema_error(schema) : "";
        pbw_schema_free(schema);
        luaL_error(L, "pbwire: descriptor set rejected: %s",
                   reason != NULL && reason[0] != '\0'
                       ? reason
                       : pbw_strerror(status));
    }
    s = (pbwlua_schema *)lua_newuserdata(L, sizeof(*s));
    s->schema = schema;
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

static int
pbwlua_encode(lua_State *L)
{
    pbwlua_schema *s = pbwlua_check_schema(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const pbw_descriptor *desc = pbwlua_find(L, s->schema, name);
    /* Reset first: the arena holds the previous call's tree, the buffer
     * the previous encoding. One reset covers both. */
    pbw_message *msg;
    const uint8_t *out = NULL;
    size_t len = 0;
    pbw_status status;

    pbw_ctx_reset(s->ctx);
    msg = pbw_message_new_ctx(s->ctx, desc);
    if (msg == NULL) {
        luaL_error(L, "pbwire: out of memory");
    }
    luaL_checktype(L, 3, LUA_TTABLE);
    pbwlua_fill(L, s->ctx, desc, msg, 3, 0);
    /* Reuses the context's buffer: steady use stops calling malloc. */
    status = pbw_encode_ctx(s->ctx, msg, &out, &len);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: encode %s: %s", name, pbw_strerror(status));
    }
    lua_pushlstring(L, (const char *)out, len);
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

    /* Arena decode: one bump allocator, no per-value malloc. The tree is
     * read-only and dies with the next reset; conversion below only reads. */
    pbw_ctx_reset(s->ctx);
    status = pbw_decode_ctx(s->ctx, desc, data, len, &msg);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: decode %s: %s", name, pbw_strerror(status));
    }
    pbwlua_push_message(L, msg, 0);
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
    pbw_status status;
    pbw_json_parse_options opts;

    opts.ignore_unknown_fields = lua_toboolean(L, 4);
    status = pbw_json_parse_opts(desc, data, len, &opts, &msg);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: from_json %s: %s", name, pbw_strerror(status));
    }
    pbwlua_push_message(L, msg, 0);
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
    pbw_status status;

    status = pbw_text_parse(desc, data, len, &msg);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: from_text %s: %s", name, pbw_strerror(status));
    }
    pbwlua_push_message(L, msg, 0);
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
    size_t consumed = 0;
    pbw_status status;

    status = pbw_decode_delimited(desc, data, len, &msg, &consumed);
    if (status != PBW_OK) {
        luaL_error(L, "pbwire: decode_delimited %s: %s", name,
                   pbw_strerror(status));
    }
    pbwlua_push_message(L, msg, 0);
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
    /* Anchor the schema: the tree reads its descriptor until __gc. */
    lua_pushvalue(L, 1);
    m->schema_ref = luaL_ref(L, LUA_REGISTRYINDEX);
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

    opts.ignore_unknown_fields = lua_toboolean(L, 4);
    status = pbw_json_parse_opts(desc, data, len, &opts, &msg);
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
    pbw_status status;

    status = pbw_text_parse(desc, data, len, &msg);
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
    pbw_status status;

    status = pbw_decode(desc, data, len, &msg);
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
    uint8_t *out = NULL;
    size_t len = 0;
    pbw_status status = pbw_encode(m->tree, &out, &len);

    if (status != PBW_OK) {
        luaL_error(L, "pbwire: encode: %s", pbw_strerror(status));
    }
    lua_pushlstring(L, (const char *)out, len);
    pbw_free(out);
    return 1;
}

static int
pbwlua_message_gc(lua_State *L)
{
    pbwlua_message *m =
        (pbwlua_message *)luaL_checkudata(L, 1, PBWLUA_MESSAGE_MT);

    if (m->tree != NULL) {
        pbw_message_free(m->tree);
        m->tree = NULL;
    }
    if (m->schema_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, m->schema_ref);
        m->schema_ref = LUA_NOREF;
    }
    return 0;
}

static const luaL_Reg pbwlua_message_methods[] = {
    {"set", pbwlua_message_set},
    {"encode", pbwlua_message_encode},
    {"to_json", pbwlua_message_to_json},
    {"to_text", pbwlua_message_to_text},
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
    {"decode_delimited", pbwlua_decode_delimited},
    {"parse_json", pbwlua_parse_json},
    {"parse_text", pbwlua_parse_text},
    {"parse_binary", pbwlua_parse_binary},
    {NULL, NULL},
};

static const luaL_Reg pbwlua_functions[] = {
    {"load", pbwlua_load_bytes},
    {"loadfile", pbwlua_load_file},
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
    luaL_register(L, "pbwire", pbwlua_functions);
    return 1;
}
#endif
