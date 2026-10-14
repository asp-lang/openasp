// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Monotonic request arena. Small allocations advance an aligned cursor; large
 * allocations receive dedicated blocks; ordinary growth doubles to a 1 MiB
 * ceiling. Mapped mode rounds blocks to pages and discards used pages on reset,
 * which keeps long-lived FastCGI workers while returning physical memory.
 */

#define _DEFAULT_SOURCE

#include "openasp.h"
#include "memory.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#define OASP_MAP_ANONYMOUS MAP_ANON
#else
#define OASP_MAP_ANONYMOUS MAP_ANONYMOUS
#endif

typedef struct oasp_block {
    /* Allocation-order chain and payload accounting for one malloc/mmap region. */
    struct oasp_block *next;
    size_t capacity;
    size_t used;
    size_t allocation_size;
    unsigned char data[];
} oasp_block;

typedef struct oasp_finalizer {
    /* LIFO callback for native resources attached to arena-owned objects. */
    struct oasp_finalizer *next;
    oasp_finalizer_fn function;
    void *context;
} oasp_finalizer;

struct oasp_arena {
    /* `current` is the allocation cursor; blocks after it do not exist. */
    oasp_block *first;
    oasp_block *current;
    size_t block_size;
    size_t next_block_size;
    /* Logical payload and reserved capacity are reported separately. */
    size_t used;
    size_t peak_used;
    size_t reserved;
    oasp_finalizer *finalizers;
    oasp_memory_manager *memory;
    int mapped;
};

