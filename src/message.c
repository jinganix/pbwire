/* Dynamic message value tree: allocation, accessors, field storage, oneof
 * handling. */

#include "internal.h"

/* A message is one allocation: the struct, its value slots, the presence bitmap
 * and the oneof cases follow each other in the same block. Only the bitmap and
 * the oneof cases are zeroed; a value slot is written before it is marked
 * present. */
pbw_message *
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
    values_off = sizeof(pbw_message);
    present_off = values_off + desc->field_count * sizeof(pbw_value);
    words = (desc->field_count + 31u) / 32u;
    oneof_off = (present_off + words * sizeof(uint32_t) + 3u) & ~(size_t)3u;
    size = oneof_off + ((size_t)desc->oneof_count + 1u) * sizeof(uint32_t);

    if (arena != NULL) {
        block = (uint8_t *)pbw_arena_alloc(arena, size);
    } else {
        block = (uint8_t *)malloc(size);
    }
    if (block == NULL) {
        return NULL;
    }
    msg = (pbw_message *)block;
    memset(msg, 0, sizeof(*msg));
    msg->desc = desc;
    msg->arena = arena;
    msg->values = (pbw_value *)(void *)(block + values_off);
    msg->present = (uint32_t *)(void *)(block + present_off);
    msg->oneof_case = (uint32_t *)(void *)(block + oneof_off);
    if (words > 0) {
        memset(msg->present, 0, words * sizeof(uint32_t));
    }
    if (desc->oneof_count > 0) {
        memset(msg->oneof_case, 0,
               ((size_t)desc->oneof_count + 1u) * sizeof(uint32_t));
    }
    return msg;
}

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

    for (i = 0; i < msg->desc->field_count; i++) {
        if (pbw_msg_present(msg, i)) {
            pbw_value_destroy(&msg->values[i]);
        }
    }
    if (msg->arena == NULL) {
        /* Heap message: the struct is ours to free. */
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
        /* Root of a decoded tree: release what the caller added, then the arena
         * that holds this very struct. */
        pbw_arena *own = msg->own;
        int packed = own->block != NULL;

        message_release(msg);
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

int
pbw_message_add_unknown(pbw_message *msg, const uint8_t *data, size_t len)
{
    size_t need = msg->unknown_len + len;
    uint8_t *grown;

    if (len == 0) {
        return PBW_OK;
    }
    if (need < msg->unknown_len) {
        return PBW_ERR_NOMEM;
    }
    if (need > msg->unknown_cap) {
        size_t cap = msg->unknown_cap == 0 ? 64 : msg->unknown_cap;

        while (cap < need) {
            cap *= 2;
        }
        if (msg->arena != NULL) {
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

static int
is_initialized(const pbw_message *msg, uint32_t depth)
{
    size_t i;

    if (msg == NULL || msg->desc == NULL || depth >= PBW_MAX_DEPTH) {
        return 0;
    }
    for (i = 0; i < msg->desc->field_count; i++) {
        const pbw_field *field = &msg->desc->fields[i];
        const pbw_value *value = &msg->values[i];

        if (!pbw_msg_present(msg, i)) {
            if (!msg->desc->proto3 && field->label == PBW_LABEL_REQUIRED) {
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

/* ---------------------------------------------------------------- values */

pbw_value *
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
            message_release(value->as.msg);
        }
        break;
    case PBW_VALUE_LIST:
        for (i = 0; i < value->as.list.len; i++) {
            pbw_value_free(value->as.list.items[i]);
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
pbw_value_new_uint_a(pbw_arena *arena, uint64_t bits)
{
    pbw_value *value = pbw_value_new_a(arena, PBW_VALUE_UINT);

    if (value != NULL) {
        value->as.u64 = bits;
    }
    return value;
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
    value->kind = PBW_VALUE_BYTES;
    value->flags = arena != NULL ? PBW_VF_ARENA : 0;
    if (arena != NULL) {
        value->as.bytes.data = (uint8_t *)pbw_arena_alloc(arena, len + 1);
    } else {
        value->as.bytes.data = (uint8_t *)malloc(len + 1);
    }
    if (value->as.bytes.data == NULL) {
        return 0;
    }
    if (len > 0) {
        memcpy(value->as.bytes.data, data, len);
    }
    value->as.bytes.data[len] = '\0';
    value->as.bytes.len = len;
    return 1;
}

pbw_value *
pbw_value_new_bytes_a(pbw_arena *arena, const void *data, size_t len)
{
    pbw_value *value = pbw_value_new_a(arena, PBW_VALUE_BYTES);

    if (value == NULL) {
        return NULL;
    }
    if (!pbw_value_init_bytes(value, arena, data, len)) {
        pbw_value_free(value);
        return NULL;
    }
    return value;
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
pbw_value_new_message_a(pbw_arena *arena, pbw_message *msg)
{
    pbw_value *value = pbw_value_new_a(arena, PBW_VALUE_MESSAGE);

    if (value == NULL) {
        return NULL;
    }
    value->as.msg = msg;
    return value;
}

pbw_value *
pbw_value_new_message(pbw_message *msg)
{
    return pbw_value_new_message_a(NULL, msg);
}

pbw_value *
pbw_value_new_list_a(pbw_arena *arena)
{
    pbw_value *value = pbw_value_new_a(arena, PBW_VALUE_LIST);

    if (value != NULL) {
        value->as.list.items = NULL;
        value->as.list.len = 0;
        value->as.list.cap = 0;
    }
    return value;
}

pbw_value *
pbw_value_new_list(void)
{
    return pbw_value_new_list_a(NULL);
}

int
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
    list->as.list.items[list->as.list.len++] = item;
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
    if (field->label == PBW_LABEL_REPEATED) {
        pbw_value *list = pbw_message_list_for(msg, field);

        if (list == NULL || !pbw_value_list_push(list, value, msg->arena)) {
            return 0;
        }
        return 1;
    }
    pbw_message_put(msg, value_field_index(msg, field), value);
    free(value);
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
    if (field->oneof_index > 0) {
        msg->oneof_case[field->oneof_index] = 0;
    }
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
    field = pbw_descriptor_find_field(msg->desc, number);
    if (field == NULL) {
        return PBW_ERR_NOT_FOUND;
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
    nested = pbw_message_new(field->sub);
    if (nested == NULL) {
        return NULL;
    }
    value = pbw_value_new_message(nested);
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
