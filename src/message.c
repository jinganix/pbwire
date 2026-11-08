/* Dynamic message value tree: allocation, accessors, field storage, oneof
 * handling. */

#include "internal.h"

pbw_message *
pbw_message_new(const pbw_descriptor *desc)
{
    return pbw_message_new_a(NULL, desc);
}

/* Releases the payloads of a message without touching the message struct or its
 * arena: used both for heap messages (which are then freed) and for messages
 * that live inside an arena, where the struct itself is not individually
 * freeable. */
static void
message_release(pbw_message *msg)
{
    size_t i;

    /* nfields, not desc->field_count: freeing after pbw_schema_free must
     * not touch the freed descriptor. */
    for (i = 0; i < msg->nfields; i++) {
        if (pbw_msg_present(msg, i)) {
            pbw_value_destroy(&msg->values[i]);
        }
    }
    if (msg->arena == NULL) {
        /* Heap message: the unknown bytes and the struct are ours to free. In an
         * arena the unknown bytes are arena memory, released with the arena. */
        free(msg->unknown);
        free(msg);
    }
}

void
pbw_message_free(pbw_message *msg)
{
    if (msg == NULL) {
        return;
    }
    if (msg->own != NULL) {
        /* Root of a decoded tree: when nothing heap was stored into it
         * (the pure decode fast path, including every bench decode), the
         * whole tree lives in the arena and a bulk free is enough. Only a
         * mutated tree needs the slot walk to release heap payloads. */
        pbw_arena *own = msg->own;
        int packed = own->block != NULL;

        if (own->has_heap) {
            message_release(msg);
        }
        pbw_arena_free(own);
        if (!packed) {
            free(own);
        }
        return;
    }
    if (msg->arena != NULL) {
        /* A submessage of an arena tree dies with that tree (or context). */
        return;
    }
    message_release(msg);
}

const pbw_descriptor *
pbw_message_descriptor(const pbw_message *msg)
{
    return msg == NULL ? NULL : msg->desc;
}

size_t
pbw_message_wire_size(const pbw_message *msg)
{
    return msg == NULL ? 0 : msg->wire_len;
}

int
pbw_message_add_unknown(pbw_message *msg, const uint8_t *data, size_t len)
{
    size_t need;
    uint8_t *grown;

    if (msg == NULL) {
        return PBW_ERR_INVALID;
    }
    if (len == 0) {
        return PBW_OK;
    }
    if (data == NULL) {
        return PBW_ERR_INVALID;
    }
    if (msg->unknown_len > SIZE_MAX - len) {
        return PBW_ERR_NOMEM;
    }
    need = msg->unknown_len + len;
    if (!pbw_cap_holds(msg->unknown_cap, msg->unknown_len)) {
        return PBW_ERR_NOMEM;
    }
    if (need > msg->unknown_cap) {
        size_t cap = msg->unknown_cap == 0 ? 64 : msg->unknown_cap;

        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        if (msg->arena != NULL) {
            if (msg->unknown == NULL && msg->unknown_len > 0) {
                return PBW_ERR_NOMEM;
            }
            grown = (uint8_t *)pbw_arena_alloc(msg->arena, cap);
            if (grown != NULL && msg->unknown_len > 0) {
                memcpy(grown, msg->unknown, msg->unknown_len);
            }
        } else {
            grown = (uint8_t *)realloc(msg->unknown, cap);
        }
        if (grown == NULL) {
            return PBW_ERR_NOMEM;
        }
        msg->unknown = grown;
        msg->unknown_cap = cap;
    }
    memcpy(msg->unknown + msg->unknown_len, data, len);
    msg->unknown_len = need;
    return PBW_OK;
}

static size_t
value_field_index(const pbw_message *msg, const pbw_field *field)
{
    return (size_t)(field - msg->desc->fields);
}

