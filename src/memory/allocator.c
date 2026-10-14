// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/memory/allocator.c`): Memory subsystem. It provides monotonic arena
 * allocation or precise generational collection. Size and cursor variables are overflow-checked
 * before arithmetic, and ownership transitions are centralized so cleanup remains
 * deterministic.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "allocator.h"
#include "memory.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

enum {
    OASP_GC_MARKED = 1u << 0,
    OASP_GC_OLD = 1u << 1,
    OASP_GC_REMEMBERED = 1u << 2,
    OASP_GC_FINALIZED = 1u << 3
};

typedef struct oasp_gc_header {
    struct oasp_gc_header *next;
    struct oasp_gc_header *hash_next;
    oasp_allocator_trace_fn trace;
    oasp_finalizer_fn finalizer;
    void *finalizer_context;
    void *payload;
    size_t size;
    size_t allocation_size;
    size_t external_bytes;
    unsigned flags;
    unsigned age;
} oasp_gc_header;

typedef enum {
    OASP_GC_IDLE,
    OASP_GC_MAJOR_MARK
} oasp_gc_phase;

struct oasp_allocator {
    oasp_allocator_kind kind;
    oasp_arena *arena;
    oasp_gc_config config;
    oasp_gc_stats stats;
    oasp_gc_header *objects;
    oasp_gc_header **buckets;
    size_t bucket_count;
    oasp_gc_header **mark_stack;
    size_t mark_count;
    size_t mark_capacity;
    oasp_allocator_roots_fn roots;
    void *roots_context;
    oasp_gc_phase phase;
    bool collecting_minor;
    bool collection_failed;
    bool failed;
};

static oasp_gc_config oasp_gc_default_config(void) {
    oasp_gc_config config = {0};
    config.heap_limit = 256u * 1024u * 1024u;
    config.young_limit = 4u * 1024u * 1024u;
    config.major_threshold = 32u * 1024u * 1024u;
    config.major_step = 256;
    config.large_object_threshold = 64u * 1024u;
    config.promotion_age = 2;
    config.diagnostics = false;
    return config;
}

static size_t oasp_pointer_hash(const void *pointer) {
    uintptr_t value = (uintptr_t)pointer;
    value >>= 4;
    value ^= value >> 17;
    value *= UINT64_C(0xed5ad4bb);
    value ^= value >> 11;
    return (size_t)value;
}

static oasp_gc_header *oasp_gc_find(const oasp_allocator *allocator, const void *payload) {
    if (!allocator || allocator->kind != OASP_ALLOCATOR_GC || !payload || !allocator->bucket_count)
        return NULL;
    size_t bucket = oasp_pointer_hash(payload) & (allocator->bucket_count - 1);
    for (oasp_gc_header *header = allocator->buckets[bucket]; header; header = header->hash_next)
        if (header->payload == payload)
            return header;
    return NULL;
}

static bool oasp_gc_resize_buckets(oasp_allocator *allocator, size_t count) {
    oasp_gc_header **buckets = calloc(count, sizeof(*buckets));
    if (!buckets)
        return false;
    for (oasp_gc_header *header = allocator->objects; header; header = header->next) {
        size_t bucket = oasp_pointer_hash(header->payload) & (count - 1);
        header->hash_next = buckets[bucket];
        buckets[bucket] = header;
    }
    free(allocator->buckets);
    allocator->buckets = buckets;
    allocator->bucket_count = count;
    return true;
}

static void oasp_gc_remove_bucket(oasp_allocator *allocator, oasp_gc_header *header) {
    size_t bucket = oasp_pointer_hash(header->payload) & (allocator->bucket_count - 1);
    oasp_gc_header **link = &allocator->buckets[bucket];
    while (*link && *link != header)
        link = &(*link)->hash_next;
    if (*link)
        *link = header->hash_next;
}

static bool oasp_gc_push(oasp_allocator *allocator, oasp_gc_header *header) {
    if (allocator->mark_count == allocator->mark_capacity) {
        size_t capacity = allocator->mark_capacity ? allocator->mark_capacity * 2 : 256;
        if (capacity < allocator->mark_capacity ||
            capacity > SIZE_MAX / sizeof(*allocator->mark_stack))
            return false;
        oasp_gc_header **stack = realloc(allocator->mark_stack, capacity * sizeof(*stack));
        if (!stack)
            return false;
        allocator->mark_stack = stack;
        allocator->mark_capacity = capacity;
    }
    allocator->mark_stack[allocator->mark_count++] = header;
    return true;
}

