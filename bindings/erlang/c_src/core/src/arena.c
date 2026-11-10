/* Bump allocator backing pbw_ctx: allocation is a pointer add, releasing is a
 * pointer reset. Chunks double in size so a steady decode workload stops
 * calling malloc entirely after the first few messages. */

#include "internal.h"

#define PBW_ARENA_MIN_CHUNK 4096u

static size_t
align_up(size_t size)
{
    /* 8 bytes keeps every struct in this library naturally aligned. */
    return (size + 7u) & ~(size_t)7u;
}

static int
chunk_add(pbw_arena *arena, size_t need)
{
    size_t cap;
    pbw_arena_chunk *chunk;

    if (arena == NULL || arena->no_grow) {
        return 0;
    }
    cap = arena->chunk_size;
    if (cap < need) {
        cap = need;
    }
    if (cap > SIZE_MAX - PBW_CHUNK_HDR) {
        return 0;
    }
    chunk = (pbw_arena_chunk *)malloc(PBW_CHUNK_HDR + cap);
    if (chunk == NULL) {
        return 0;
    }
    chunk->next = arena->head;
    chunk->used = 0;
    chunk->cap = cap;
    arena->head = chunk;
    /* Grow the next chunk so a workload that needs more keeps its balance: the
     * chunk count stays small while per-call allocation stays O(1). */
    arena->chunk_size = cap < (size_t)1 << 20 ? cap * 2 : cap;
    return 1;
}

void *
pbw_arena_alloc_slow(pbw_arena *arena, size_t size)
{
    pbw_arena_chunk *chunk;
    void *ptr;

    size_t large_at;

    if (arena == NULL) {
        return NULL;
    }
    large_at = arena->retain_cap > PBW_ARENA_LARGE_THRESHOLD
                   ? arena->retain_cap
                   : PBW_ARENA_LARGE_THRESHOLD;

    if (size > SIZE_MAX - 7u) {
        return NULL;
    }
    size = align_up(size == 0 ? 1 : size);
    if (arena->no_grow) {
        /* Caller memory: the head chunk is the whole capacity and there is no
         * large list to fall back to, so a fit miss is the caller's NOMEM. */
        chunk = arena->head;
        if (chunk == NULL || !pbw_cap_holds(chunk->cap, chunk->used) ||
            chunk->cap - chunk->used < size) {
            return NULL;
        }
        ptr = (char *)chunk + PBW_CHUNK_HDR + chunk->used;
        chunk->used += size;
        return ptr;
    }
    if (size > large_at) {
        /* Oversized single allocation: one malloc on the large list, freed on
         * reset/free instead of pinning the reusable head chunk. */
        pbw_arena_chunk *big;

        /* new_packed guards the same addition; sizes here can approach
         * SIZE_MAX - 7 through the public bytes constructors. */
        if (size > SIZE_MAX - PBW_CHUNK_HDR) {
            return NULL;
        }
        big = (pbw_arena_chunk *)malloc(PBW_CHUNK_HDR + size);

        if (big == NULL) {
            return NULL;
        }
        big->next = arena->large;
        big->used = size;
        big->cap = size;
        arena->large = big;
        return (char *)big + PBW_CHUNK_HDR;
    }
    chunk = arena->head;
    if (chunk == NULL || !pbw_cap_holds(chunk->cap, chunk->used) ||
        chunk->cap - chunk->used < size) {
        if (!chunk_add(arena, size)) {
            return NULL;
        }
        chunk = arena->head;
    }
    ptr = (char *)chunk + PBW_CHUNK_HDR + chunk->used;
    chunk->used += size;
    return ptr;
}

