// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Fork-safe immutable module cache. The parent allocates shared metadata and
 * bytes before workers use process-shared locking to acquire or publish
 * versioned entries. Optional disk storage is a secondary validated source,
 * never trusted as executable bytecode without the compiler image checks.
 */

#ifndef OPENASP_SHARED_CACHE_H
#define OPENASP_SHARED_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct oasp_shared_cache oasp_shared_cache;

typedef struct {
    /* Borrowed immutable bytes pinned until oasp_shared_cache_release(). */
    const void *data;
    size_t size;
    /* Version identifies path content; generation tracks global publication. */
    uint64_t version;
    uint64_t generation;
    uint32_t entry;
    uint32_t reader_slot;
} oasp_shared_cache_view;

/*
 * Creates cache state with a hard shared-memory byte `capacity`. The optional
 * disk directory is copied. NULL is returned for invalid sizes or setup errors.
 */
oasp_shared_cache *oasp_shared_cache_create(size_t capacity, const char *disk_directory);

/* Controls how often unchanged filesystem metadata may be trusted. */
void oasp_shared_cache_set_validation_interval(oasp_shared_cache *cache, long seconds);
long oasp_shared_cache_validation_interval(const oasp_shared_cache *cache);
/* Selects whether newly compiled images carry normalized ASP debug source. */
void oasp_shared_cache_set_debug_info(oasp_shared_cache *cache, bool enabled);
bool oasp_shared_cache_debug_info(const oasp_shared_cache *cache);
void oasp_shared_cache_destroy(oasp_shared_cache *cache);

/*
 * Pins the newest entry for `path`. `observed_version` allows callers to avoid
 * recompiling after another worker wins publication. A successful view must be
 * released exactly once, including when subsequent image decoding fails.
 */
bool oasp_shared_cache_acquire(oasp_shared_cache *cache, const char *path, oasp_shared_cache_view *view, uint64_t *observed_version);
void oasp_shared_cache_release(oasp_shared_cache *cache, oasp_shared_cache_view *view);

/* Reclaims leases left by a worker after the supervisor has reaped it. */
void oasp_shared_cache_recover_process(oasp_shared_cache *cache, int64_t process_id);

/*
 * Publishes `data[0..size)` only if the path still has `expected_version`;
 * writes the winning version on success or race. Input bytes are copied.
 */
bool oasp_shared_cache_publish(oasp_shared_cache *cache, const char *path, const void *data, size_t size, uint64_t expected_version,
                               uint64_t *published_version);

/* Loads a heap-owned disk image; caller frees `*data` after validation/use. */
bool oasp_shared_cache_disk_load(oasp_shared_cache *cache, const char *path, void **data, size_t *size);
uint64_t oasp_shared_cache_generation(const oasp_shared_cache *cache);

#endif