void oasp_allocator_trace(oasp_allocator *allocator, const void *object) {
    oasp_gc_header *header = oasp_gc_find(allocator, object);
    if (!header || (header->flags & OASP_GC_MARKED))
        return;
    if (!oasp_gc_push(allocator, header)) {
        allocator->collection_failed = true;
        allocator->failed = true;
        allocator->stats.allocation_failures++;
        return;
    }
    header->flags |= OASP_GC_MARKED;
}

static void oasp_gc_trace_roots(oasp_allocator *allocator, bool minor) {
    allocator->collecting_minor = minor;
    if (allocator->roots)
        allocator->roots(allocator, allocator->roots_context);
    if (minor) {
        for (oasp_gc_header *header = allocator->objects; header; header = header->next)
            if ((header->flags & (OASP_GC_OLD | OASP_GC_REMEMBERED)) ==
                (OASP_GC_OLD | OASP_GC_REMEMBERED))
                oasp_allocator_trace(allocator, header->payload);
    }
}

static size_t oasp_gc_drain(oasp_allocator *allocator, size_t budget) {
    size_t work = 0;
    while (allocator->mark_count && work < budget) {
        oasp_gc_header *header = allocator->mark_stack[--allocator->mark_count];
        if (header->trace)
            header->trace(allocator, header->payload);
        work++;
    }
    return work;
}

static bool oasp_gc_is_unreachable(const oasp_gc_header *header, bool major) {
    return !(header->flags & OASP_GC_MARKED) && (major || !(header->flags & OASP_GC_OLD));
}

static bool oasp_gc_run_finalizers(oasp_allocator *allocator, bool major) {
    bool ran = false;
    for (oasp_gc_header *header = allocator->objects; header; header = header->next) {
        if (!oasp_gc_is_unreachable(header, major) || !header->finalizer ||
            (header->flags & OASP_GC_FINALIZED))
            continue;
        header->flags |= OASP_GC_FINALIZED;
        header->finalizer(header->finalizer_context);
        allocator->stats.finalized_objects++;
        ran = true;
    }
    return ran;
}

static void oasp_gc_sweep(oasp_allocator *allocator, bool major) {
    oasp_gc_header **link = &allocator->objects;
    while (*link) {
        oasp_gc_header *header = *link;
        bool dead = oasp_gc_is_unreachable(header, major);
        if (dead) {
            *link = header->next;
            oasp_gc_remove_bucket(allocator, header);
            allocator->stats.heap_bytes -= header->allocation_size;
            allocator->stats.external_bytes -= header->external_bytes;
            if (header->flags & OASP_GC_OLD)
                allocator->stats.old_bytes -= header->allocation_size;
            else
                allocator->stats.young_bytes -= header->allocation_size;
            allocator->stats.object_count--;
            oasp_memory_release(allocator->config.memory, header->allocation_size + header->external_bytes);
            free(header);
            continue;
        }
        if (!(header->flags & OASP_GC_OLD)) {
            if (major || ++header->age >= allocator->config.promotion_age ||
                header->size >= allocator->config.large_object_threshold) {
                header->flags |= OASP_GC_OLD;
                allocator->stats.young_bytes -= header->allocation_size;
                allocator->stats.old_bytes += header->allocation_size;
                allocator->stats.promoted_objects++;
            }
        }
        header->flags &= ~OASP_GC_MARKED;
        if (major)
            header->flags &= ~OASP_GC_REMEMBERED;
        link = &header->next;
    }
}

static void oasp_gc_abort_collection(oasp_allocator *allocator) {
    for (oasp_gc_header *header = allocator->objects; header; header = header->next)
        header->flags &= ~OASP_GC_MARKED;
    allocator->mark_count = 0;
    allocator->collecting_minor = false;
    allocator->collection_failed = false;
    allocator->phase = OASP_GC_IDLE;
}

