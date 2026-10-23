// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/memory/allocator.h`): Memory subsystem. It provides monotonic arena
 * allocation or precise generational collection. Size and cursor variables are overflow-checked
 * before arithmetic, and ownership transitions are centralized so cleanup remains
 * deterministic.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_ALLOCATOR_H
#define OPENASP_ALLOCATOR_H

#include "openasp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct oasp_allocator oasp_allocator;
typedef struct oasp_memory_manager oasp_memory_manager;

/*
 * GC callbacks receive the active allocator. Trace functions mark references
 * held by one managed object; root callbacks mark references held by runtime
 * stacks and native state. Callback contexts are borrowed.
 */
typedef void (*oasp_allocator_trace_fn)(oasp_allocator *allocator, void *object);
typedef void (*oasp_allocator_roots_fn)(oasp_allocator *allocator, void *context);

typedef enum {
    OASP_ALLOCATOR_ARENA,
    OASP_ALLOCATOR_GC
} oasp_allocator_kind;

typedef struct {
    /* Combined managed and externally-accounted hard limit; zero uses default. */
    size_t heap_limit;
    /* Young-generation target that triggers minor collection. */
    size_t young_limit;
    /* Old-generation growth that schedules an incremental major cycle. */
    size_t major_threshold;
    /* Number of old objects traced at each major safepoint. */
    size_t major_step;
    /* Objects at or above this size bypass the young generation. */
    size_t large_object_threshold;
    /* Survived minor collections required before promotion. */
    unsigned promotion_age;
    /* Emit final counters when the allocator is destroyed. */
    bool diagnostics;
    /* Optional request-wide budget shared with arenas and external storage. */
    oasp_memory_manager *memory;
} oasp_gc_config;

/* Snapshot counters used for diagnostics and regression tests. */
typedef struct {
    size_t heap_bytes;
    size_t external_bytes;
    size_t young_bytes;
    size_t old_bytes;
    size_t object_count;
    size_t minor_collections;
    size_t major_collections;
    size_t promoted_objects;
    size_t finalized_objects;
    size_t peak_bytes;
    size_t allocation_failures;
} oasp_gc_stats;

/*
 * Creates either a non-owning arena adapter or an owning generational
 * allocator. GC configuration is copied and normalized; NULL uses defaults.
 */
oasp_allocator *oasp_allocator_create_arena(oasp_arena *arena);
oasp_allocator *oasp_allocator_create_gc(const oasp_gc_config *config);
void oasp_allocator_destroy(oasp_allocator *allocator);
oasp_allocator_kind oasp_allocator_kind_of(const oasp_allocator *allocator);

/*
 * Allocation returns aligned zeroed payloads. `trace` describes payload
 * references and may be NULL. Copies include a trailing NUL byte for C APIs.
 */
void oasp_allocator_set_roots(oasp_allocator *allocator, oasp_allocator_roots_fn function, void *context);
void *oasp_allocator_alloc(oasp_allocator *allocator, size_t size, size_t alignment, oasp_allocator_trace_fn trace);
char *oasp_allocator_copy(oasp_allocator *allocator, const char *data, size_t length);

/*
 * Native ownership hooks. Finalizers run exactly once before reclamation;
 * external accounting applies a signed byte delta to `owner`; write barriers
 * must be called when an old object starts referencing a young object.
 */
bool oasp_allocator_set_finalizer(oasp_allocator *allocator, void *object, oasp_finalizer_fn function, void *context);
bool oasp_allocator_account_external(oasp_allocator *allocator, void *owner, ptrdiff_t delta);
void oasp_allocator_note_external_pressure(oasp_allocator *allocator, size_t bytes);
size_t oasp_allocator_object_size(const oasp_allocator *allocator, const void *object);
void oasp_allocator_write_barrier(oasp_allocator *allocator, void *owner, const void *value);
void oasp_allocator_trace(oasp_allocator *allocator, const void *object);

/*
 * Safepoints advance pending incremental work. Explicit collection completes a
 * minor or major cycle synchronously. Statistics and failure state are
 * read-only snapshots and remain valid until allocator destruction.
 */
void oasp_allocator_safepoint(oasp_allocator *allocator);
void oasp_allocator_collect(oasp_allocator *allocator, bool major);
bool oasp_allocator_failed(const oasp_allocator *allocator);
void oasp_allocator_get_stats(const oasp_allocator *allocator, oasp_gc_stats *stats);
void oasp_allocator_dump(const oasp_allocator *allocator, FILE *stream);

#endif