/* Rounds `value` up; callers first prove power-of-two alignment and no overflow. */
static size_t oasp_align_up(size_t value, size_t alignment) {
    size_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

/*
 * Allocates one payload block. Mapped blocks are page-rounded so munmap and
 * madvise can operate on exact regions; `allocation_size` retains that extent.
 */
static bool oasp_block_layout(size_t capacity, int mapped, size_t *allocation_size, size_t *actual_capacity) {
    if (capacity > SIZE_MAX - sizeof(oasp_block))
        return false;
    *allocation_size = sizeof(oasp_block) + capacity;
    if (mapped) {
        long page_size_value = sysconf(_SC_PAGESIZE);
        size_t page_size = page_size_value > 0 ? (size_t)page_size_value : 4096;
        if (*allocation_size > SIZE_MAX - (page_size - 1))
            return false;
        *allocation_size = oasp_align_up(*allocation_size, page_size);
    }
    *actual_capacity = *allocation_size - sizeof(oasp_block);
    return true;
}

static oasp_block *oasp_block_new(size_t capacity, int mapped) {
    size_t allocation_size;
    if (!oasp_block_layout(capacity, mapped, &allocation_size, &capacity))
        return NULL;
    oasp_block *block;
    if (mapped) {
        block = mmap(NULL, allocation_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | OASP_MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED)
            return NULL;
    } else {
        block = malloc(allocation_size);
    }
    if (!block)
        return NULL;
    block->next = NULL;
    block->capacity = capacity;
    block->used = 0;
    block->allocation_size = allocation_size;
    return block;
}

/* Releases a block with the allocator matching its creation mode. */
static void oasp_block_destroy(oasp_block *block, int mapped) {
    if (mapped)
        (void)munmap(block, block->allocation_size);
    else
        free(block);
}

/* Builds the arena and its reusable first block, normalizing tiny sizes to 4 KiB. */
static oasp_arena *oasp_arena_create_internal(size_t block_size, int mapped) {
    oasp_arena *arena = calloc(1, sizeof(*arena));
    if (!arena)
        return NULL;
    arena->block_size = block_size < 4096 ? 4096 : block_size;
    arena->next_block_size = arena->block_size;
    arena->mapped = mapped;
    arena->first = oasp_block_new(arena->block_size, mapped);
    if (!arena->first) {
        free(arena);
        return NULL;
    }
    arena->current = arena->first;
    arena->reserved = arena->first->capacity;
    return arena;
}

oasp_arena *oasp_arena_create(size_t block_size) { return oasp_arena_create_internal(block_size, 0); }

oasp_arena *oasp_arena_create_mapped(size_t block_size) { return oasp_arena_create_internal(block_size, 1); }

/*
 * Returns aligned uninitialized storage owned by `arena`. If the current block
 * cannot fit it, a dedicated or geometrically grown block is linked after the
 * cursor. All additions are overflow-checked before allocation.
 */
void *oasp_arena_alloc(oasp_arena *arena, size_t size, size_t alignment) {
    if (!arena || !size || alignment == 0 || (alignment & (alignment - 1)))
        return NULL;
    oasp_block *block = arena->current;
    size_t offset = oasp_align_up(block->used, alignment);
    if (offset > block->capacity || size > block->capacity - offset) {
        size_t capacity = arena->next_block_size;
        if (size > SIZE_MAX - alignment)
            return NULL;
        bool dedicated = capacity < size + alignment;
        if (dedicated)
            capacity = size + alignment;
        size_t allocation_size;
        size_t actual_capacity;
        if (!oasp_block_layout(capacity, arena->mapped, &allocation_size, &actual_capacity) ||
            !oasp_memory_reserve(arena->memory, actual_capacity, "arena block"))
            return NULL;
        (void)allocation_size;
        block = oasp_block_new(capacity, arena->mapped);
        if (!block) {
            oasp_memory_release(arena->memory, actual_capacity);
            return NULL;
        }
        block->next = arena->current->next;
        arena->current->next = block;
        arena->current = block;
        arena->reserved += block->capacity;
        if (!dedicated && arena->next_block_size < 1024 * 1024) {
            size_t grown = arena->next_block_size > SIZE_MAX / 2 ? SIZE_MAX : arena->next_block_size * 2;
            arena->next_block_size = grown > 1024 * 1024 ? 1024 * 1024 : grown;
        }
        offset = oasp_align_up(0, alignment);
    }
    void *result = block->data + offset;
    block->used = offset + size;
    arena->used += size;
    if (arena->used > arena->peak_used)
        arena->peak_used = arena->used;
    return result;
}

/*
 * Registers a LIFO finalizer in arena memory. LIFO matches construction order,
 * so dependent native resources are released before the objects they depend on.
 */
int oasp_arena_defer(oasp_arena *arena, oasp_finalizer_fn function, void *context) {
    if (!arena || !function)
        return 0;
    oasp_finalizer *finalizer = oasp_arena_alloc(arena, sizeof(*finalizer), _Alignof(oasp_finalizer));
    if (!finalizer)
        return 0;
    finalizer->function = function;
    finalizer->context = context;
    finalizer->next = arena->finalizers;
    arena->finalizers = finalizer;
    return 1;
}

/* Captures allocation and finalizer cursors without allocating. */
oasp_arena_checkpoint oasp_arena_checkpoint_save(const oasp_arena *arena) {
    oasp_arena_checkpoint checkpoint = {0};
    if (!arena)
        return checkpoint;
    checkpoint.block = arena->current;
    checkpoint.finalizer = arena->finalizers;
    checkpoint.block_used = arena->current->used;
    checkpoint.used = arena->used;
    return checkpoint;
}

/*
 * Rolls back only to a checkpoint belonging to this live arena. New finalizers
 * run first, later blocks are released, and accounting/growth state is restored.
 */
void oasp_arena_checkpoint_restore(oasp_arena *arena, oasp_arena_checkpoint checkpoint) {
    if (!arena || !checkpoint.block)
        return;
    oasp_block *marked = checkpoint.block;
    oasp_block *cursor = arena->first;
    while (cursor && cursor != marked)
        cursor = cursor->next;
    if (!cursor || checkpoint.block_used > marked->used)
        return;
    while (arena->finalizers != checkpoint.finalizer) {
        oasp_finalizer *finalizer = arena->finalizers;
        if (!finalizer)
            return;
        arena->finalizers = finalizer->next;
        finalizer->function(finalizer->context);
    }
    oasp_block *discard = marked->next;
    marked->next = NULL;
    while (discard) {
        oasp_block *next = discard->next;
        arena->reserved -= discard->capacity;
        oasp_memory_release(arena->memory, discard->capacity);
        oasp_block_destroy(discard, arena->mapped);
        discard = next;
    }
    marked->used = checkpoint.block_used;
    arena->current = marked;
    arena->used = checkpoint.used;
    arena->next_block_size = marked == arena->first ? arena->block_size : marked->capacity;
    if (arena->next_block_size > 1024 * 1024)
        arena->next_block_size = 1024 * 1024;
}

/* Runs and detaches all registered finalizers exactly once in LIFO order. */
static void oasp_arena_finalize(oasp_arena *arena) {
    oasp_finalizer *finalizer = arena->finalizers;
    arena->finalizers = NULL;
    while (finalizer) {
        oasp_finalizer *next = finalizer->next;
        finalizer->function(finalizer->context);
        finalizer = next;
    }
}

/*
 * Runs finalizers, frees every growth block, and reuses the first block. Mapped
 * arenas advise away page-aligned payload pages so RSS can fall without losing
 * the stable virtual allocation needed by the next request.
 */
void oasp_arena_reset(oasp_arena *arena) {
    if (!arena)
        return;
    oasp_arena_finalize(arena);
    oasp_block *block = arena->first;
    oasp_block *next = block->next;
    while (next) {
        oasp_block *discard = next;
        next = next->next;
        oasp_memory_release(arena->memory, discard->capacity);
        oasp_block_destroy(discard, arena->mapped);
    }
    block->next = NULL;
    if (arena->mapped && block->used) {
        long page_size_value = sysconf(_SC_PAGESIZE);
        size_t page_size = page_size_value > 0 ? (size_t)page_size_value : 4096;
        uintptr_t allocation_start = (uintptr_t)block;
        uintptr_t data_start = oasp_align_up((uintptr_t)block->data, page_size);
        uintptr_t allocation_end = allocation_start + block->allocation_size;
        if (data_start < allocation_end)
            (void)madvise((void *)data_start, allocation_end - data_start, MADV_DONTNEED);
    }
    block->used = 0;
    arena->current = block;
    arena->next_block_size = arena->block_size;
    arena->used = 0;
    arena->peak_used = 0;
    arena->reserved = block->capacity;
}

/* Finalizes native resources, releases every block, then frees arena metadata. */
void oasp_arena_destroy(oasp_arena *arena) {
    if (!arena)
        return;
    oasp_arena_finalize(arena);
    oasp_block *block = arena->first;
    while (block) {
        oasp_block *next = block->next;
        oasp_memory_release(arena->memory, block->capacity);
        oasp_block_destroy(block, arena->mapped);
        block = next;
    }
    free(arena);
}

/* Read-only accounting accessors; NULL is reported as zero for diagnostics. */
size_t oasp_arena_used(const oasp_arena *arena) { return arena ? arena->used : 0; }

size_t oasp_arena_peak_used(const oasp_arena *arena) { return arena ? arena->peak_used : 0; }
size_t oasp_arena_reserved(const oasp_arena *arena) { return arena ? arena->reserved : 0; }

bool oasp_arena_bind_memory(oasp_arena *arena, oasp_memory_manager *manager) {
    if (!arena || !manager)
        return false;
    if (arena->memory == manager)
        return true;
    if (arena->memory)
        oasp_arena_unbind_memory(arena);
    if (!oasp_memory_reserve(manager, arena->reserved, "arena capacity"))
        return false;
    arena->memory = manager;
    return true;
}

void oasp_arena_unbind_memory(oasp_arena *arena) {
    if (!arena || !arena->memory)
        return;
    oasp_memory_release(arena->memory, arena->reserved);
    arena->memory = NULL;
}