static void oasp_gc_finish_collection(oasp_allocator *allocator, bool major) {
    if (allocator->collection_failed) {
        oasp_gc_abort_collection(allocator);
        return;
    }
    /*
     * Mutators run between incremental mark slices. Write barriers preserve
     * heap edges, while this short final remark also catches roots replaced
     * since the initial snapshot.
     */
    if (major && allocator->phase == OASP_GC_MAJOR_MARK) {
        for (oasp_gc_header *header = allocator->objects; header; header = header->next)
            header->flags &= ~OASP_GC_MARKED;
        allocator->mark_count = 0;
        oasp_gc_trace_roots(allocator, false);
        (void)oasp_gc_drain(allocator, SIZE_MAX);
        if (allocator->collection_failed) {
            oasp_gc_abort_collection(allocator);
            return;
        }
    }
    if (oasp_gc_run_finalizers(allocator, major)) {
        for (oasp_gc_header *header = allocator->objects; header; header = header->next)
            header->flags &= ~OASP_GC_MARKED;
        allocator->mark_count = 0;
        oasp_gc_trace_roots(allocator, major ? false : true);
        (void)oasp_gc_drain(allocator, SIZE_MAX);
        if (allocator->collection_failed) {
            oasp_gc_abort_collection(allocator);
            return;
        }
    }
    oasp_gc_sweep(allocator, major);
    allocator->collecting_minor = false;
    allocator->phase = OASP_GC_IDLE;
    if (major)
        allocator->stats.major_collections++;
    else
        allocator->stats.minor_collections++;
}

oasp_allocator *oasp_allocator_create_arena(oasp_arena *arena) {
    if (!arena)
        return NULL;
    oasp_allocator *allocator = calloc(1, sizeof(*allocator));
    if (!allocator)
        return NULL;
    allocator->kind = OASP_ALLOCATOR_ARENA;
    allocator->arena = arena;
    return allocator;
}

oasp_allocator *oasp_allocator_create_gc(const oasp_gc_config *configuration) {
    oasp_allocator *allocator = calloc(1, sizeof(*allocator));
    if (!allocator)
        return NULL;
    allocator->kind = OASP_ALLOCATOR_GC;
    allocator->config = oasp_gc_default_config();
    if (configuration) {
        if (configuration->heap_limit)
            allocator->config.heap_limit = configuration->heap_limit;
        if (configuration->young_limit)
            allocator->config.young_limit = configuration->young_limit;
        if (configuration->major_threshold)
            allocator->config.major_threshold = configuration->major_threshold;
        if (configuration->major_step)
            allocator->config.major_step = configuration->major_step;
        if (configuration->large_object_threshold)
            allocator->config.large_object_threshold = configuration->large_object_threshold;
        if (configuration->promotion_age)
            allocator->config.promotion_age = configuration->promotion_age;
        allocator->config.diagnostics = configuration->diagnostics;
        allocator->config.memory = configuration->memory;
    }
    if (!oasp_gc_resize_buckets(allocator, 1024)) {
        free(allocator);
        return NULL;
    }
    return allocator;
}

oasp_allocator_kind oasp_allocator_kind_of(const oasp_allocator *allocator) {
    return allocator ? allocator->kind : OASP_ALLOCATOR_ARENA;
}

void oasp_allocator_set_roots(oasp_allocator *allocator, oasp_allocator_roots_fn function, void *context) {
    if (!allocator)
        return;
    allocator->roots = function;
    allocator->roots_context = context;
}

