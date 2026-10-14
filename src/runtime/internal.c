// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/runtime/internal.c`): Runtime core. This unit manages ASP values,
 * frames, bytecode execution, and allocator roots. State variables encode interpreter control
 * flow and errors; all allocation and reference updates must preserve arena or generational-GC
 * ownership rules.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

oasp_process_config oasp_process_options;

void oasp_process_configure(void) {
    oasp_process_options.trace_variable = getenv("OPENASP_TRACE_VARIABLE");
    oasp_process_options.trace_errors = getenv("OPENASP_TRACE_ERRORS") != NULL;
    oasp_process_options.trace_array = getenv("OPENASP_TRACE_ARRAY") != NULL;
    oasp_process_options.trace_vm = getenv("OPENASP_TRACE_VM") != NULL;
    oasp_process_options.trace_dictionary = getenv("OPENASP_TRACE_DICTIONARY") != NULL;
}

bool oasp_buffer_init(oasp_buffer *buffer, size_t capacity) {
    if (!buffer)
        return false;
    if (capacity < 64)
        capacity = 64;
    buffer->data = malloc(capacity);
    if (!buffer->data)
        return false;
    buffer->length = 0;
    buffer->capacity = capacity;
    buffer->data[0] = '\0';
    return true;
}

bool oasp_buffer_append(oasp_buffer *buffer, const void *data, size_t length) {
    if (!buffer || (!data && length))
        return false;
    if (length > SIZE_MAX - buffer->length - 1)
        return false;
    size_t required = buffer->length + length + 1;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        char *grown = realloc(buffer->data, capacity);
        if (!grown)
            return false;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    if (length)
        memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

bool oasp_buffer_append_byte(oasp_buffer *buffer, char value) { return oasp_buffer_append(buffer, &value, 1); }

void oasp_buffer_dispose(oasp_buffer *buffer) {
    if (!buffer)
        return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

char *oasp_arena_copy(oasp_arena *arena, const char *data, size_t length) {
    if (!arena || (!data && length))
        return NULL;
    char *copy = oasp_arena_alloc(arena, length + 1, _Alignof(char));
    if (!copy)
        return NULL;
    if (length)
        memcpy(copy, data, length);
    copy[length] = '\0';
    return copy;
}

char *oasp_arena_lower(oasp_arena *arena, const char *data, size_t length) {
    char *copy = oasp_arena_copy(arena, data, length);
    if (!copy)
        return NULL;
    for (size_t i = 0; i < length; i++)
        copy[i] = (char)tolower((unsigned char)copy[i]);
    return copy;
}

bool oasp_ascii_equal_n(const char *left, size_t left_length, const char *right) {
    size_t right_length = strlen(right);
    if (left_length != right_length)
        return false;
    for (size_t i = 0; i < left_length; i++) {
        if (tolower((unsigned char)left[i]) != tolower((unsigned char)right[i]))
            return false;
    }
    return true;
}

bool oasp_ascii_equal(oasp_slice left, const char *right) { return oasp_ascii_equal_n(left.data, left.length, right); }
