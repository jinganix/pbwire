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
    size_t cap = arena->chunk_size;
    pbw_arena_chunk *chunk;

    if (arena == NULL) {
        return 0;
    }
    if (cap < need) {
        cap = need;
    }
    if (cap > SIZE_MAX - sizeof(*chunk)) {
        return 0;
    }
    chunk = (pbw_arena_chunk *)malloc(sizeof(*chunk) + cap);
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
    if (size > large_at) {
        /* Oversized single allocation: one malloc on the large list, freed on
         * reset/free instead of pinning the reusable head chunk. */
        pbw_arena_chunk *big;

        /* new_packed guards the same addition; sizes here can approach
         * SIZE_MAX - 7 through the public bytes constructors. */
        if (size > SIZE_MAX - sizeof(*big)) {
            return NULL;
        }
        big = (pbw_arena_chunk *)malloc(sizeof(*big) + size);

        if (big == NULL) {
            return NULL;
        }
        big->next = arena->large;
        big->used = size;
        big->cap = size;
        arena->large = big;
        return (char *)(big + 1);
    }
    chunk = arena->head;
    if (chunk == NULL || !pbw_cap_holds(chunk->cap, chunk->used) ||
        chunk->cap - chunk->used < size) {
        if (!chunk_add(arena, size)) {
            return NULL;
        }
        chunk = arena->head;
    }
    ptr = (char *)(chunk + 1) + chunk->used;
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

    if (head == NULL || arena->block != NULL) {
        /* A packed arena belongs to one decoded tree and is never reset.
         * A fresh context has no chunks yet: still return it to the blank
         * (mutable) slate. */
        arena->readonly = 0;
        return;
    }
    /* A reset context is a blank slate: trees built into it afterwards are
     * mutable until a decode seals it read-only again. */
    arena->readonly = 0;
    /* Large single allocations never become resident. */
    for (chunk = arena->large; chunk != NULL;) {
        pbw_arena_chunk *next = chunk->next;

        free(chunk);
        chunk = next;
    }
    arena->large = NULL;
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

    if (first_chunk > SIZE_MAX - sizeof(pbw_arena) - sizeof(pbw_arena_chunk)) {
        return NULL;
    }
    total = sizeof(pbw_arena) + sizeof(pbw_arena_chunk) + first_chunk;
    block = (uint8_t *)malloc(total);
    if (block == NULL) {
        return NULL;
    }
    arena = (pbw_arena *)(void *)block;
    chunk = (pbw_arena_chunk *)(void *)(block + sizeof(pbw_arena));
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
    return arena;
}

void
pbw_arena_free(pbw_arena *arena)
{
    pbw_arena_chunk *chunk;
    pbw_arena_chunk *big;

    if (arena == NULL) {
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
            (pbw_arena_chunk *)(void *)(base + sizeof(pbw_arena));

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