void *oasp_allocator_alloc(oasp_allocator *allocator, size_t size, size_t alignment, oasp_allocator_trace_fn trace) {
    if (!allocator || !size)
        return NULL;
    if (allocator->kind == OASP_ALLOCATOR_ARENA)
        return oasp_arena_alloc(allocator->arena, size, alignment);
    if (!alignment || (alignment & (alignment - 1)) || alignment > alignof(max_align_t))
        return NULL;
    if (size > SIZE_MAX - sizeof(oasp_gc_header) - alignof(max_align_t))
        return NULL;
    size_t allocation_size = sizeof(oasp_gc_header) + size + alignof(max_align_t);
    size_t total = allocator->stats.heap_bytes + allocator->stats.external_bytes;
    if (allocation_size > allocator->config.heap_limit ||
        total > allocator->config.heap_limit - allocation_size) {
        allocator->failed = true;
        allocator->stats.allocation_failures++;
        return NULL;
    }
    if (!oasp_memory_reserve(allocator->config.memory, allocation_size, "managed heap")) {
        allocator->failed = true;
        allocator->stats.allocation_failures++;
        return NULL;
    }
    oasp_gc_header *header = calloc(1, allocation_size);
    if (!header) {
        oasp_memory_release(allocator->config.memory, allocation_size);
        allocator->failed = true;
        allocator->stats.allocation_failures++;
        return NULL;
    }
    uintptr_t start = (uintptr_t)(header + 1);
    uintptr_t payload = (start + alignment - 1) & ~(uintptr_t)(alignment - 1);
    header->payload = (void *)payload;
    header->size = size;
    header->allocation_size = allocation_size;
    header->trace = trace;
    if (size >= allocator->config.large_object_threshold)
        header->flags |= OASP_GC_OLD;
    if (allocator->phase == OASP_GC_MAJOR_MARK)
        header->flags |= OASP_GC_MARKED;
    header->next = allocator->objects;
    allocator->objects = header;
    size_t bucket = oasp_pointer_hash(header->payload) & (allocator->bucket_count - 1);
    header->hash_next = allocator->buckets[bucket];
    allocator->buckets[bucket] = header;
    allocator->stats.heap_bytes += allocation_size;
    if (header->flags & OASP_GC_OLD)
        allocator->stats.old_bytes += allocation_size;
    else
        allocator->stats.young_bytes += allocation_size;
    allocator->stats.object_count++;
    size_t committed = allocator->stats.heap_bytes + allocator->stats.external_bytes;
    if (committed > allocator->stats.peak_bytes)
        allocator->stats.peak_bytes = committed;
    return header->payload;
}

char *oasp_allocator_copy(oasp_allocator *allocator, const char *data, size_t length) {
    if (length == SIZE_MAX)
        return NULL;
    char *copy = oasp_allocator_alloc(allocator, length + 1, alignof(char), NULL);
    if (!copy)
        return NULL;
    if (length)
        memcpy(copy, data, length);
    copy[length] = '\0';
    return copy;
}

bool oasp_allocator_set_finalizer(oasp_allocator *allocator, void *object, oasp_finalizer_fn function, void *context) {
    if (!allocator || !function)
        return false;
    if (allocator->kind == OASP_ALLOCATOR_ARENA)
        return oasp_arena_defer(allocator->arena, function, context) != 0;
    oasp_gc_header *header = oasp_gc_find(allocator, object);
    if (!header)
        return false;
    header->finalizer = function;
    header->finalizer_context = context;
    return true;
}

bool oasp_allocator_account_external(oasp_allocator *allocator, void *owner, ptrdiff_t delta) {
    if (!allocator || allocator->kind != OASP_ALLOCATOR_GC)
        return true;
    oasp_gc_header *header = oasp_gc_find(allocator, owner);
    if (!header)
        return false;
    if (delta < 0) {
        size_t amount = (size_t)(-(delta + 1)) + 1;
        if (amount > header->external_bytes)
            amount = header->external_bytes;
        header->external_bytes -= amount;
        allocator->stats.external_bytes -= amount;
        oasp_memory_release(allocator->config.memory, amount);
        return true;
    }
    size_t amount = (size_t)delta;
    size_t committed = allocator->stats.heap_bytes + allocator->stats.external_bytes;
    if (amount > allocator->config.heap_limit ||
        committed > allocator->config.heap_limit - amount) {
        allocator->failed = true;
        allocator->stats.allocation_failures++;
        return false;
    }
    if (!oasp_memory_reserve(allocator->config.memory, amount, "external allocation")) {
        allocator->failed = true;
        allocator->stats.allocation_failures++;
        return false;
    }
    header->external_bytes += amount;
    allocator->stats.external_bytes += amount;
    committed += amount;
    if (committed > allocator->stats.peak_bytes)
        allocator->stats.peak_bytes = committed;
    return true;
}

size_t oasp_allocator_object_size(const oasp_allocator *allocator, const void *object) {
    oasp_gc_header *header = oasp_gc_find(allocator, object);
    return header ? header->size : 0;
}

