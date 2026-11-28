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
            /* An arena message's struct outlives its payload (a clear() only
             * releases the child), so a released slot must never read as
             * present. */
            pbw_msg_set_present(msg, i, 0);
        }
    }
    if (msg->arena == NULL) {
        /* Heap message: the unknown bytes and the struct are ours to free. In an
         * arena the unknown bytes are arena memory, released with the arena. */
        free(msg->unknown);
        free(msg);
    }
}

/* Releases a message together with whatever it owns, ignoring `stored`: the
 * caller IS its owner (a value slot, a list item, or the tree being freed). */
static void
message_free_body(pbw_message *msg)
{
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

void
pbw_message_free(pbw_message *msg)
{
    if (msg == NULL || msg->stored) {
        /* Owned by a parent slot or list (set/set_message, an adopted decoded
         * root, or a decoded tree's submessage): it dies with the parent,
         * like arena memory. The release walk calls message_free_body()
         * directly, so a direct free here is a no-op. */
        return;
    }
    message_free_body(msg);
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

/* Caller-bounded append: intentionally no max_bytes check here (decode-path
 * unknowns are bounded by limits->max_bytes instead); repeated calls grow
 * the store without bound, so the caller must bound total bytes/calls.
 * shortcut: caller-bounded, upgrade to checked wrapper when gateway exposes
 * raw append to untrusted input. */
int
pbw_message_add_unknown(pbw_message *msg, const uint8_t *data, size_t len)
{
    size_t need;
    uint8_t *grown;
    uint8_t *stash = NULL;

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
        /* The caller may append the message's own blob to itself; a heap
         * realloc can move that block, so copy the source first. Arena
         * memory never moves, so the copy below stays valid without one. */
        if (msg->arena == NULL && msg->unknown != NULL && len <= (size_t)(UINTPTR_MAX - (uintptr_t)(const void *)data)) {
            uintptr_t base = (uintptr_t)(void *)msg->unknown;
            uintptr_t src = (uintptr_t)(const void *)data;

            if (src < base + msg->unknown_len && src + len > base) {
                stash = (uint8_t *)malloc(len);
                if (stash == NULL) {
                    return PBW_ERR_NOMEM;
                }
                memcpy(stash, data, len);
                data = stash;
            }
        }
        if (msg->arena != NULL) {
            if (msg->unknown == NULL && msg->unknown_len > 0) {
                free(stash);
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
            free(stash);
            return PBW_ERR_NOMEM;
        }
        msg->unknown = grown;
        msg->unknown_cap = cap;
    }
    memmove(msg->unknown + msg->unknown_len, data, len);
    msg->unknown_len = need;
    free(stash);
    return PBW_OK;
}

/* 1 when `target` is `msg` or sits anywhere below it: attaching such a tree
 * would make one message its own ancestor, and clear()/free() would release
 * the arena (or struct) it is walking inside. Only reachable path: an
 * unowned heap root attached into a nested node of an existing tree, which is
 * why the walk is only paid there. */
static int
message_contains(const pbw_message *msg, const pbw_message *target,
                 uint32_t depth)
{
    size_t i;

    if (msg == target) {
        return 1;
    }
    if (depth > PBW_MAX_DEPTH) {
        /* Deeper than the encode nesting cap (encode and the printers allow a
         * node at PBW_MAX_DEPTH, not below it): such a tree is already
         * unusable, so answer "contains" and let the store be refused.
         * Answering 0 accepted a cycle closed below the walk, whose root is
         * then `stored` and unreachable for every free. */
        return 1;
    }
    for (i = 0; i < msg->nfields; i++) {
        const pbw_value *v;

        if (!pbw_msg_present(msg, i)) {
            continue;
        }
        v = &msg->values[i];
        if (v->kind == PBW_VALUE_LIST) {
            size_t j;

            if (v->as.list.items == NULL) {
                continue;
            }
            for (j = 0; j < v->as.list.len; j++) {
                const pbw_value *item = v->as.list.items[j];

                if (item == NULL || item->kind != PBW_VALUE_MESSAGE ||
                    item->as.msg == NULL) {
                    continue;
                }
                if (message_contains(item->as.msg, target, depth + 1)) {
                    return 1;
                }
            }
        } else if (v->kind == PBW_VALUE_MESSAGE && v->as.msg != NULL) {
            if (message_contains(v->as.msg, target, depth + 1)) {
                return 1;
            }
        }
    }
    return 0;
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

    if (!pbw_msg_fresh(msg)) {
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

    if (!pbw_msg_fresh(msg) || field == NULL) {
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
    if (!pbw_msg_fresh(msg) || depth > PBW_MAX_DEPTH) {
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
            if (value->as.list.len > 0 && value->as.list.items == NULL) {
                /* Corrupt list (items but no array), same shape the
                 * encoder and the printers refuse: report uninitialized
                 * instead of reading NULL[0]. */
                return 0;
            }
            for (j = 0; j < value->as.list.len; j++) {
                const pbw_value *item = value->as.list.items[j];

                /* Same guard as encode: a hostile put() can plant a
                 * non-message item here, and dereferencing it crashes
                 * instead of reporting uninitialized. */
                if (item == NULL || item->kind != PBW_VALUE_MESSAGE ||
                    item->as.msg == NULL ||
                    !is_initialized(item->as.msg, depth + 1)) {
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

    if (!pbw_msg_fresh(msg) || fnums == NULL || values == NULL) {
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

    if (!pbw_msg_fresh(msg)) {
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

    if (!pbw_msg_fresh(msg)) {
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

/* Releases a message a wrapper owned, whichever wrapper: an arena
 * submessage's struct outlives the release, so it becomes unowned again (a
 * caller that cleared the field may adopt it elsewhere); a heap message and an
 * adopted decoded root are released with their payloads. */
static void
message_release_slot(pbw_message *m)
{
    if (m->arena != NULL && m->own == NULL) {
        message_release(m);
        m->stored = 0;
        return;
    }
    message_free_body(m);
}

/* 1 when the message lives in a decoded tree's arena but no tree owns it any
 * more: pbw_message_clear() released it, so no teardown walk reaches its slots
 * (a context or caller-memory arena keeps this false: those are roots). */
static int
message_detached(const pbw_message *msg)
{
    return msg->arena != NULL && msg->own == NULL && msg->stored == 0 &&
           msg->arena->block != NULL && !msg->arena->no_grow;
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
            pbw_message *m = value->as.msg;

            value->as.msg = NULL;
            if ((value->flags & PBW_VF_EMBEDDED) == 0 && m->stored) {
                /* A wrapper the caller made around a message another tree
                 * owns: that tree releases it, not this wrapper. */
                break;
            }
            /* An arena submessage's struct dies with the arena, but a heap
             * payload stored into it (or into one of its children) does not:
             * that walk is the only place it can be released. */
            message_release_slot(m);
        }
        break;
    case PBW_VALUE_LIST:
        /* Items pushed into a list carry PBW_VF_EMBEDDED (pbw_value_free on
         * a borrowed item is the documented no-op), so their wrappers are
         * released here instead of through pbw_value_free. Arena items die
         * with their arena. */
        if (value->as.list.items != NULL) {
            /* A list with items but no array is corrupt: nothing to walk. */
            for (i = 0; i < value->as.list.len; i++) {
                pbw_value *item = value->as.list.items[i];

                if (item == NULL) {
                    continue;
                }
                if ((item->flags & PBW_VF_ARENA) != 0) {
                    /* The wrapper dies with the arena, but a heap payload
                     * stored into an arena message item must be released. */
                    if (item->kind == PBW_VALUE_MESSAGE &&
                        item->as.msg != NULL) {
                        message_release_slot(item->as.msg);
                    }
                    continue;
                }
                pbw_value_destroy(item);
                free(item);
            }
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

/* Wrong-kind (including NULL) reads as 0 by design, indistinguishable from a
 * genuine 0: callers must check pbw_value_get_kind() == PBW_VALUE_UINT first.
 * No status-returning checked accessor exists; signatures stay frozen for ABI. */
uint64_t
pbw_value_get_uint(const pbw_value *value)
{
    if (value == NULL || value->kind != PBW_VALUE_UINT) {
        return 0;
    }
    return value->as.u64;
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
        value->as.list.items == NULL || index >= value->as.list.len) {
        /* A list with items but no array is corrupt: a failed lookup, not
         * a NULL-before-array read. */
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
    /* Any store/clear invalidates the decode-size hint, so a shrink never
     * over-reserves the next encode. */
    msg->wire_len = 0;
    if (pbw_msg_present(msg, index)) {
        /* Absent before destroy, for the same reason the release walk does it:
         * an arena message's struct outlives its payload. */
        pbw_msg_set_present(msg, index, 0);
        pbw_value_destroy(&msg->values[index]);
    }
}

/* Copies value into a field slot and marks it present. Used by the decoder,
 * which owns no value struct of its own. */
void
pbw_message_put(pbw_message *msg, size_t index, const pbw_value *value)
{
    const pbw_field *field = &msg->desc->fields[index];

    if (value->kind == PBW_VALUE_BYTES && value->as.bytes.len > 0 &&
        value->as.bytes.data == NULL) {
        /* Every constructor NUL-terminates and refuses this shape, so it can
         * only arrive from a hand-built value: refuse it here instead of
         * planting a payload every writer would read through NULL. Checked
         * before the oneof update, so a rejected value cannot leave the
         * group pointing at an absent field. */
        return;
    }
    if (field->oneof_index > 0) {
        uint32_t number = msg->oneof_case[field->oneof_index];

        if (number != 0 && number != field->number) {
            const pbw_field *previous = pbw_find_field(msg->desc, number);

            if (previous == NULL &&
                (msg->desc->schema == NULL ||
                 !msg->desc->schema->linked)) {
                /* Unsorted rows: the binary search above cannot see them,
                 * so an old sibling would stay present beside the new one. */
                size_t k;

                for (k = 0; k < msg->desc->field_count; k++) {
                    if (msg->desc->fields[k].number == number) {
                        previous = &msg->desc->fields[k];
                        break;
                    }
                }
            }
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
    if (value->kind == PBW_VALUE_MESSAGE && value->as.msg != NULL) {
        /* Owned now: storing the same sub-message twice would free it
         * twice, so set_field() rejects it (see the stored check). */
        value->as.msg->stored = 1;
    }
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
        size_t index = value_field_index(msg, field);
        int fresh = !pbw_msg_present(msg, index);
        pbw_value *list = pbw_message_list_for(msg, field);

        if (!pbw_value_list_push(list, value, msg->arena)) {
            /* Do not leave an empty list behind as a present field when the
             * push is what failed: the caller keeps the value and the field
             * stays absent, like a singular store that failed. */
            if (fresh) {
                clear_slot(msg, index);
            }
            return 0;
        }
        /* The message item is marked owned by pbw_value_list_push(). */
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

    if (msg == NULL || !pbw_msg_fresh(msg) ||
        (msg->arena != NULL && msg->arena->readonly)) {
        /* Context-owned messages are read-only; stale ones are dead. */
        return;
    }
    field = pbw_descriptor_find_field(msg->desc, number);
    if (field == NULL && msg->desc != NULL && msg->desc->schema != NULL &&
        !msg->desc->schema->linked) {
        /* An unlinked descriptor has no sorted rows or number table, so the
         * lookup above refuses it; set_field() still accepts a field pointer,
         * so clear has to work there too. Linear scan over the added rows. */
        size_t i;

        for (i = 0; i < msg->desc->field_count; i++) {
            if (msg->desc->fields[i].number == number) {
                field = &msg->desc->fields[i];
                break;
            }
        }
    }
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

    if (msg == NULL || value == NULL || (value->flags & PBW_VF_EMBEDDED) != 0 ||
        !pbw_msg_fresh(msg)) {
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
        !pbw_msg_fresh(msg) || field == NULL) {
        return PBW_ERR_INVALID;
    }
    base = msg->desc->fields;
    if (field < base || field >= base + msg->desc->field_count) {
        return PBW_ERR_INVALID;
    }
    if (message_detached(msg)) {
        /* pbw_message_clear() released it: nothing walks its slots any more, so
         * a payload stored here (or a child created here) would leak. Store it
         * back into a tree first; the same message is writable again afterwards. */
        return PBW_ERR_INVALID;
    }
    if (msg->arena != NULL) {
        if (msg->arena->readonly) {
            return PBW_ERR_INVALID;
        }
        if ((msg->arena->block == NULL || msg->arena->no_grow) &&
            (value->flags & PBW_VF_ARENA) == 0) {
            /* A context arena (heap-grown or caller memory) releases by reset,
             * so it cannot free a heap payload: only values built in that arena
             * (pbw_value_new_*_ctx) can be stored. A decoded tree's own packed
             * arena can: pbw_message_free() walks its slots (has_heap). */
            return PBW_ERR_INVALID;
        }
    } else if ((value->flags & PBW_VF_ARENA) != 0) {
        /* A heap tree cannot hold a context-arena payload: the next reset
         * would free it under the tree. */
        return PBW_ERR_INVALID;
    }
    if (msg->arena != NULL && (value->flags & PBW_VF_ARENA) != 0) {
        /* Same arena only: a value from another arena dies with its own
         * reset, and a value from before this arena's reset is already
         * dead (owns() sees only live bytes, below `used`). The wrapper
         * itself counts too: the repeated path keeps the wrapper pointer,
         * so a foreign wrapper would dangle even when it carries no
         * payload (plain integers). */
        const void *payload = NULL;

        if (!pbw_arena_owns(msg->arena, value)) {
            return PBW_ERR_INVALID;
        }
        if (value->kind == PBW_VALUE_BYTES) {
            payload = value->as.bytes.data;
        } else if (value->kind == PBW_VALUE_MESSAGE) {
            payload = value->as.msg;
        } else if (value->kind == PBW_VALUE_LIST) {
            payload = value->as.list.items;
        }
        if (!pbw_arena_owns(msg->arena, payload)) {
            return PBW_ERR_INVALID;
        }
    }
    if (value->kind == PBW_VALUE_MESSAGE && value->as.msg != NULL &&
        value->as.msg->stored) {
        /* Already owned by another message: sharing it would free it twice. */
        return PBW_ERR_INVALID;
    }
    if (value->kind == PBW_VALUE_MESSAGE && value->as.msg != NULL) {
        pbw_message *kid = value->as.msg;

        /* No message may become its own ancestor: clear() or free() would
         * then release the arena (or struct) it is walking inside. The two
         * cheap cases are the message itself and the root of the decoded tree
         * this message lives in (they share the arena). */
        if (kid == msg ||
            (msg->arena != NULL && kid->own == msg->arena)) {
            return PBW_ERR_INVALID;
        }
        /* The remaining way to close a cycle is an unowned root attached into
         * a nested node; that root's tree is the only place the target could
         * be, so one walk over it answers exactly. */
        if (kid->stored == 0 && msg->stored != 0 &&
            message_contains(kid, msg, 0)) {
            return PBW_ERR_INVALID;
        }
    }
    if (value->kind == PBW_VALUE_MESSAGE && value->as.msg != NULL &&
        value->as.msg->arena != NULL) {
        /* A child with an arena must die with this tree, or a reset frees it
         * under the parent. Two ways that holds: it shares this tree's arena
         * (checked above when the wrapper is an arena value), or it owns its
         * arena (a decoded root) and this tree walks its slots on free -- a
         * heap tree, or a decoded tree's packed arena, where a heap payload
         * arms the walk (pbw_message_store()). A context or caller-memory
         * arena never walks (reset is a pointer move), so an adopted root's
         * arena would leak there. */
        pbw_message *kid = value->as.msg;
        int shared = msg->arena != NULL && pbw_arena_owns(msg->arena, kid);
        int walks = msg->arena == NULL ||
                    (msg->arena->block != NULL && !msg->arena->no_grow);

        if (!shared && (kid->own == NULL || !walks)) {
            return PBW_ERR_INVALID;
        }
    }
    if (value->kind == PBW_VALUE_BYTES && !pbw_bytes_readable(value)) {
        /* A NULL pointer with a length can only arrive from a hand-built
         * value (the constructors refuse it): the slot copy below would
         * silently drop it while reporting success, and the repeated path
         * would plant an item every writer refuses. Fail it here instead. */
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

    if (msg == NULL || !pbw_msg_fresh(msg) || message_detached(msg) ||
        (msg->arena != NULL && msg->arena->readonly)) {
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
