// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/storage/shared_cache.c`): Persistent/shared storage. It validates
 * binary formats and versions before exposing borrowed views, and coordinates process-safe
 * publication or state updates. Offset, size, generation, and lock variables protect
 * consistency across workers.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "shared_cache.h"

#include "module_image.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OASP_CACHE_MAGIC UINT64_C(0x4f41535053484d31)
#define OASP_CACHE_BUCKETS 257u
#define OASP_CACHE_ENTRIES 1024u
#define OASP_CACHE_READER_SLOTS 256u
#define OASP_CACHE_NONE UINT32_MAX
#define OASP_CACHE_READY 1u
#define OASP_CACHE_RETIRED 2u

typedef struct {
    atomic_uint status;
    atomic_uint references;
    uint32_t next;
    uint32_t path_offset;
    uint32_t path_length;
    uint32_t image_offset;
    uint32_t image_length;
    uint64_t hash;
    uint64_t version;
    uint64_t generation;
    int64_t modified_seconds;
    int64_t modified_nanoseconds;
    int64_t file_size;
} oasp_shared_entry;

typedef struct {
    atomic_int process_id;
    atomic_uint references;
} oasp_shared_reader;

typedef struct {
    uint64_t magic;
    uint64_t capacity;
    atomic_uint_fast64_t generation;
    atomic_uint_fast64_t next_version;
    atomic_uint readers;
    uint32_t entry_count;
    uint32_t arena_offset;
    uint32_t arena_used;
    uint32_t buckets[OASP_CACHE_BUCKETS];
    oasp_shared_reader reader_slots[OASP_CACHE_READER_SLOTS];
    oasp_shared_entry entries[OASP_CACHE_ENTRIES];
} oasp_shared_header;

struct oasp_shared_cache {
    int fd;
    unsigned char *mapping;
    size_t capacity;
    char *disk_directory;
    long validation_interval;
    bool debug_info;
    time_t validated_at[OASP_CACHE_ENTRIES];
};