void
pbw_arena_reset(pbw_arena *arena)
{
    /* Chunks are pushed at the head and grow, so the head is the largest one:
     * keep it and drop the rest. A workload that needs the same amount of space
     * every time then stops calling malloc entirely. */
    pbw_arena_chunk *head;
    pbw_arena_chunk *chunk;

    if (arena == NULL) {
        return;
    }
    head = arena->head;

    if (arena->no_grow) {
        /* Caller memory: nothing can be given back, so a reset is a pointer
         * move and the block stays exactly where the caller put it. */
        arena->readonly = 0;
        arena->has_heap = 0;
        if (head != NULL) {
            head->used = 0;
        }
        return;
    }
    if (arena->block != NULL) {
        /* A packed arena belongs to one decoded tree and is never reset; it
         * is released as a whole by pbw_message_free(). */
        arena->readonly = 0;
        return;
    }
    /* A reset context is a blank slate: trees built into it afterwards are
     * mutable until a decode seals it read-only again. */
    arena->readonly = 0;
    arena->has_heap = 0;
    /* Large single allocations never become resident. Free them even when no
     * head chunk exists: a context that only ever held oversized payloads
     * (pbw_value_new_bytes_ctx above the threshold, no small allocation)
     * would otherwise pin every one of them until pbw_ctx_free(). */
    for (chunk = arena->large; chunk != NULL;) {
        pbw_arena_chunk *next = chunk->next;

        free(chunk);
        chunk = next;
    }
    arena->large = NULL;
    if (head == NULL) {
        arena->chunk_size = PBW_ARENA_MIN_CHUNK;
        return;
    }
    /* An outlier that grew the head past the budget is dropped as well:
     * steady workloads under budget stay malloc-free, anything above pays
     * one malloc per reset instead of pinning the peak forever. */
    if (head->cap > arena->retain_cap) {
        free(head);
        arena->head = NULL;
        arena->chunk_size = PBW_ARENA_MIN_CHUNK;
        return;
    }
    for (chunk = head->next; chunk != NULL;) {
        pbw_arena_chunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    head->next = NULL;
    head->used = 0;
}

pbw_arena *
pbw_arena_new_packed(size_t first_chunk)
{
    size_t total;
    uint8_t *block;
    pbw_arena *arena;
    pbw_arena_chunk *chunk;

    if (first_chunk > SIZE_MAX - PBW_ARENA_HDR - PBW_CHUNK_HDR) {
        return NULL;
    }
    total = PBW_ARENA_HDR + PBW_CHUNK_HDR + first_chunk;
    block = (uint8_t *)malloc(total);
    if (block == NULL) {
        return NULL;
    }
    arena = (pbw_arena *)(void *)block;
    chunk = (pbw_arena_chunk *)(void *)(block + PBW_ARENA_HDR);
    chunk->next = NULL;
    chunk->used = 0;
    chunk->cap = first_chunk;
    arena->head = chunk;
    arena->large = NULL;
    arena->retain_cap = PBW_ARENA_LARGE_THRESHOLD;
    arena->chunk_size = first_chunk < PBW_ARENA_MIN_CHUNK ? PBW_ARENA_MIN_CHUNK
                                                          : first_chunk;
    arena->readonly = 0;
    arena->block = block;
    arena->has_heap = 0;
    arena->no_grow = 0;
    return arena;
}

int
pbw_arena_init_mem(pbw_arena *arena, void *mem, size_t len)
{
    pbw_arena_chunk *chunk;

    if (arena == NULL || mem == NULL || len < PBW_CHUNK_HDR + 8u) {
        return 0;
    }
    memset(arena, 0, sizeof(*arena));
    chunk = (pbw_arena_chunk *)mem;
    chunk->next = NULL;
    chunk->used = 0;
    chunk->cap = len - PBW_CHUNK_HDR;
    arena->head = chunk;
    arena->chunk_size = chunk->cap;
    arena->block = mem;
    arena->no_grow = 1;
    return 1;
}

void
pbw_arena_free(pbw_arena *arena)
{
    pbw_arena_chunk *chunk;
    pbw_arena_chunk *big;

    if (arena == NULL) {
        return;
    }
    if (arena->no_grow) {
        /* Caller memory: the arena never allocated, so it owns nothing to
         * free and the caller's block stays valid after this call. */
        return;
    }
    chunk = arena->head;
    big = arena->large;

    while (big != NULL) {
        pbw_arena_chunk *next = big->next;

        free(big);
        big = next;
    }
    arena->large = NULL;
    if (arena->block != NULL) {
        /* The arena struct and its first chunk live in the block itself. */
        uint8_t *base = (uint8_t *)arena->block;
        pbw_arena_chunk *first =
            (pbw_arena_chunk *)(void *)(base + PBW_ARENA_HDR);

        for (chunk = arena->head; chunk != NULL && chunk != first;) {
            pbw_arena_chunk *next = chunk->next;

            free(chunk);
            chunk = next;
        }
        free(base);
        return;
    }
    while (chunk != NULL) {
        pbw_arena_chunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    arena->head = NULL;
    arena->chunk_size = PBW_ARENA_MIN_CHUNK;
}

size_t
pbw_arena_used(const pbw_arena *arena)
{
    const pbw_arena_chunk *chunk;
    size_t used = 0;

    if (arena == NULL) {
        return 0;
    }

    for (chunk = arena->head; chunk != NULL; chunk = chunk->next) {
        used += chunk->used;
    }
    for (chunk = arena->large; chunk != NULL; chunk = chunk->next) {
        used += chunk->used;
    }
    return used;
}