pbw_value *
pbw_message_get(const pbw_message *msg, uint32_t number)
{
    const pbw_field *field;
    size_t index;

    if (msg == NULL) {
        return NULL;
    }
    field = pbw_descriptor_find_field(msg->desc, number);
    if (field == NULL) {
        return NULL;
    }
    index = value_field_index(msg, field);
    return pbw_msg_present(msg, index) ? &msg->values[index] : NULL;
}

pbw_value *
pbw_message_get_field(const pbw_message *msg, const pbw_field *field)
{
    const pbw_field *base;
    size_t index;

    if (msg == NULL || msg->desc == NULL || field == NULL) {
        return NULL;
    }
    base = msg->desc->fields;
    if (field < base || field >= base + msg->desc->field_count) {
        return NULL;
    }
    index = (size_t)(field - base);
    return pbw_msg_present(msg, index) ? &msg->values[index] : NULL;
}

static int
is_initialized(const pbw_message *msg, uint32_t depth)
{
    size_t i;

    /* Read `>` like the decoder: a message at depth PBW_MAX_DEPTH is
     * accepted on the wire, so the required-field walk must judge it too. */
    if (msg == NULL || msg->desc == NULL || depth > PBW_MAX_DEPTH) {
        return 0;
    }
    /* The flag is computed by pbw_schema_link(); an unlinked descriptor is
     * unsupported for messages already (its fields are still unsorted), so
     * only a linked tree may take the shortcut. */
    if (!msg->desc->has_required && (msg->desc->schema == NULL ||
                                     msg->desc->schema->linked)) {
        return 1; /* no required field in this subtree */
    }
    for (i = 0; i < msg->desc->field_count; i++) {
        const pbw_field *field = &msg->desc->fields[i];
        const pbw_value *value = &msg->values[i];

        if (!pbw_msg_present(msg, i)) {
            if (pbw_field_is_required(field, msg->desc)) {
                return 0;
            }
            continue;
        }
        if (field->type != PBW_TYPE_MESSAGE && field->type != PBW_TYPE_GROUP) {
            continue;
        }
        if (field->label == PBW_LABEL_REPEATED) {
            size_t j;

            if (value->kind != PBW_VALUE_LIST) {
                continue;
            }
            for (j = 0; j < value->as.list.len; j++) {
                if (!is_initialized(value->as.list.items[j]->as.msg,
                                    depth + 1)) {
                    return 0;
                }
            }
            continue;
        }
        if (value->kind != PBW_VALUE_MESSAGE ||
            !is_initialized(value->as.msg, depth + 1)) {
            return 0;
        }
    }
    return 1;
}

int
pbw_message_is_initialized(const pbw_message *msg)
{
    return is_initialized(msg, 0);
}

size_t
pbw_message_present_list(const pbw_message *msg, uint32_t *fnums,
                         const pbw_value **values, size_t cap)
{
    size_t i;
    size_t n = 0;

    if (msg == NULL || msg->desc == NULL || fnums == NULL || values == NULL) {
        return 0;
    }
    for (i = 0; i < msg->desc->field_count && n < cap; i++) {
        if (pbw_msg_present(msg, i)) {
            fnums[n] = msg->desc->fields[i].number;
            values[n] = &msg->values[i];
            n++;
        }
    }
    return n;
}

size_t
pbw_message_field_count(const pbw_message *msg)
{
    size_t i;
    size_t count = 0;

    if (msg == NULL) {
        return 0;
    }
    for (i = 0; i < msg->desc->field_count; i++) {
        count += (size_t)pbw_msg_present(msg, i);
    }
    return count;
}

const pbw_field *
pbw_message_field_at(const pbw_message *msg, size_t index,
                     pbw_value **value)
{
    size_t i;
    size_t seen = 0;

    if (msg == NULL) {
        return NULL;
    }
    for (i = 0; i < msg->desc->field_count; i++) {
        if (!pbw_msg_present(msg, i)) {
            continue;
        }
        if (seen == index) {
            if (value != NULL) {
                *value = &msg->values[i];
            }
            return &msg->desc->fields[i];
        }
        seen++;
    }
    return NULL;
}