static uint64_t oasp_cache_hash(const char *text) {
    uint64_t hash = UINT64_C(1469598103934665603);
    while (*text) {
        hash ^= (unsigned char)*text++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static size_t oasp_cache_align(size_t value) { return (value + 7u) & ~(size_t)7u; }

static bool oasp_cache_lock(oasp_shared_cache *cache, short type) {
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    for (;;) {
        if (fcntl(cache->fd, F_SETLKW, &lock) == 0)
            return true;
        if (errno != EINTR)
            return false;
    }
}

static void oasp_cache_unlock(oasp_shared_cache *cache) {
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    (void)fcntl(cache->fd, F_SETLK, &lock);
}

static oasp_shared_header *oasp_cache_header(oasp_shared_cache *cache) { return (oasp_shared_header *)cache->mapping; }

static const char *oasp_cache_entry_path(const oasp_shared_cache *cache, const oasp_shared_entry *entry) {
    return (const char *)cache->mapping + entry->path_offset;
}

static bool oasp_cache_entry_matches(const oasp_shared_cache *cache, const oasp_shared_entry *entry, const char *path, uint64_t hash) {
    size_t length = strlen(path);
    return entry->hash == hash && entry->path_length == length && entry->path_offset <= cache->capacity && length <= cache->capacity - entry->path_offset &&
           !memcmp(oasp_cache_entry_path(cache, entry), path, length);
}

static uint32_t oasp_cache_find_locked(oasp_shared_cache *cache, const char *path, uint64_t hash) {
    oasp_shared_header *header = oasp_cache_header(cache);
    uint32_t index = header->buckets[hash % OASP_CACHE_BUCKETS];
    while (index != OASP_CACHE_NONE) {
        if (index >= header->entry_count)
            return OASP_CACHE_NONE;
        oasp_shared_entry *entry = &header->entries[index];
        if (atomic_load_explicit(&entry->status, memory_order_acquire) == OASP_CACHE_READY && oasp_cache_entry_matches(cache, entry, path, hash))
            return index;
        index = entry->next;
    }
    return OASP_CACHE_NONE;
}

static void oasp_cache_initialize(oasp_shared_header *header, size_t capacity, uint64_t generation) {
    memset(header, 0, sizeof(*header));
    header->magic = OASP_CACHE_MAGIC;
    header->capacity = capacity;
    atomic_init(&header->generation, generation);
    atomic_init(&header->next_version, 1);
    atomic_init(&header->readers, 0);
    for (size_t i = 0; i < OASP_CACHE_READER_SLOTS; i++) {
        atomic_init(&header->reader_slots[i].process_id, 0);
        atomic_init(&header->reader_slots[i].references, 0);
    }
    header->arena_offset = (uint32_t)oasp_cache_align(sizeof(*header));
    header->arena_used = header->arena_offset;
    for (size_t i = 0; i < OASP_CACHE_BUCKETS; i++)
        header->buckets[i] = OASP_CACHE_NONE;
}

oasp_shared_cache *oasp_shared_cache_create(size_t capacity, const char *disk_directory) {
    size_t minimum = oasp_cache_align(sizeof(oasp_shared_header)) + 65536;
    if (capacity < minimum)
        capacity = minimum;
    if (capacity > UINT32_MAX)
        return NULL;
    char temporary[] = "/tmp/openasp-shm-XXXXXX";
    int fd = mkstemp(temporary);
    if (fd < 0)
        return NULL;
    (void)unlink(temporary);
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (ftruncate(fd, (off_t)capacity)) {
        close(fd);
        return NULL;
    }
    void *mapping = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    oasp_shared_cache *cache = calloc(1, sizeof(*cache));
    if (!cache) {
        munmap(mapping, capacity);
        close(fd);
        return NULL;
    }
    cache->fd = fd;
    cache->mapping = mapping;
    cache->capacity = capacity;
    if (disk_directory && *disk_directory) {
        cache->disk_directory = strdup(disk_directory);
        if (!cache->disk_directory || (mkdir(cache->disk_directory, 0700) && errno != EEXIST)) {
            oasp_shared_cache_destroy(cache);
            return NULL;
        }
    }
    oasp_cache_initialize(mapping, capacity, 1);
    return cache;
}

void oasp_shared_cache_set_validation_interval(oasp_shared_cache *cache, long seconds) {
    if (cache)
        cache->validation_interval = seconds;
}

long oasp_shared_cache_validation_interval(const oasp_shared_cache *cache) {
    return cache ? cache->validation_interval : 0;
}

void oasp_shared_cache_set_debug_info(oasp_shared_cache *cache, bool enabled) {
    if (cache)
        cache->debug_info = enabled;
}

bool oasp_shared_cache_debug_info(const oasp_shared_cache *cache) {
    return cache && cache->debug_info;
}

void oasp_shared_cache_destroy(oasp_shared_cache *cache) {
    if (!cache)
        return;
    if (cache->mapping)
        munmap(cache->mapping, cache->capacity);
    if (cache->fd >= 0)
        close(cache->fd);
    free(cache->disk_directory);
    free(cache);
}

bool oasp_shared_cache_acquire(oasp_shared_cache *cache, const char *path, oasp_shared_cache_view *view, uint64_t *observed_version) {
    if (view) {
        memset(view, 0, sizeof(*view));
        view->reader_slot = OASP_CACHE_NONE;
    }
    if (observed_version)
        *observed_version = 0;
    if (!cache || !path || !view || !oasp_cache_lock(cache, F_RDLCK))
        return false;
    oasp_shared_header *header = oasp_cache_header(cache);
    uint64_t hash = oasp_cache_hash(path);
    uint32_t index = oasp_cache_find_locked(cache, path, hash);
    if (index == OASP_CACHE_NONE) {
        oasp_cache_unlock(cache);
        return false;
    }
    oasp_shared_entry *entry = &header->entries[index];
    if (observed_version)
        *observed_version = entry->version;
    if (entry->image_offset > cache->capacity || entry->image_length > cache->capacity - entry->image_offset) {
        oasp_cache_unlock(cache);
        return false;
    }
    int process_id = (int)getpid();
    uint32_t reader_slot = OASP_CACHE_NONE;
    for (uint32_t i = 0; i < OASP_CACHE_READER_SLOTS; i++) {
        int owner = atomic_load_explicit(&header->reader_slots[i].process_id, memory_order_acquire);
        if (owner == process_id) {
            reader_slot = i;
            break;
        }
        if (owner == 0 && atomic_compare_exchange_strong_explicit(&header->reader_slots[i].process_id, &owner, process_id,
                                                                  memory_order_acq_rel, memory_order_acquire)) {
            reader_slot = i;
            break;
        }
    }
    if (reader_slot == OASP_CACHE_NONE) {
        oasp_cache_unlock(cache);
        return false;
    }
    atomic_fetch_add_explicit(&entry->references, 1, memory_order_acq_rel);
    atomic_fetch_add_explicit(&header->reader_slots[reader_slot].references, 1, memory_order_acq_rel);
    atomic_fetch_add_explicit(&header->readers, 1, memory_order_acq_rel);
    view->data = cache->mapping + entry->image_offset;
    view->size = entry->image_length;
    view->version = entry->version;
    view->generation = entry->generation;
    view->entry = index;
    view->reader_slot = reader_slot;
    oasp_cache_unlock(cache);
    time_t now = time(NULL);
    bool validate = cache->validation_interval == 0 ||
                    (cache->validation_interval > 0 &&
                     (cache->validated_at[index] == 0 ||
                      now - cache->validated_at[index] >= cache->validation_interval));
    if (validate && !oasp_module_image_fresh(view->data, view->size, path)) {
        oasp_shared_cache_release(cache, view);
        return false;
    }
    if (validate)
        cache->validated_at[index] = now;
    return true;
}

void oasp_shared_cache_release(oasp_shared_cache *cache, oasp_shared_cache_view *view) {
    if (!cache || !view || !view->data)
        return;
    oasp_shared_header *header = oasp_cache_header(cache);
    if (!oasp_cache_lock(cache, F_RDLCK))
        return;
    if (view->entry < OASP_CACHE_ENTRIES)
        atomic_fetch_sub_explicit(&header->entries[view->entry].references, 1, memory_order_acq_rel);
    if (view->reader_slot < OASP_CACHE_READER_SLOTS)
        atomic_fetch_sub_explicit(&header->reader_slots[view->reader_slot].references, 1, memory_order_acq_rel);
    atomic_fetch_sub_explicit(&header->readers, 1, memory_order_acq_rel);
    oasp_cache_unlock(cache);
    memset(view, 0, sizeof(*view));
    view->reader_slot = OASP_CACHE_NONE;
}

void oasp_shared_cache_recover_process(oasp_shared_cache *cache, int64_t process_id) {
    if (!cache || process_id <= 0 || process_id > INT_MAX || !oasp_cache_lock(cache, F_WRLCK))
        return;
    oasp_shared_header *header = oasp_cache_header(cache);
    for (size_t i = 0; i < OASP_CACHE_READER_SLOTS; i++) {
        oasp_shared_reader *reader = &header->reader_slots[i];
        if (atomic_load_explicit(&reader->process_id, memory_order_acquire) != (int)process_id)
            continue;
        unsigned references = atomic_exchange_explicit(&reader->references, 0, memory_order_acq_rel);
        atomic_store_explicit(&reader->process_id, 0, memory_order_release);
        if (references)
            atomic_fetch_sub_explicit(&header->readers, references, memory_order_acq_rel);
        break;
    }
    oasp_cache_unlock(cache);
}

static bool oasp_cache_file_path(const oasp_shared_cache *cache, const char *path, char output[4096]) {
    if (!cache->disk_directory)
        return false;
    int length = snprintf(output, 4096, "%s/%016llx.oac", cache->disk_directory, (unsigned long long)oasp_cache_hash(path));
    return length > 0 && length < 4096;
}

static void oasp_cache_disk_store(oasp_shared_cache *cache, const char *path, const void *data, size_t size) {
    char destination[4096];
    if (!oasp_cache_file_path(cache, path, destination))
        return;
    char temporary[4096];
    int length = snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", destination, (long)getpid());
    if (length <= 0 || (size_t)length >= sizeof(temporary))
        return;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    const unsigned char *cursor = data;
    size_t remaining = size;
    while (remaining) {
        ssize_t written = write(fd, cursor, remaining);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        cursor += written;
        remaining -= (size_t)written;
    }
    bool complete = remaining == 0 && fsync(fd) == 0;
    close(fd);
    if (!complete || rename(temporary, destination))
        (void)unlink(temporary);
}

bool oasp_shared_cache_disk_load(oasp_shared_cache *cache, const char *path, void **data, size_t *size) {
    if (data)
        *data = NULL;
    if (size)
        *size = 0;
    if (!cache || !path || !data || !size)
        return false;
    char source[4096];
    if (!oasp_cache_file_path(cache, path, source))
        return false;
    int fd = open(source, O_RDONLY);
    if (fd < 0)
        return false;
    struct stat metadata;
    if (fstat(fd, &metadata) || metadata.st_size <= 0 || (uint64_t)metadata.st_size > SIZE_MAX) {
        close(fd);
        return false;
    }
    size_t length = (size_t)metadata.st_size;
    void *buffer = malloc(length);
    if (!buffer) {
        close(fd);
        return false;
    }
    unsigned char *cursor = buffer;
    size_t remaining = length;
    while (remaining) {
        ssize_t count = read(fd, cursor, remaining);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        cursor += count;
        remaining -= (size_t)count;
    }
    close(fd);
    if (remaining || !oasp_module_image_valid(buffer, length, path)) {
        free(buffer);
        (void)unlink(source);
        return false;
    }
    *data = buffer;
    *size = length;
    return true;
}

static bool oasp_cache_wait_for_readers(oasp_shared_header *header) {
    for (size_t attempts = 0; attempts < 5000; attempts++) {
        if (atomic_load_explicit(&header->readers, memory_order_acquire) == 0)
            return true;
        struct timespec delay = {0, 1000000};
        (void)nanosleep(&delay, NULL);
    }
    return false;
}

bool oasp_shared_cache_publish(oasp_shared_cache *cache, const char *path, const void *data, size_t size, uint64_t expected_version,
                               uint64_t *published_version) {
    if (published_version)
        *published_version = 0;
    if (!cache || !path || !data || !size || !oasp_module_image_valid(data, size, path) || !oasp_cache_lock(cache, F_WRLCK))
        return false;
    oasp_shared_header *header = oasp_cache_header(cache);
    uint64_t hash = oasp_cache_hash(path);
    uint32_t previous = oasp_cache_find_locked(cache, path, hash);
    if (previous != OASP_CACHE_NONE && header->entries[previous].version != expected_version &&
        oasp_module_image_valid(cache->mapping + header->entries[previous].image_offset, header->entries[previous].image_length, path)) {
        if (published_version)
            *published_version = header->entries[previous].version;
        oasp_cache_unlock(cache);
        return true;
    }

    size_t path_length = strlen(path);
    size_t path_offset = oasp_cache_align(header->arena_used);
    size_t image_offset = oasp_cache_align(path_offset + path_length);
    bool needs_restart = header->entry_count >= OASP_CACHE_ENTRIES || path_offset > cache->capacity || path_length > cache->capacity - path_offset ||
                         image_offset > cache->capacity || size > cache->capacity - image_offset;
    if (needs_restart) {
        uint64_t generation = atomic_load_explicit(&header->generation, memory_order_relaxed) + 1;
        if (!oasp_cache_wait_for_readers(header)) {
            oasp_cache_unlock(cache);
            return false;
        }
        oasp_cache_initialize(header, cache->capacity, generation);
        previous = OASP_CACHE_NONE;
        path_offset = oasp_cache_align(header->arena_used);
        image_offset = oasp_cache_align(path_offset + path_length);
        if (image_offset > cache->capacity || size > cache->capacity - image_offset) {
            oasp_cache_unlock(cache);
            return false;
        }
    }

    uint32_t index = header->entry_count++;
    oasp_shared_entry *entry = &header->entries[index];
    memset(entry, 0, sizeof(*entry));
    memcpy(cache->mapping + path_offset, path, path_length);
    memcpy(cache->mapping + image_offset, data, size);
    entry->next = header->buckets[hash % OASP_CACHE_BUCKETS];
    entry->path_offset = (uint32_t)path_offset;
    entry->path_length = (uint32_t)path_length;
    entry->image_offset = (uint32_t)image_offset;
    entry->image_length = (uint32_t)size;
    entry->hash = hash;
    entry->version = atomic_fetch_add_explicit(&header->next_version, 1, memory_order_relaxed);
    entry->generation = atomic_load_explicit(&header->generation, memory_order_relaxed);
    struct stat metadata;
    if (!stat(path, &metadata)) {
        entry->modified_seconds = (int64_t)metadata.st_mtime;
#if defined(__APPLE__)
        entry->modified_nanoseconds = (int64_t)metadata.st_mtimespec.tv_nsec;
#else
        entry->modified_nanoseconds = (int64_t)metadata.st_mtim.tv_nsec;
#endif
        entry->file_size = (int64_t)metadata.st_size;
    }
    atomic_init(&entry->references, 0);
    header->arena_used = (uint32_t)(image_offset + size);
    header->buckets[hash % OASP_CACHE_BUCKETS] = index;
    atomic_store_explicit(&entry->status, OASP_CACHE_READY, memory_order_release);
    if (previous != OASP_CACHE_NONE)
        atomic_store_explicit(&header->entries[previous].status, OASP_CACHE_RETIRED, memory_order_release);
    if (published_version)
        *published_version = entry->version;
    oasp_cache_unlock(cache);
    oasp_cache_disk_store(cache, path, data, size);
    return true;
}

uint64_t oasp_shared_cache_generation(const oasp_shared_cache *cache) {
    if (!cache)
        return 0;
    return atomic_load_explicit(&((const oasp_shared_header *)cache->mapping)->generation, memory_order_acquire);
}
