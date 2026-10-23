// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`include/openasp.h`): Public embedding API. Opaque handles keep allocator,
 * request, engine, and shared-cache ownership behind a stable C ABI. Pointer parameters are
 * borrowed unless a function explicitly creates an object; returned text remains owned by its
 * request.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_H
#define OPENASP_H

#include <stddef.h>

typedef struct oasp_arena oasp_arena;
typedef struct oasp_request oasp_request;
typedef struct oasp_engine oasp_engine;
typedef struct oasp_shared_cache oasp_shared_cache;
typedef struct oasp_debugger oasp_debugger;

/*
 * Finalizers release native resources owned by arena or GC objects. Response
 * sinks receive serialized body fragments; zero accepts a chunk and nonzero
 * reports a write failure.
 * Both callbacks borrow their context and must not retain arena-backed input.
 */
typedef void (*oasp_finalizer_fn)(void *context);
typedef int (*oasp_response_sink_fn)(void *context, const void *data, size_t length);

/* Hard request limits; zero means unlimited only where the implementation says so. */
typedef struct {
    /* Request-owned arena, managed-heap, and explicit external bytes. Zero is unlimited. */
    size_t memory_limit;
    /* Maximum response bytes retained when Response.Buffer is enabled. */
    size_t max_buffered_response_bytes;
    /* Aggregate serialized application-header bytes. */
    size_t max_response_header_bytes;
    /* Maximum number of application-provided response headers. */
    size_t max_response_header_count;
    /* Monotonic execution deadline relative to request creation. */
    unsigned long request_timeout_ms;
} oasp_request_limits;

/*
 * Opaque arena rollback marker. Callers may save and restore it only on the
 * same live arena; restoring releases allocations and runs newer finalizers.
 */
typedef struct {
    void *block;
    void *finalizer;
    size_t block_used;
    size_t used;
} oasp_arena_checkpoint;

/*
 * Arena lifecycle and allocation API. `block_size` selects the reusable first
 * block, `alignment` must be a nonzero power of two, and returned storage lives
 * until reset/restore/destroy. Mapped arenas return unused pages to the OS.
 */
oasp_arena *oasp_arena_create(size_t block_size);
oasp_arena *oasp_arena_create_mapped(size_t block_size);
void oasp_arena_reset(oasp_arena *arena);
void oasp_arena_destroy(oasp_arena *arena);
void *oasp_arena_alloc(oasp_arena *arena, size_t size, size_t alignment);
int oasp_arena_defer(oasp_arena *arena, oasp_finalizer_fn function, void *context);
oasp_arena_checkpoint oasp_arena_checkpoint_save(const oasp_arena *arena);
void oasp_arena_checkpoint_restore(oasp_arena *arena, oasp_arena_checkpoint checkpoint);
size_t oasp_arena_used(const oasp_arena *arena);
size_t oasp_arena_peak_used(const oasp_arena *arena);
size_t oasp_arena_reserved(const oasp_arena *arena);

/*
 * Request construction copies input strings into `arena`. The extended form
 * borrows all three arenas and restores each to its construction checkpoint
 * when the request is destroyed. Their destruction remains the caller's
 * responsibility, which lets persistent FastCGI workers recycle fixed blocks.
 */
oasp_request *oasp_request_create(const char *root, const char *query, const char *body, const char *method);
oasp_request *oasp_request_create_with_arenas(const char *root, const char *query, const char *body, const char *method, oasp_arena *arena,
                                              oasp_arena *response_arena, oasp_arena *scratch_arena);
void oasp_request_destroy(oasp_request *request);
oasp_arena *oasp_request_arena(oasp_request *request);
oasp_arena *oasp_request_scratch_arena(oasp_request *request);

/*
 * Server variables and decoded request collections return borrowed strings
 * valid for the request lifetime. Missing collection keys return an empty
 * value; the *_has and *_count functions preserve ASP collection semantics.
 */