void
pbw_value_destroy(pbw_value *value)
{
    size_t i;

    switch (value->kind) {
    case PBW_VALUE_BYTES:
        if ((value->flags & PBW_VF_ARENA) == 0) {
            free(value->as.bytes.data);
        }
        break;
    case PBW_VALUE_MESSAGE:
        if (value->as.msg != NULL) {
            /* A nested value normally shares its parent's arena or is a heap
             * message; a nested *decoded root* owns an arena of its own, and
             * only pbw_message_free() releases that. */
            if (value->as.msg->own != NULL) {
                pbw_message_free(value->as.msg);
            } else {
                message_release(value->as.msg);
            }
            value->as.msg = NULL;
        }
        break;
    case PBW_VALUE_LIST:
        /* Items pushed into a list carry PBW_VF_EMBEDDED (pbw_value_free on
         * a borrowed item is the documented no-op), so their wrappers are
         * released here instead of through pbw_value_free. Arena items die
         * with their arena. */
        for (i = 0; i < value->as.list.len; i++) {
            pbw_value *item = value->as.list.items[i];

            if (item == NULL || (item->flags & PBW_VF_ARENA) != 0) {
                continue;
            }
            pbw_value_destroy(item);
            free(item);
        }
        if ((value->flags & PBW_VF_ARENA) == 0) {
            free(value->as.list.items);
        }
        break;
    case PBW_VALUE_UINT:
    default:
        break;
    }
}

void
pbw_value_free(pbw_value *value)
{
    if (value == NULL || (value->flags & (PBW_VF_EMBEDDED | PBW_VF_ARENA)) != 0) {
        /* An embedded value belongs to its message, an arena value to its
         * context. */
        return;
    }
    pbw_value_destroy(value);
    free(value);
}

pbw_value *
pbw_value_new_uint(uint64_t bits)
{
    return pbw_value_new_uint_a(NULL, bits);
}

pbw_value *
pbw_value_new_int(int64_t number)
{
    return pbw_value_new_uint((uint64_t)number);
}

pbw_value *
pbw_value_new_bool(int flag)
{
    return pbw_value_new_uint(flag ? 1u : 0u);
}

pbw_value *
pbw_value_new_float(float number)
{
    uint32_t bits;

    memcpy(&bits, &number, sizeof(bits));
    return pbw_value_new_uint(bits);
}

pbw_value *
pbw_value_new_double(double number)
{
    uint64_t bits;

    memcpy(&bits, &number, sizeof(bits));
    return pbw_value_new_uint(bits);
}

/* Always allocates len + 1 and terminates: string and bytes share one value
 * kind, and decoded strings must be usable as C strings. */
int
pbw_value_init_bytes(pbw_value *value, pbw_arena *arena, const void *data,
                     size_t len)
{
    /* Leave the value destructible on every failure path: the caller hands a
     * failed init to pbw_value_free(). */
    if (value == NULL) {
        return 0;
    }
    value->kind = PBW_VALUE_BYTES;
    value->flags = arena != NULL ? PBW_VF_ARENA : 0;
    value->as.bytes.data = NULL;
    value->as.bytes.len = 0;
    if (len > SIZE_MAX - 8) {
        /* len + 1 must fit and stay 8-byte alignable for the arena; there is no
         * such buffer. */
        return 0;
    }
    if (len > 0 && data == NULL) {
        /* A NULL payload with a nonzero length would otherwise expose
         * uninitialized heap/arena bytes on the wire. Zero it instead. */
        data = pbw_empty_input;
        /* Fall through to the zero-fill below. */
    }
    if (arena != NULL) {
        value->as.bytes.data = (uint8_t *)pbw_arena_alloc(arena, len + 1);
    } else {
        value->as.bytes.data = (uint8_t *)malloc(len + 1);
    }
    if (value->as.bytes.data == NULL) {
        return 0;
    }
    if (len > 0) {
        if (data != NULL && data != pbw_empty_input) {
            memcpy(value->as.bytes.data, data, len);
        } else {
            memset(value->as.bytes.data, 0, len);
        }
    }
    value->as.bytes.data[len] = '\0';
    value->as.bytes.len = len;
    return 1;
}

