// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Low-level shared utilities used by compiler and runtime modules. Slices are
 * non-owning byte ranges; buffers own reallocatable heap memory; process
 * options are initialized once from the environment. Helpers use explicit
 * lengths so source text and ASP strings need not be NUL-terminated.
 */

#ifndef OPENASP_INTERNAL_H
#define OPENASP_INTERNAL_H

#include "openasp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OASP_ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))

typedef struct {
    /* Borrowed bytes; `data` may be NULL only when `length` is zero. */
    const char *data;
    size_t length;
} oasp_slice;

typedef struct {
    /* Heap-owned growable bytes; capacity is always at least length. */
    char *data;
    size_t length;
    size_t capacity;
} oasp_buffer;

typedef struct {
    /* Optional environment-selected trace filters used only for diagnostics. */
    const char *trace_variable;
    bool trace_errors;
    bool trace_array;
    bool trace_vm;
    bool trace_dictionary;
} oasp_process_config;

extern oasp_process_config oasp_process_options;

/* Reads trace environment variables into process-global borrowed pointers. */
void oasp_process_configure(void);

/*
 * Buffer helpers initialize ownership, append overflow-checked bytes, and free
 * storage. A failed append leaves existing bytes valid for eventual disposal.
 */
bool oasp_buffer_init(oasp_buffer *buffer, size_t capacity);
bool oasp_buffer_append(oasp_buffer *buffer, const void *data, size_t length);
bool oasp_buffer_append_byte(oasp_buffer *buffer, char value);
void oasp_buffer_dispose(oasp_buffer *buffer);

/* Copies or lowercases an explicit byte range into NUL-terminated arena storage. */
char *oasp_arena_copy(oasp_arena *arena, const char *data, size_t length);
char *oasp_arena_lower(oasp_arena *arena, const char *data, size_t length);

/* ASCII-only case-insensitive comparisons avoid locale-dependent identifiers. */
bool oasp_ascii_equal(oasp_slice left, const char *right);
bool oasp_ascii_equal_n(const char *left, size_t left_length, const char *right);

#endif