int oasp_request_set_server_variable(oasp_request *request, const char *name, const char *value);
const char *oasp_request_server_variable(const oasp_request *request, const char *name);
const char *oasp_request_root(const oasp_request *request);
const char *oasp_request_path(const oasp_request *request);
void oasp_request_set_path(oasp_request *request, const char *path);
const char *oasp_request_query(const oasp_request *request, const char *name);
const char *oasp_request_form(const oasp_request *request, const char *name);
int oasp_request_query_has(const oasp_request *request, const char *name);
int oasp_request_form_has(const oasp_request *request, const char *name);
size_t oasp_request_query_count(const oasp_request *request);
size_t oasp_request_form_count(const oasp_request *request);
const char *oasp_request_cookie_value(const oasp_request *request, const char *name);
const char *oasp_request_cookie_subvalue(const oasp_request *request, const char *name, const char *subkey);
const char *oasp_request_query_string(const oasp_request *request);
const char *oasp_request_body(const oasp_request *request);
size_t oasp_request_body_length(const oasp_request *request);
int oasp_request_set_body(oasp_request *request, const void *body, size_t length);
const char *oasp_request_method(const oasp_request *request);

/*
 * Limits are copied into the request. Failure and deadline accessors allow
 * transport code to distinguish an interpreter error from successful output.
 */
int oasp_request_set_limits(oasp_request *request, const oasp_request_limits *limits);
int oasp_request_deadline_expired(const oasp_request *request);
long long oasp_request_deadline_milliseconds(const oasp_request *request);
int oasp_request_failed(const oasp_request *request);
const char *oasp_request_error(const oasp_request *request);
size_t oasp_request_memory_usage(const oasp_request *request);
size_t oasp_request_peak_memory_usage(const oasp_request *request);

/*
 * Output functions append bytes or stream them through `sink`. Sink and
 * context are borrowed. Once committed, headers cannot be changed; flush
 * reports failure when the sink rejects data or a configured limit is hit.
 */
void oasp_request_write(oasp_request *request, const char *text, size_t length);
void oasp_request_binary_write(oasp_request *request, const char *data, size_t length);
void oasp_request_set_output_sink(oasp_request *request, oasp_response_sink_fn sink, void *context);
int oasp_request_set_buffered(oasp_request *request, int buffered);
int oasp_request_buffered(const oasp_request *request);
int oasp_request_flush(oasp_request *request);
int oasp_request_output_committed(const oasp_request *request);
const char *oasp_request_output(const oasp_request *request, size_t *length);
void oasp_request_truncate_output(oasp_request *request, size_t length);

/*
 * Response metadata is request-owned after each setter succeeds. Header and
 * cookie setters validate limits and return zero on success and nonzero on
 * allocation or policy failure; indexed getters return borrowed values.
 */
int oasp_request_set_status(oasp_request *request, const char *status);
const char *oasp_request_status(const oasp_request *request);
int oasp_request_set_content_type(oasp_request *request, const char *content_type);
const char *oasp_request_content_type(const oasp_request *request);
int oasp_request_set_charset(oasp_request *request, const char *charset);
const char *oasp_request_charset(const oasp_request *request);
int oasp_request_set_codepage(oasp_request *request, int codepage);
int oasp_request_add_header(oasp_request *request, const char *name, const char *value);
size_t oasp_request_header_count(const oasp_request *request);
const char *oasp_request_header_name(const oasp_request *request, size_t index);
const char *oasp_request_header_value(const oasp_request *request, size_t index);
int oasp_request_set_cookie(oasp_request *request, const char *name, const char *value);
int oasp_request_set_cookie_subvalue(oasp_request *request, const char *name, const char *subkey, const char *value);
int oasp_request_set_cookie_attribute(oasp_request *request, const char *name, const char *attribute, const char *value);

/*
 * Convenience renderers create a temporary engine. Long-running callers should
 * own an engine, optionally attach a shared immutable module cache, and call
 * release_memory between idle periods. Paths are resolved below request root.
 */
int oasp_render_file(oasp_request *request, const char *path);
int oasp_render_text(oasp_request *request, const char *source, size_t length, const char *source_name);
oasp_engine *oasp_engine_create(void);
oasp_engine *oasp_engine_create_with_cache(oasp_shared_cache *cache);
int oasp_engine_enable_shared_state(oasp_engine *engine, const char *directory, const char *root);
void oasp_engine_release_memory(oasp_engine *engine);
void oasp_engine_destroy(oasp_engine *engine);
int oasp_engine_render_file(oasp_engine *engine, oasp_request *request, const char *path);
int oasp_engine_render_file_debug(oasp_engine *engine, oasp_request *request, const char *path, const oasp_debugger *debugger);

#endif