pbw_value *
pbw_value_new_bytes(const void *data, size_t len)
{
    return pbw_value_new_bytes_a(NULL, data, len);
}

pbw_value *
pbw_value_new_string(const char *str)
{
    return str == NULL ? NULL : pbw_value_new_bytes(str, strlen(str));
}

pbw_value *
pbw_value_new_message(pbw_message *msg)
{
    return pbw_value_new_message_a(NULL, msg);
}

int
pbw_value_list_reserve(pbw_value *list, uint32_t cap, pbw_arena *arena)
{
    pbw_value **items;
    size_t bytes;

    if (list == NULL || list->kind != PBW_VALUE_LIST ||
        cap <= list->as.list.cap) {
        return list != NULL && list->kind == PBW_VALUE_LIST;
    }
    bytes = (size_t)cap * sizeof(*items);
    if (bytes / sizeof(*items) != cap) {
        return 0;
    }
    if (arena != NULL) {
        /* Same as the push growth: no realloc in an arena, the old block is
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
    return 1;
}

pbw_value_kind
pbw_value_get_kind(const pbw_value *value)
{
    return value == NULL ? PBW_VALUE_UINT : value->kind;
}

uint64_t
pbw_value_get_uint(const pbw_value *value)
{
    return value == NULL ? 0 : value->as.u64;
}

int64_t
pbw_value_get_int(const pbw_value *value)
{
    return (int64_t)pbw_value_get_uint(value);
}

int32_t
pbw_value_get_int32(const pbw_value *value)
{
    return (int32_t)pbw_value_get_uint(value);
}

uint32_t
pbw_value_get_uint32(const pbw_value *value)
{
    return (uint32_t)pbw_value_get_uint(value);
}

int
pbw_value_get_bool(const pbw_value *value)
{
    return pbw_value_get_uint(value) != 0;
}

float
pbw_value_get_float(const pbw_value *value)
{
    uint32_t bits = (uint32_t)pbw_value_get_uint(value);
    float number;

    memcpy(&number, &bits, sizeof(number));
    return number;
}

double
pbw_value_get_double(const pbw_value *value)
{
    uint64_t bits = pbw_value_get_uint(value);
    double number;

    memcpy(&number, &bits, sizeof(number));
    return number;
}

const void *
pbw_value_get_bytes(const pbw_value *value, size_t *len)
{
    if (value == NULL || value->kind != PBW_VALUE_BYTES) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    if (len != NULL) {
        *len = value->as.bytes.len;
    }
    return value->as.bytes.data;
}

const char *
pbw_value_get_string(const pbw_value *value)
{
    return (const char *)pbw_value_get_bytes(value, NULL);
}

pbw_message *
pbw_value_get_message(const pbw_value *value)
{
    return value != NULL && value->kind == PBW_VALUE_MESSAGE ? value->as.msg
                                                             : NULL;
}

size_t
pbw_value_count(const pbw_value *value)
{
    return value != NULL && value->kind == PBW_VALUE_LIST
               ? (size_t)value->as.list.len
               : 0;
}

pbw_value *
pbw_value_at(const pbw_value *value, size_t index)
{
    if (value == NULL || value->kind != PBW_VALUE_LIST ||
        index >= value->as.list.len) {
        return NULL;
    }
    return value->as.list.items[index];
}

/* --------------------------------------------------------------- storage */

static int
value_matches(const pbw_field *field, const pbw_value *value)
{
    switch (field->type) {
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
        return value->kind == PBW_VALUE_BYTES;
    case PBW_TYPE_MESSAGE:
    case PBW_TYPE_GROUP:
        return value->kind == PBW_VALUE_MESSAGE && value->as.msg != NULL &&
               (field->sub == NULL || value->as.msg->desc == field->sub);
    default:
        return value->kind == PBW_VALUE_UINT;
    }
}

/* Drops the value of one field slot (no presence bookkeeping). */
static void
clear_slot(pbw_message *msg, size_t index)
{
    if (pbw_msg_present(msg, index)) {
        pbw_value_destroy(&msg->values[index]);
        pbw_msg_set_present(msg, index, 0);
    }
}

/* Copies value into a field slot and marks it present. Used by the decoder,
 * which owns no value struct of its own. */
void
pbw_message_put(pbw_message *msg, size_t index, const pbw_value *value)
{
    const pbw_field *field = &msg->desc->fields[index];

    if (field->oneof_index > 0) {
        uint32_t number = msg->oneof_case[field->oneof_index];

        if (number != 0 && number != field->number) {
            const pbw_field *previous = pbw_find_field(msg->desc, number);

            if (previous != NULL) {
                clear_slot(msg, value_field_index(msg, previous));
            }
        }
        msg->oneof_case[field->oneof_index] = field->number;
    }
    clear_slot(msg, index);
    msg->values[index] = *value;
    /* Keep the caller's arena flag: a decoded payload lives in the arena, one
     * the caller passed in is theirs to free with the message. */
    msg->values[index].flags = value->flags | PBW_VF_EMBEDDED;
    pbw_msg_set_present(msg, index, 1);
}

/* Stores a value the caller owns: singular fields copy it into the slot and
 * release the wrapper, repeated fields keep the value in their list. */
int
pbw_message_store(pbw_message *msg, const pbw_field *field, pbw_value *value)
{
    if (msg->arena != NULL && (value->flags & PBW_VF_ARENA) == 0) {
        /* Heap payload entering an arena tree: the bulk free fast path no
         * longer applies, the release walk must run. */
        msg->arena->has_heap = 1;
    }
    if (field->label == PBW_LABEL_REPEATED) {
        pbw_value *list = pbw_message_list_for(msg, field);

        if (list == NULL || !pbw_value_list_push(list, value, msg->arena)) {
            return 0;
        }
        return 1;
    }
    pbw_message_put(msg, value_field_index(msg, field), value);
    /* put() copied the payload into the slot (shared, not cloned): release
     * only the wrapper. Arena wrappers die with their arena, which is what
     * makes arena-built trees work. */
    if ((value->flags & (PBW_VF_EMBEDDED | PBW_VF_ARENA)) == 0) {
        free(value);
    }
    return 1;
}

/* Get-or-create the list storage of a repeated field. */
pbw_value *
pbw_message_list_for(pbw_message *msg, const pbw_field *field)
{
    size_t index = value_field_index(msg, field);
    pbw_value *list;

    if (pbw_msg_present(msg, index)) {
        list = &msg->values[index];
        if (list->kind == PBW_VALUE_LIST) {
            return list;
        }
        clear_slot(msg, index);
    }
    list = &msg->values[index];
    list->kind = PBW_VALUE_LIST;
    list->flags = PBW_VF_EMBEDDED | (msg->arena != NULL ? PBW_VF_ARENA : 0u);
    list->as.list.items = NULL;
    list->as.list.len = 0;
    list->as.list.cap = 0;
    pbw_msg_set_present(msg, index, 1);
    return list;
}

void
pbw_message_clear(pbw_message *msg, uint32_t number)
{
    const pbw_field *field;
    size_t index;

    if (msg == NULL || (msg->arena != NULL && msg->arena->readonly)) {
        /* Context-owned messages are read-only. */
        return;
    }
    field = pbw_descriptor_find_field(msg->desc, number);
    if (field == NULL) {
        return;
    }
    index = value_field_index(msg, field);
    clear_slot(msg, index);
    if (field->oneof_index > 0 &&
        msg->oneof_case[field->oneof_index] == field->number) {
        /* Only the member that is actually set clears the case: clearing
         * an unset sibling must leave the active member addressable. */
        msg->oneof_case[field->oneof_index] = 0;
    }
}

size_t
pbw_message_unknown_len(const pbw_message *msg)
{
    return msg == NULL ? 0 : msg->unknown_len;
}

const uint8_t *
pbw_message_unknown_bytes(const pbw_message *msg)
{
    return msg == NULL ? NULL : msg->unknown;
}

pbw_status
pbw_message_add_unknown_bytes(pbw_message *msg, const void *data, size_t len)
{
    if (msg == NULL || (data == NULL && len > 0)) {
        return PBW_ERR_INVALID;
    }
    if (msg->arena != NULL && msg->arena->readonly) {
        return PBW_ERR_INVALID;
    }
    return (pbw_status)pbw_message_add_unknown(msg, (const uint8_t *)data, len);
}

pbw_status
pbw_message_set(pbw_message *msg, uint32_t number, pbw_value *value)
{
    const pbw_field *field;

    if (msg == NULL || value == NULL || (value->flags & PBW_VF_EMBEDDED) != 0) {
        return PBW_ERR_INVALID;
    }
    if (msg->arena != NULL && msg->arena->readonly) {
        /* Messages decoded through a context are read-only: a stored value would
         * outlive the arena that owns its siblings. */
        return PBW_ERR_INVALID;
    }
    field = msg->desc != NULL ? pbw_descriptor_find_field(msg->desc, number) : NULL;
    if (field == NULL) {
        return PBW_ERR_NOT_FOUND;
    }
    return pbw_message_set_field(msg, field, value);
}

pbw_status
pbw_message_set_field(pbw_message *msg, const pbw_field *field, pbw_value *value)
{
    const pbw_field *base;

    if (msg == NULL || value == NULL || (value->flags & PBW_VF_EMBEDDED) != 0 ||
        msg->desc == NULL || field == NULL) {
        return PBW_ERR_INVALID;
    }
    if (msg->arena != NULL && msg->arena->readonly) {
        return PBW_ERR_INVALID;
    }
    base = msg->desc->fields;
    if (field < base || field >= base + msg->desc->field_count) {
        return PBW_ERR_INVALID;
    }
    if (!value_matches(field, value)) {
        return PBW_ERR_TYPE;
    }
    if (!pbw_message_store(msg, field, value)) {
        return PBW_ERR_NOMEM;
    }
    return PBW_OK;
}

pbw_message *
pbw_message_set_message(pbw_message *msg, uint32_t number)
{
    const pbw_field *field;
    pbw_value *value;
    pbw_message *nested;

    if (msg == NULL || (msg->arena != NULL && msg->arena->readonly)) {
        return NULL;
    }
    field = pbw_descriptor_find_field(msg->desc, number);
    if (field == NULL ||
        (field->type != PBW_TYPE_MESSAGE && field->type != PBW_TYPE_GROUP) ||
        field->sub == NULL) {
        return NULL;
    }
    if (field->label != PBW_LABEL_REPEATED) {
        size_t index = value_field_index(msg, field);

        if (pbw_msg_present(msg, index) &&
            msg->values[index].kind == PBW_VALUE_MESSAGE) {
            return msg->values[index].as.msg;
        }
    }
    nested = pbw_message_new_a(msg->arena, field->sub);
    if (nested == NULL) {
        return NULL;
    }
    value = pbw_value_new_message_a(msg->arena, nested);
    if (value == NULL) {
        pbw_message_free(nested);
        return NULL;
    }
    if (!pbw_message_store(msg, field, value)) {
        pbw_value_free(value);
        return NULL;
    }
    return nested;
}