void oasp_allocator_write_barrier(oasp_allocator *allocator, void *owner, const void *value) {
    if (!allocator || allocator->kind != OASP_ALLOCATOR_GC)
        return;
    oasp_gc_header *owner_header = oasp_gc_find(allocator, owner);
    oasp_gc_header *value_header = oasp_gc_find(allocator, value);
    if (!owner_header || !value_header)
        return;
    if ((owner_header->flags & OASP_GC_OLD) && !(value_header->flags & OASP_GC_OLD))
        owner_header->flags |= OASP_GC_REMEMBERED;
    if (allocator->phase == OASP_GC_MAJOR_MARK &&
        (owner_header->flags & OASP_GC_MARKED) &&
        !(value_header->flags & OASP_GC_MARKED))
        oasp_allocator_trace(allocator, value);
}

void oasp_allocator_collect(oasp_allocator *allocator, bool major) {
    if (!allocator || allocator->kind != OASP_ALLOCATOR_GC)
        return;
    allocator->phase = OASP_GC_IDLE;
    allocator->mark_count = 0;
    allocator->collection_failed = false;
    for (oasp_gc_header *header = allocator->objects; header; header = header->next)
        header->flags &= ~OASP_GC_MARKED;
    oasp_gc_trace_roots(allocator, !major);
    (void)oasp_gc_drain(allocator, SIZE_MAX);
    oasp_gc_finish_collection(allocator, major);
}

void oasp_allocator_safepoint(oasp_allocator *allocator) {
    if (!allocator || allocator->kind != OASP_ALLOCATOR_GC)
        return;
    if (allocator->stats.young_bytes >= allocator->config.young_limit)
        oasp_allocator_collect(allocator, false);
    if (allocator->phase == OASP_GC_IDLE &&
        allocator->stats.old_bytes + allocator->stats.external_bytes >=
            allocator->config.major_threshold) {
        allocator->mark_count = 0;
        allocator->collection_failed = false;
        for (oasp_gc_header *header = allocator->objects; header; header = header->next)
            header->flags &= ~OASP_GC_MARKED;
        allocator->phase = OASP_GC_MAJOR_MARK;
        oasp_gc_trace_roots(allocator, false);
    }
    if (allocator->phase == OASP_GC_MAJOR_MARK) {
        (void)oasp_gc_drain(allocator, allocator->config.major_step);
        if (allocator->collection_failed) {
            oasp_gc_abort_collection(allocator);
            return;
        }
        if (!allocator->mark_count)
            oasp_gc_finish_collection(allocator, true);
    }
}

bool oasp_allocator_failed(const oasp_allocator *allocator) {
    return allocator && allocator->failed;
}

void oasp_allocator_get_stats(const oasp_allocator *allocator, oasp_gc_stats *stats) {
    if (!stats)
        return;
    memset(stats, 0, sizeof(*stats));
    if (allocator && allocator->kind == OASP_ALLOCATOR_GC)
        *stats = allocator->stats;
}

void oasp_allocator_dump(const oasp_allocator *allocator, FILE *stream) {
    if (!allocator || allocator->kind != OASP_ALLOCATOR_GC || !stream)
        return;
    fprintf(stream,
            "OpenASP GC: heap=%zu external=%zu young=%zu old=%zu objects=%zu "
            "minor=%zu major=%zu promoted=%zu finalized=%zu peak=%zu limit=%zu "
            "allocation_failures=%zu\n",
            allocator->stats.heap_bytes, allocator->stats.external_bytes,
            allocator->stats.young_bytes, allocator->stats.old_bytes,
            allocator->stats.object_count, allocator->stats.minor_collections,
            allocator->stats.major_collections, allocator->stats.promoted_objects,
            allocator->stats.finalized_objects, allocator->stats.peak_bytes,
            allocator->config.heap_limit, allocator->stats.allocation_failures);
}

void oasp_allocator_destroy(oasp_allocator *allocator) {
    if (!allocator)
        return;
    if (allocator->kind == OASP_ALLOCATOR_GC) {
        for (oasp_gc_header *header = allocator->objects; header; header = header->next) {
            if (header->finalizer && !(header->flags & OASP_GC_FINALIZED)) {
                header->flags |= OASP_GC_FINALIZED;
                header->finalizer(header->finalizer_context);
                allocator->stats.finalized_objects++;
            }
        }
        oasp_gc_header *header = allocator->objects;
        while (header) {
            oasp_gc_header *next = header->next;
            oasp_memory_release(allocator->config.memory, header->allocation_size + header->external_bytes);
            free(header);
            header = next;
        }
        free(allocator->mark_stack);
        free(allocator->buckets);
    }
    free(allocator);
}
