// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`tests/test_allocator.c`): Native regression test. Local fixtures construct
 * the smallest state needed to exercise ownership, limits, persistence, and failure paths;
 * assertions document the externally observable contract and guard against silent regressions.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "allocator.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct gc_node {
    struct gc_node *next;
    const char *text;
} gc_node;

typedef struct {
    gc_node *node;
} gc_roots;

static void trace_node(oasp_allocator *allocator, void *object) {
    gc_node *node = object;
    oasp_allocator_trace(allocator, node->next);
    oasp_allocator_trace(allocator, node->text);
}

static void trace_roots(oasp_allocator *allocator, void *context) {
    gc_roots *roots = context;
    oasp_allocator_trace(allocator, roots->node);
}

static void count_finalizer(void *context) {
    size_t *count = context;
    (*count)++;
}

int main(void) {
    oasp_memory_manager arena_memory;
    oasp_memory_manager_init(&arena_memory, 8192);
    oasp_arena *limited_arena = oasp_arena_create(4096);
    assert(limited_arena);
    assert(oasp_arena_bind_memory(limited_arena, &arena_memory));
    assert(arena_memory.current == oasp_arena_reserved(limited_arena));
    assert(!oasp_arena_alloc(limited_arena, 5000, _Alignof(max_align_t)));
    assert(arena_memory.allocation_failures == 1);
    assert(strstr(oasp_memory_error(&arena_memory), "allowed memory size of 8192 bytes exhausted"));
    oasp_arena_unbind_memory(limited_arena);
    assert(arena_memory.current == 0);
    oasp_arena_destroy(limited_arena);

    oasp_gc_config config = {0};
    config.heap_limit = 1024 * 1024;
    config.young_limit = 256;
    config.major_threshold = 512;
    config.major_step = 1;
    config.large_object_threshold = 4096;
    config.promotion_age = 2;
    oasp_allocator *allocator = oasp_allocator_create_gc(&config);
    assert(allocator);

    gc_roots roots = {0};
    oasp_allocator_set_roots(allocator, trace_roots, &roots);
    roots.node = oasp_allocator_alloc(allocator, sizeof(*roots.node), _Alignof(gc_node), trace_node);
    assert(roots.node);
    roots.node->text = oasp_allocator_copy(allocator, "root", 4);
    roots.node->next = oasp_allocator_alloc(allocator, sizeof(*roots.node), _Alignof(gc_node), trace_node);
    assert(roots.node->text && roots.node->next);
    roots.node->next->text = oasp_allocator_copy(allocator, "young", 5);
    oasp_allocator_write_barrier(allocator, roots.node, roots.node->next);
    oasp_allocator_collect(allocator, false);
    oasp_allocator_collect(allocator, false);

    oasp_gc_stats stats;
    oasp_allocator_get_stats(allocator, &stats);
    assert(stats.object_count == 4);
    assert(stats.promoted_objects == 4);

    size_t finalized = 0;
    assert(oasp_allocator_set_finalizer(allocator, roots.node->next, count_finalizer, &finalized));
    roots.node->next->next = roots.node->next;
    gc_node *old_child = roots.node->next;
    roots.node->next = NULL;
    oasp_allocator_write_barrier(allocator, roots.node, NULL);
    oasp_allocator_collect(allocator, true);
    assert(finalized == 1);
    oasp_allocator_get_stats(allocator, &stats);
    assert(stats.object_count == 2);

    assert(oasp_allocator_account_external(allocator, roots.node, 4096));
    oasp_allocator_get_stats(allocator, &stats);
    assert(stats.external_bytes == 4096);
    assert(oasp_allocator_account_external(allocator, roots.node, -4096));
    (void)old_child;

    roots.node = NULL;
    oasp_allocator_collect(allocator, true);
    oasp_allocator_get_stats(allocator, &stats);
    assert(stats.object_count == 0);
    oasp_allocator_destroy(allocator);

    oasp_memory_manager gc_memory;
    oasp_memory_manager_init(&gc_memory, 4096);
    oasp_gc_config managed = {.heap_limit = 1024 * 1024, .memory = &gc_memory};
    allocator = oasp_allocator_create_gc(&managed);
    assert(allocator);
    assert(oasp_allocator_alloc(allocator, 512, _Alignof(max_align_t), NULL));
    assert(gc_memory.current > 512);
    assert(!oasp_allocator_alloc(allocator, 4096, _Alignof(max_align_t), NULL));
    assert(gc_memory.allocation_failures == 1);
    oasp_allocator_destroy(allocator);
    assert(gc_memory.current == 0);

    oasp_gc_config incremental = {0};
    incremental.heap_limit = 1024 * 1024;
    incremental.young_limit = 1024 * 1024;
    incremental.major_threshold = 1;
    incremental.major_step = 1;
    incremental.large_object_threshold = 1;
    allocator = oasp_allocator_create_gc(&incremental);
    assert(allocator);
    roots.node = oasp_allocator_alloc(allocator, sizeof(*roots.node), _Alignof(gc_node), trace_node);
    assert(roots.node);
    roots.node->next =
        oasp_allocator_alloc(allocator, sizeof(*roots.node), _Alignof(gc_node), trace_node);
    roots.node->text = oasp_allocator_copy(allocator, "old-root", 8);
    gc_node *replacement =
        oasp_allocator_alloc(allocator, sizeof(*replacement), _Alignof(gc_node), trace_node);
    assert(roots.node->next && roots.node->text && replacement);
    replacement->text = oasp_allocator_copy(allocator, "replacement", 11);
    assert(replacement->text);
    oasp_allocator_set_roots(allocator, trace_roots, &roots);
    oasp_allocator_safepoint(allocator);
    roots.node = replacement;
    do {
        oasp_allocator_safepoint(allocator);
        oasp_allocator_get_stats(allocator, &stats);
    } while (!stats.major_collections);
    assert(roots.node == replacement);
    assert(roots.node->text && !strcmp(roots.node->text, "replacement"));
    assert(stats.object_count == 2);
    oasp_allocator_destroy(allocator);

    oasp_gc_config tiny = {0};
    tiny.heap_limit = 256;
    allocator = oasp_allocator_create_gc(&tiny);
    assert(allocator);
    assert(!oasp_allocator_alloc(allocator, 1024, _Alignof(max_align_t), NULL));
    assert(oasp_allocator_failed(allocator));
    oasp_allocator_destroy(allocator);

    puts("allocator tests passed");
    return 0;
}
