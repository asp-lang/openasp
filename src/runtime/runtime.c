// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

/*
 * Module overview (`src/runtime/runtime.c`): Runtime core. This unit manages ASP values,
 * frames, bytecode execution, and allocator roots. State variables encode interpreter control
 * flow and errors; all allocation and reference updates must preserve arena or generational-GC
 * ownership rules.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "runtime.h"
#include "internal.h"
#include "memory.h"
#include "ast_image.h"
#include "bytecode.h"
#include "component.h"
#include "debugger.h"
#include "vm.h"
#include "symbol.h"

#include <ctype.h>
#include <errno.h>
#include <iconv.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * Runtime foundation: allocator integration, GC roots, variables, errors,
 * arrays, classes, dates, and shared helpers used by the execution layers.
 */
static bool oasp_runtime_execute_range(oasp_runtime *runtime, const oasp_stmt *first, const oasp_stmt *last);
bool oasp_runtime_execute_dynamic(oasp_runtime *runtime, const oasp_block *program);
static void oasp_runtime_error(oasp_runtime *runtime, int number, const char *description, size_t line);
static bool oasp_runtime_declare(oasp_runtime *runtime, oasp_slice name, const oasp_value *value, bool constant);

bool oasp_text_is_utf8(const char *data, size_t length) {
    if (!data && length)
        return false;
    const unsigned char *bytes = (const unsigned char *)data;
    size_t i = 0;
    while (i < length) {
        unsigned char first = bytes[i++];
        if (first <= 0x7f)
            continue;
        size_t continuation;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation = 2;
            if (i >= length || (first == 0xe0 && bytes[i] < 0xa0) || (first == 0xed && bytes[i] >= 0xa0))
                return false;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation = 3;
            if (i >= length || (first == 0xf0 && bytes[i] < 0x90) || (first == 0xf4 && bytes[i] >= 0x90))
                return false;
        } else {
            return false;
        }
        if (continuation > length - i)
            return false;
        for (size_t j = 0; j < continuation; j++)
            if ((bytes[i++] & 0xc0) != 0x80)
                return false;
    }
    return true;
}

bool oasp_text_convert(oasp_buffer *output, const char *data, size_t length, const char *from_charset, const char *to_charset) {
    if (!output || (!data && length) || !from_charset || !*from_charset || !to_charset || !*to_charset) {
        errno = EINVAL;
        return false;
    }
    memset(output, 0, sizeof(*output));
    iconv_t converter = iconv_open(to_charset, from_charset);
    if (converter == (iconv_t)-1)
        return false;
    if (!oasp_buffer_init(output, length < SIZE_MAX - 16 ? length + 16 : length)) {
        iconv_close(converter);
        errno = ENOMEM;
        return false;
    }
    char *input = (char *)data;
    size_t input_left = length;
    bool converted = true;
    while (input_left) {
        char chunk[4096];
        char *cursor = chunk;
        size_t available = sizeof(chunk);
        size_t status = iconv(converter, &input, &input_left, &cursor, &available);
        if (!oasp_buffer_append(output, chunk, sizeof(chunk) - available)) {
            errno = ENOMEM;
            converted = false;
            break;
        }
        if (status == (size_t)-1 && errno != E2BIG) {
            converted = false;
            break;
        }
    }
    while (converted) {
        char chunk[32];
        char *cursor = chunk;
        size_t available = sizeof(chunk);
        size_t status = iconv(converter, NULL, NULL, &cursor, &available);
        if (!oasp_buffer_append(output, chunk, sizeof(chunk) - available)) {
            errno = ENOMEM;
            converted = false;
            break;
        }
        if (status != (size_t)-1)
            break;
        if (errno != E2BIG) {
            converted = false;
            break;
        }
    }
    int saved_error = errno;
    iconv_close(converter);
    if (!converted) {
        oasp_buffer_dispose(output);
        errno = saved_error;
    }
    return converted;
}

struct oasp_fork_child {
    struct oasp_fork_child *next;
    pid_t pid;
};

struct oasp_string_builder {
    struct oasp_string_builder *next;
    oasp_binding *target;
    char *data;
    size_t length;
    size_t capacity;
    bool shared;
};

static void oasp_runtime_trace_value(oasp_allocator *allocator, const oasp_value *value) {
    if (!value)
        return;
    if (value->kind == OASP_VALUE_STRING)
        oasp_allocator_trace(allocator, value->as.string.data);
    else if (value->kind == OASP_VALUE_DATE)
        oasp_allocator_trace(allocator, value->as.date.data);
    else if (value->kind == OASP_VALUE_ARRAY)
        oasp_allocator_trace(allocator, value->as.array);
    else if (value->kind == OASP_VALUE_OBJECT)
        oasp_allocator_trace(allocator, value->as.object);
}

static void oasp_runtime_trace_values(oasp_allocator *allocator, void *object) {
    size_t count = oasp_allocator_object_size(allocator, object) / sizeof(oasp_value);
    oasp_value *values = object;
    for (size_t i = 0; i < count; i++)
        oasp_runtime_trace_value(allocator, &values[i]);
}

static void oasp_runtime_trace_array(oasp_allocator *allocator, void *object) {
    oasp_array *array = object;
    oasp_allocator_trace(allocator, array->bounds);
    oasp_allocator_trace(allocator, array->items);
}

static void oasp_runtime_trace_binding(oasp_allocator *allocator, void *object) {
    oasp_binding *binding = object;
    oasp_allocator_trace(allocator, binding->next);
    oasp_allocator_trace(allocator, binding->name.data);
    oasp_allocator_trace(allocator, binding->reference);
    oasp_runtime_trace_value(allocator, &binding->value);
}

static void oasp_runtime_trace_object(oasp_allocator *allocator, void *object) {
    oasp_object *value = object;
    oasp_allocator_trace(allocator, value->class_def);
    oasp_allocator_trace(allocator, value->fields);
    oasp_allocator_trace(allocator, value->native);
    oasp_component_gc_trace(allocator, value);
}

static void oasp_runtime_trace_class_def(oasp_allocator *allocator, void *object) {
    oasp_class_def *definition = object;
    oasp_allocator_trace(allocator, definition->next);
    oasp_allocator_trace(allocator, definition->name.data);
    oasp_allocator_trace(allocator, definition->bytecode);
}

static void oasp_runtime_trace_roots(oasp_allocator *allocator, void *context) {
    oasp_runtime *runtime = context;
    oasp_allocator_trace(allocator, runtime->variables);
    oasp_allocator_trace(allocator, runtime->session);
    oasp_allocator_trace(allocator, runtime->application);
    oasp_allocator_trace(allocator, runtime->with_object);
    oasp_allocator_trace(allocator, runtime->global_slots);
    oasp_allocator_trace(allocator, runtime->routines);
    oasp_allocator_trace(allocator, runtime->classes);
    oasp_allocator_trace(allocator, runtime->bytecode);
    oasp_allocator_trace(allocator, runtime->global_slot_bytecode);
    oasp_allocator_trace(allocator, runtime->error_description);
    oasp_allocator_trace(allocator, runtime->error_source);
    for (size_t i = 0; i < runtime->global_slot_count; i++)
        oasp_allocator_trace(allocator, runtime->global_slots[i]);
    if (runtime->routines)
        for (size_t i = 0; i < runtime->routines->count; i++)
            oasp_allocator_trace(allocator, runtime->routines->items[i].bytecode);
    for (oasp_class_def *definition = runtime->classes; definition; definition = definition->next) {
        oasp_allocator_trace(allocator, definition);
        oasp_allocator_trace(allocator, definition->name.data);
        oasp_allocator_trace(allocator, definition->bytecode);
    }
    for (oasp_frame *frame = runtime->frame; frame; frame = frame->parent) {
        oasp_allocator_trace(allocator, frame->locals);
        oasp_allocator_trace(allocator, frame->object);
        for (size_t i = 0; i < frame->slot_count; i++) {
            oasp_allocator_trace(allocator, frame->slots[i].reference);
            oasp_runtime_trace_value(allocator, &frame->slots[i].value);
        }
        for (oasp_string_builder *builder = frame->string_builders; builder; builder = builder->next) {
            oasp_allocator_trace(allocator, builder->target);
            oasp_allocator_trace(allocator, builder->data);
        }
    }
    for (oasp_string_builder *builder = runtime->string_builders; builder; builder = builder->next) {
        oasp_allocator_trace(allocator, builder->target);
        oasp_allocator_trace(allocator, builder->data);
    }
    for (size_t i = 0; i < OASP_ARRAY_COUNT(runtime->recordset_ordinals); i++)
        if (runtime->recordset_ordinals[i].valid)
            oasp_allocator_trace(allocator, runtime->recordset_ordinals[i].recordset);
    for (oasp_gc_root *root = runtime->gc_roots; root; root = root->previous) {
        for (size_t i = 0; i < root->value_count; i++)
            oasp_runtime_trace_value(allocator, &root->values[i]);
        for (size_t i = 0; i < root->object_count; i++)
            oasp_allocator_trace(allocator, root->objects[i]);
        for (size_t i = 0; i < root->array_count; i++)
            oasp_allocator_trace(allocator, root->arrays[i]);
    }
}

void *oasp_runtime_alloc(oasp_runtime *runtime, size_t size, size_t alignment) {
    return runtime && runtime->allocator
               ? oasp_allocator_alloc(runtime->allocator, size, alignment, NULL)
               : runtime ? oasp_arena_alloc(runtime->arena, size, alignment) : NULL;
}

void *oasp_runtime_alloc_values(oasp_runtime *runtime, size_t count) {
    if (!runtime || (count && count > SIZE_MAX / sizeof(oasp_value)))
        return NULL;
    return runtime->allocator
               ? oasp_allocator_alloc(runtime->allocator, count * sizeof(oasp_value),
                                      _Alignof(oasp_value), oasp_runtime_trace_values)
               : oasp_runtime_alloc(runtime, count * sizeof(oasp_value), _Alignof(oasp_value));
}

oasp_binding *oasp_runtime_alloc_binding(oasp_runtime *runtime) {
    return runtime && runtime->allocator
               ? oasp_allocator_alloc(runtime->allocator, sizeof(oasp_binding),
                                      _Alignof(oasp_binding), oasp_runtime_trace_binding)
               : runtime ? oasp_arena_alloc(runtime->arena, sizeof(oasp_binding),
                                            _Alignof(oasp_binding)) : NULL;
}

oasp_array *oasp_runtime_alloc_array(oasp_runtime *runtime) {
    return runtime && runtime->allocator
               ? oasp_allocator_alloc(runtime->allocator, sizeof(oasp_array),
                                      _Alignof(oasp_array), oasp_runtime_trace_array)
               : runtime ? oasp_arena_alloc(runtime->arena, sizeof(oasp_array),
                                            _Alignof(oasp_array)) : NULL;
}

oasp_object *oasp_runtime_alloc_object(oasp_runtime *runtime) {
    return runtime && runtime->allocator
               ? oasp_allocator_alloc(runtime->allocator, sizeof(oasp_object),
                                      _Alignof(oasp_object), oasp_runtime_trace_object)
               : runtime ? oasp_arena_alloc(runtime->arena, sizeof(oasp_object),
                                            _Alignof(oasp_object)) : NULL;
}

oasp_class_def *oasp_runtime_alloc_class_def(oasp_runtime *runtime) {
    return runtime && runtime->allocator
               ? oasp_allocator_alloc(runtime->allocator, sizeof(oasp_class_def),
                                      _Alignof(oasp_class_def), oasp_runtime_trace_class_def)
               : runtime ? oasp_arena_alloc(runtime->arena, sizeof(oasp_class_def),
                                            _Alignof(oasp_class_def)) : NULL;
}

char *oasp_runtime_copy(oasp_runtime *runtime, const char *data, size_t length) {
    return runtime && runtime->allocator
               ? oasp_allocator_copy(runtime->allocator, data, length)
               : runtime ? oasp_arena_copy(runtime->arena, data, length) : NULL;
}

char *oasp_runtime_lower(oasp_runtime *runtime, const char *data, size_t length) {
    char *copy = oasp_runtime_copy(runtime, data, length);
    if (!copy)
        return NULL;
    for (size_t i = 0; i < length; i++)
        copy[i] = (char)tolower((unsigned char)copy[i]);
    return copy;
}

bool oasp_runtime_defer(oasp_runtime *runtime, void *owner, oasp_finalizer_fn function, void *context) {
    return runtime && runtime->allocator
               ? oasp_allocator_set_finalizer(runtime->allocator, owner, function, context)
               : runtime && oasp_arena_defer(runtime->arena, function, context);
}

bool oasp_runtime_account_external(oasp_runtime *runtime, void *owner, ptrdiff_t delta) {
    if (!runtime)
        return true;
    if (runtime->allocator)
        return oasp_allocator_account_external(runtime->allocator, owner, delta);
    if (delta <= 0) {
        size_t amount = delta < 0 ? (size_t)(-(delta + 1)) + 1 : 0;
        oasp_memory_release(runtime->memory, amount);
        return true;
    }
    return oasp_memory_reserve(runtime->memory, (size_t)delta, "external allocation");
}

void oasp_runtime_write_barrier(oasp_runtime *runtime, void *owner, const oasp_value *value) {
    if (!runtime || !runtime->allocator || !value)
        return;
    if (value->kind == OASP_VALUE_STRING)
        oasp_allocator_write_barrier(runtime->allocator, owner, value->as.string.data);
    else if (value->kind == OASP_VALUE_DATE)
        oasp_allocator_write_barrier(runtime->allocator, owner, value->as.date.data);
    else if (value->kind == OASP_VALUE_ARRAY)
        oasp_allocator_write_barrier(runtime->allocator, owner, value->as.array);
    else if (value->kind == OASP_VALUE_OBJECT)
        oasp_allocator_write_barrier(runtime->allocator, owner, value->as.object);
}

oasp_value oasp_runtime_value_copy(oasp_runtime *runtime, const oasp_value *value, bool copy_text) {
    if (!runtime || !value)
        return oasp_value_empty();
    oasp_value copy = *value;
    if (copy_text && (value->kind == OASP_VALUE_STRING || value->kind == OASP_VALUE_DATE)) {
        oasp_slice source = value->kind == OASP_VALUE_STRING ? value->as.string : value->as.date;
        char *text = oasp_runtime_copy(runtime, source.data, source.length);
        if (!text)
            return oasp_value_empty();
        if (value->kind == OASP_VALUE_STRING)
            copy.as.string.data = text;
        else
            copy.as.date.data = text;
    } else if (value->kind == OASP_VALUE_ARRAY && value->as.array) {
        const oasp_array *source = value->as.array;
        oasp_array *array = oasp_runtime_alloc_array(runtime);
        if (!array)
            return oasp_value_empty();
        memset(array, 0, sizeof(*array));
        array->rank = source->rank;
        array->length = source->length;
        if (source->rank) {
            array->bounds = oasp_runtime_alloc(runtime, source->rank * sizeof(*array->bounds),
                                               _Alignof(int64_t));
            if (!array->bounds)
                return oasp_value_empty();
            memcpy(array->bounds, source->bounds, source->rank * sizeof(*array->bounds));
            if (runtime->allocator)
                oasp_allocator_write_barrier(runtime->allocator, array, array->bounds);
        }
        if (source->length) {
            array->items = oasp_runtime_alloc_values(runtime, source->length);
            if (!array->items)
                return oasp_value_empty();
            for (size_t i = 0; i < source->length; i++)
                array->items[i] = oasp_runtime_value_copy(runtime, &source->items[i], copy_text);
            if (runtime->allocator)
                oasp_allocator_write_barrier(runtime->allocator, array, array->items);
        }
        copy.as.array = array;
    }
    return copy;
}

static void oasp_runtime_reap_fork_children(oasp_runtime *runtime, bool block) {
    if (!runtime)
        return;
    do {
        bool reaped = false;
        oasp_fork_child **link = &runtime->fork_children;
        while (*link) {
            oasp_fork_child *child = *link;
            int status = 0;
            pid_t waited;
            do {
                waited = waitpid(child->pid, &status, WNOHANG);
            } while (waited < 0 && errno == EINTR);
            if (waited == child->pid || (waited < 0 && errno == ECHILD)) {
                *link = child->next;
                free(child);
                reaped = true;
                continue;
            }
            link = &child->next;
        }
        if (!block || !runtime->fork_children)
            return;
        if (!reaped) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
            while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
            }
        }
    } while (runtime->fork_children);
}

bool oasp_runtime_register_fork_child(oasp_runtime *runtime, pid_t pid) {
    if (!runtime || pid <= 0)
        return false;
    oasp_fork_child *child = malloc(sizeof(*child));
    if (!child) {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return false;
    }
    child->pid = pid;
    child->next = runtime->fork_children;
    runtime->fork_children = child;
    return true;
}

void oasp_runtime_reset_fork_children(oasp_runtime *runtime) {
    if (!runtime)
        return;
    while (runtime->fork_children) {
        oasp_fork_child *child = runtime->fork_children;
        runtime->fork_children = child->next;
        free(child);
    }
}

void oasp_runtime_gc_root_enter(oasp_runtime *runtime, oasp_gc_root *root, oasp_value *values, size_t value_count, oasp_object **objects,
                                size_t object_count, oasp_array **arrays, size_t array_count) {
    if (!runtime || !root)
        return;
    root->previous = runtime->gc_roots;
    root->values = values;
    root->value_count = value_count;
    root->objects = objects;
    root->object_count = object_count;
    root->arrays = arrays;
    root->array_count = array_count;
    runtime->gc_roots = root;
}

void oasp_runtime_gc_root_leave(oasp_runtime *runtime, oasp_gc_root *root) {
    if (runtime && root && runtime->gc_roots == root)
        runtime->gc_roots = root->previous;
}

void oasp_runtime_gc_safepoint(oasp_runtime *runtime) {
    if (runtime && runtime->allocator)
        oasp_allocator_safepoint(runtime->allocator);
}

bool oasp_runtime_safepoint(oasp_runtime *runtime, size_t line) {
    if (!runtime)
        return false;
    oasp_runtime_gc_safepoint(runtime);
    if (runtime->fork_children && (runtime->safepoint_counter & 65535u) == 0)
        oasp_runtime_reap_fork_children(runtime, false);
    if (runtime->stopped)
        return true;
    if ((runtime->safepoint_counter++ & 1023u) != 0)
        return true;
    if (!oasp_request_deadline_expired(runtime->request))
        return true;
    oasp_runtime_error(runtime, 258, "request execution deadline exceeded", line);
    runtime->stopped = true;
    return false;
}

void oasp_runtime_gc_stats(const oasp_runtime *runtime, oasp_gc_stats *stats) {
    oasp_allocator_get_stats(runtime ? runtime->allocator : NULL, stats);
}

static void oasp_runtime_error(oasp_runtime *runtime, int number, const char *description, size_t line) {
    runtime->error_number = number;
    const char *memory_error = oasp_memory_error(runtime->memory);
    runtime->error_description = memory_error
                                     ? memory_error
                                     : description ? oasp_runtime_copy(runtime, description, strlen(description)) : NULL;
    if (description && !runtime->error_description)
        runtime->error_description = "runtime error";
    runtime->error_source = "OpenASP";
    runtime->error_line = line;
    runtime->error_generation++;
    runtime->error_pending = true;
    if (oasp_process_options.trace_errors)
        fprintf(stderr, "OpenASP trace: line %zu error %d: %s\n", line, number, description ? description : "runtime error");
    oasp_runtime_debug_error(runtime);
}

static void oasp_runtime_clear_error(oasp_runtime *runtime) {
    runtime->error_number = 0;
    runtime->error_description = NULL;
    runtime->error_source = NULL;
    runtime->error_line = 0;
    runtime->error_pending = false;
}

void oasp_runtime_raise(oasp_runtime *runtime, int number, const char *description, const char *source, size_t line) {
    oasp_runtime_error(runtime, number, description, line);
    runtime->error_source = source ? oasp_runtime_copy(runtime, source, strlen(source)) : NULL;
    if (source && !runtime->error_source)
        runtime->error_source = "OpenASP";
}

void oasp_runtime_init(oasp_runtime *runtime, oasp_request *request) {
    memset(runtime, 0, sizeof(*runtime));
    runtime->request = request;
    runtime->arena = oasp_request_arena(request);
    runtime->scratch = oasp_request_scratch_arena(request);
    runtime->format_arena = runtime->scratch;
    runtime->memory = oasp_request_memory_manager(request);
    runtime->session_timeout = 20;
}

bool oasp_runtime_init_service(oasp_runtime *runtime, oasp_request *request, const oasp_gc_config *config) {
    oasp_runtime_init(runtime, request);
    oasp_gc_config effective = config ? *config : (oasp_gc_config){0};
    effective.memory = oasp_request_memory_manager(request);
    runtime->allocator = oasp_allocator_create_gc(&effective);
    runtime->format_arena = oasp_arena_create(4096);
    bool format_bound = !effective.memory ||
                        (runtime->format_arena &&
                         oasp_arena_bind_memory(runtime->format_arena, effective.memory));
    if (!runtime->allocator || !runtime->format_arena || !format_bound) {
        oasp_arena_destroy(runtime->format_arena);
        oasp_allocator_destroy(runtime->allocator);
        runtime->format_arena = runtime->scratch;
        runtime->allocator = NULL;
        return false;
    }
    oasp_allocator_set_roots(runtime->allocator, oasp_runtime_trace_roots, runtime);
    return true;
}

void oasp_runtime_dispose(oasp_runtime *runtime) {
    if (!runtime)
        return;
    oasp_runtime_reap_fork_children(runtime, true);
    oasp_runtime_reset_fork_children(runtime);
    if (runtime->format_arena != runtime->scratch)
        oasp_arena_destroy(runtime->format_arena);
    runtime->format_arena = runtime->scratch;
    oasp_allocator_destroy(runtime->allocator);
    runtime->allocator = NULL;
    runtime->gc_roots = NULL;
}

oasp_value oasp_runtime_persist_copy(oasp_runtime *runtime, const oasp_value *value) {
    if (!runtime || !value)
        return oasp_value_empty();
    if (value->kind == OASP_VALUE_ARRAY) {
        const oasp_array *source = value->as.array;
        if (!source)
            return oasp_value_empty();
        oasp_array *array = oasp_runtime_alloc_array(runtime);
        if (!array)
            return oasp_value_empty();
        memset(array, 0, sizeof(*array));
        array->rank = source->rank;
        array->length = source->length;
        if (source->rank) {
            array->bounds = oasp_runtime_alloc(runtime, source->rank * sizeof(*array->bounds), _Alignof(int64_t));
            if (!array->bounds)
                return oasp_value_empty();
            memcpy(array->bounds, source->bounds, source->rank * sizeof(*array->bounds));
        }
        if (source->length) {
            array->items = oasp_runtime_alloc_values(runtime, source->length);
            if (!array->items)
                return oasp_value_empty();
            for (size_t i = 0; i < source->length; i++)
                array->items[i] = oasp_runtime_persist_copy(runtime, &source->items[i]);
        }
        oasp_value copy = *value;
        copy.as.array = array;
        return copy;
    }
    if (value->kind == OASP_VALUE_OBJECT) {
        const oasp_object *source = value->as.object;
        if (!source)
            return oasp_value_nothing();
        oasp_object *object = NULL;
        if (source->component_kind != OASP_COMPONENT_NONE) {
            if (!oasp_component_persist_copy(runtime, source, &object))
                return oasp_value_nothing();
        } else if (source->class_def) {
            object = oasp_runtime_alloc_object(runtime);
            oasp_class_def *class_def = oasp_runtime_alloc_class_def(runtime);
            if (!object || !class_def)
                return oasp_value_empty();
            memset(object, 0, sizeof(*object));
            memset(class_def, 0, sizeof(*class_def));
            class_def->name.length = source->class_def->name.length;
            class_def->name.data = oasp_runtime_copy(runtime, source->class_def->name.data, source->class_def->name.length);
            if (!class_def->name.data)
                return oasp_value_empty();
            object->class_def = class_def;
            object->fields = oasp_runtime_clone_store(runtime, source->fields);
        } else {
            return oasp_value_nothing();
        }
        oasp_value copy = *value;
        copy.as.object = object;
        return copy;
    }
    return oasp_runtime_value_copy(runtime, value, false);
}

oasp_binding *oasp_runtime_clone_store(oasp_runtime *runtime, const oasp_binding *store) {
    if (!runtime)
        return NULL;
    oasp_binding *first = NULL;
    oasp_binding **tail = &first;
    for (const oasp_binding *source = store; source; source = source->next) {
        oasp_binding *binding = oasp_runtime_alloc_binding(runtime);
        if (!binding)
            return NULL;
        memset(binding, 0, sizeof(*binding));
        binding->name.length = source->name.length;
        binding->name.data = oasp_runtime_copy(runtime, source->name.data, source->name.length);
        if (!binding->name.data)
            return NULL;
        binding->value = oasp_runtime_persist_copy(runtime, &source->value);
        binding->constant = source->constant;
        *tail = binding;
        tail = &binding->next;
    }
    return first;
}

void oasp_runtime_init_state(oasp_runtime *runtime, oasp_request *request, const oasp_binding *session, const oasp_binding *application, int session_timeout) {
    oasp_runtime_init(runtime, request);
    runtime->session = oasp_runtime_clone_store(runtime, session);
    runtime->application = oasp_runtime_clone_store(runtime, application);
    runtime->session_timeout = session_timeout > 0 ? session_timeout : 20;
}

static bool oasp_name_equal(oasp_slice left, oasp_slice right) {
    if (left.length != right.length)
        return false;
    for (size_t i = 0; i < left.length; i++) {
        unsigned char a = (unsigned char)left.data[i];
        unsigned char b = (unsigned char)right.data[i];
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b + ('a' - 'A'));
        if (a != b)
            return false;
    }
    return true;
}

static oasp_binding *oasp_binding_find(oasp_binding *bindings, oasp_slice name) {
    for (oasp_binding *binding = bindings; binding; binding = binding->next)
        if (oasp_name_equal(binding->name, name))
            return binding->reference ? binding->reference : binding;
    return NULL;
}

static oasp_binding *oasp_binding_find_promote(oasp_binding **bindings, oasp_slice name) {
    oasp_binding **link = bindings;
    while (*link) {
        oasp_binding *binding = *link;
        if (oasp_name_equal(binding->name, name)) {
            if (link != bindings) {
                *link = binding->next;
                binding->next = *bindings;
                *bindings = binding;
            }
            return binding->reference ? binding->reference : binding;
        }
        link = &binding->next;
    }
    return NULL;
}

static oasp_binding *oasp_binding_resolve(oasp_binding *binding) {
    return binding && binding->reference ? binding->reference : binding;
}

static oasp_binding *oasp_frame_find_slot(oasp_frame *frame, oasp_slice name) {
    for (size_t i = 0; frame && i < frame->slot_count; i++)
        if (oasp_name_equal(frame->slots[i].name, name))
            return oasp_binding_resolve(&frame->slots[i]);
    return NULL;
}

static oasp_binding *oasp_runtime_find_binding(oasp_runtime *runtime, oasp_slice name) {
    oasp_frame *frame = runtime->frame;
    if (!frame || (uintptr_t)frame < 4096)
        return oasp_binding_find_promote(&runtime->variables, name);
    oasp_binding *binding = oasp_binding_find(frame->locals, name);
    if (!binding)
        binding = oasp_frame_find_slot(frame, name);
    if (!binding && frame->object)
        binding = oasp_binding_find_promote(&frame->object->fields, name);
    if (binding)
        return binding;
    return oasp_binding_find_promote(&runtime->variables, name);
}

static void oasp_runtime_mark_builder_value(oasp_runtime *runtime, const oasp_value *value) {
    if (!runtime || !value || value->kind != OASP_VALUE_STRING)
        return;
    for (oasp_string_builder *builder = runtime->string_builders; builder; builder = builder->next)
        if (builder->data == value->as.string.data && builder->length == value->as.string.length)
            builder->shared = true;
    for (oasp_frame *frame = runtime->frame; frame; frame = frame->parent)
        for (oasp_string_builder *builder = frame->string_builders; builder; builder = builder->next)
            if (builder->data == value->as.string.data && builder->length == value->as.string.length)
                builder->shared = true;
}

static void oasp_runtime_invalidate_builder(oasp_runtime *runtime, oasp_binding *binding) {
    if (!runtime || !binding)
        return;
    for (oasp_string_builder *builder = runtime->string_builders; builder; builder = builder->next)
        if (builder->target == binding)
            builder->target = NULL;
    for (oasp_frame *frame = runtime->frame; frame; frame = frame->parent)
        for (oasp_string_builder *builder = frame->string_builders; builder; builder = builder->next)
            if (builder->target == binding)
                builder->target = NULL;
}

static bool oasp_runtime_write_binding(oasp_runtime *runtime, oasp_binding *binding, const oasp_value *value, bool constant, oasp_slice trace_name) {
    if (!binding || !value)
        return false;
    if (binding->constant) {
        oasp_runtime_error(runtime, 500, "cannot assign to constant", 0);
        return false;
    }
    oasp_runtime_mark_builder_value(runtime, value);
    oasp_runtime_invalidate_builder(runtime, binding);
    binding->dirty = true;
    binding->value = oasp_runtime_value_copy(runtime, value, false);
    oasp_runtime_write_barrier(runtime, binding, &binding->value);
    binding->constant = constant;
    const char *name = oasp_process_options.trace_variable;
    if (name && oasp_ascii_equal_n(trace_name.data, trace_name.length, name)) {
        char *formatted = oasp_value_format(runtime->format_arena, &binding->value);
        fprintf(stderr, "OpenASP variable %.*s = %s\n", (int)trace_name.length, trace_name.data, formatted ? formatted : "");
    }
    return true;
}

const oasp_value *oasp_runtime_get(oasp_runtime *runtime, oasp_slice name) {
    oasp_binding *binding = NULL;
    if (runtime && (!runtime->frame || (uintptr_t)runtime->frame < 4096) &&
        runtime->global_slot_bytecode && runtime->global_slots) {
        for (size_t i = 0; i < runtime->global_slot_count; i++) {
            oasp_slice global_name;
            if (oasp_bytecode_global_slot_at(runtime->global_slot_bytecode, i, &global_name) &&
                oasp_name_equal(global_name, name)) {
                binding = runtime->global_slots[i];
                break;
            }
        }
    }
    if (!binding)
        binding = oasp_runtime_find_binding(runtime, name);
    if (binding)
        oasp_runtime_mark_builder_value(runtime, &binding->value);
    return binding ? &binding->value : NULL;
}

static const oasp_value *oasp_store_get(oasp_binding *store, const char *name) {
    size_t length = strlen(name);
    for (oasp_binding *binding = store; binding; binding = binding->next) {
        if (binding->name.length == length && oasp_ascii_equal_n(binding->name.data, binding->name.length, name))
            return &binding->value;
    }
    return NULL;
}

static bool oasp_store_set(oasp_runtime *runtime, oasp_binding **store, const char *name, const oasp_value *value) {
    size_t length = strlen(name);
    oasp_binding *binding;
    for (binding = *store; binding; binding = binding->next) {
        if (binding->name.length == length && oasp_ascii_equal_n(binding->name.data, binding->name.length, name))
            break;
    }
    if (!binding) {
        binding = oasp_runtime_alloc_binding(runtime);
        if (!binding)
            return false;
        memset(binding, 0, sizeof(*binding));
        binding->name.data = oasp_runtime_lower(runtime, name, length);
        if (!binding->name.data)
            return false;
        binding->name.length = length;
        binding->next = *store;
        *store = binding;
    }
    binding->value = oasp_runtime_value_copy(runtime, value, false);
    oasp_runtime_write_barrier(runtime, binding, &binding->value);
    return true;
}

static void oasp_store_remove(oasp_binding **store, const char *name) {
    if (!store || !name)
        return;
    size_t length = strlen(name);
    oasp_binding **link = store;
    while (*link) {
        oasp_binding *binding = *link;
        if (binding->name.length == length && oasp_ascii_equal_n(binding->name.data, binding->name.length, name)) {
            *link = binding->next;
            return;
        }
        link = &binding->next;
    }
}

bool oasp_runtime_set(oasp_runtime *runtime, oasp_slice name, const oasp_value *value, bool constant) {
    if (oasp_ascii_equal(name, "err")) {
        int64_t number;
        if (value && oasp_value_to_int(value, &number) && !number) {
            oasp_runtime_clear_error(runtime);
            return true;
        }
        oasp_runtime_error(runtime, 438, "Err only supports assignment to zero", 0);
        return false;
    }
    oasp_binding *binding = oasp_runtime_find_binding(runtime, name);
    if (!binding) {
        binding = oasp_runtime_alloc_binding(runtime);
        if (!binding)
            return false;
        memset(binding, 0, sizeof(*binding));
        binding->name.data = oasp_runtime_lower(runtime, name.data, name.length);
        if (!binding->name.data)
            return false;
        binding->name.length = name.length;
        oasp_binding **destination = runtime->frame ? &runtime->frame->locals : &runtime->variables;
        binding->next = *destination;
        *destination = binding;
    }
    return oasp_runtime_write_binding(runtime, binding, value, constant, name);
}

static bool oasp_runtime_prepare_global_slots(oasp_runtime *runtime, const oasp_bytecode *bytecode) {
    if (runtime->global_slot_bytecode == bytecode && runtime->global_slot_count == bytecode->global_slot_count)
        return true;
    oasp_binding **slots =
        bytecode->global_slot_count ? oasp_runtime_alloc(runtime, bytecode->global_slot_count * sizeof(*slots), _Alignof(oasp_binding *)) : NULL;
    if (bytecode->global_slot_count && !slots)
        return false;
    if (slots)
        memset(slots, 0, bytecode->global_slot_count * sizeof(*slots));
    runtime->global_slot_bytecode = bytecode;
    runtime->global_slots = slots;
    runtime->global_slot_count = bytecode->global_slot_count;
    oasp_value empty = oasp_value_empty();
    for (size_t i = 0; i < bytecode->global_slot_count; i++) {
        oasp_slice name;
        if (!oasp_bytecode_global_slot_at(bytecode, i, &name))
            return false;
        oasp_binding *binding = oasp_binding_find(runtime->variables, name);
        if (!binding) {
            if (!oasp_runtime_declare(runtime, name, &empty, false))
                return false;
            binding = oasp_binding_find(runtime->variables, name);
        }
        runtime->global_slots[i] = binding;
    }
    return true;
}

const oasp_value *oasp_runtime_get_global_slot(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t slot) {
    if (!runtime || !bytecode || slot >= bytecode->global_slot_count || !oasp_runtime_prepare_global_slots(runtime, bytecode))
        return NULL;
    oasp_binding *binding = runtime->global_slots[slot];
    if (!binding) {
        oasp_slice name;
        if (!oasp_bytecode_global_slot_at(bytecode, slot, &name))
            return NULL;
        binding = oasp_binding_find(runtime->variables, name);
        runtime->global_slots[slot] = binding;
    }
    if (binding && binding->value.kind == OASP_VALUE_EMPTY) {
        oasp_slice name;
        if (oasp_bytecode_global_slot_at(bytecode, slot, &name)) {
            oasp_binding *resolved = oasp_binding_find_promote(&runtime->variables, name);
            if (resolved && resolved != binding && resolved->value.kind != OASP_VALUE_EMPTY) {
                binding = resolved;
                runtime->global_slots[slot] = binding;
            }
        }
    }
    if (binding)
        oasp_runtime_mark_builder_value(runtime, &binding->value);
    return binding ? &binding->value : NULL;
}

bool oasp_runtime_set_global_slot(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t slot, const oasp_value *value, bool constant) {
    if (!runtime || !bytecode || slot >= bytecode->global_slot_count || !oasp_runtime_prepare_global_slots(runtime, bytecode))
        return false;
    oasp_binding *binding = runtime->global_slots[slot];
    oasp_slice name;
    if (!oasp_bytecode_global_slot_at(bytecode, slot, &name))
        return false;
    if (!binding) {
        binding = oasp_binding_find(runtime->variables, name);
        if (!binding) {
            binding = oasp_runtime_alloc_binding(runtime);
            if (!binding)
                return false;
            memset(binding, 0, sizeof(*binding));
            binding->name.data = oasp_runtime_lower(runtime, name.data, name.length);
            if (!binding->name.data)
                return false;
            binding->name.length = name.length;
            binding->next = runtime->variables;
            runtime->variables = binding;
        }
        runtime->global_slots[slot] = binding;
    }
    return oasp_runtime_write_binding(runtime, binding, value, constant, name);
}

const oasp_value *oasp_runtime_get_local_slot(const oasp_runtime *runtime, size_t slot) {
    if (!runtime || !runtime->frame || slot >= runtime->frame->slot_count)
        return NULL;
    oasp_binding *binding = oasp_binding_resolve(&runtime->frame->slots[slot]);
    if (binding)
        oasp_runtime_mark_builder_value((oasp_runtime *)runtime, &binding->value);
    return binding ? &binding->value : NULL;
}

bool oasp_runtime_set_local_slot(oasp_runtime *runtime, size_t slot, const oasp_value *value, bool constant) {
    if (!runtime || !runtime->frame || slot >= runtime->frame->slot_count)
        return false;
    oasp_binding *binding = oasp_binding_resolve(&runtime->frame->slots[slot]);
    return oasp_runtime_write_binding(runtime, binding, value, constant, binding->name);
}

static bool oasp_runtime_declare(oasp_runtime *runtime, oasp_slice name, const oasp_value *value, bool constant) {
    oasp_binding **destination = runtime->frame ? &runtime->frame->locals : &runtime->variables;
    oasp_binding *binding = runtime->frame ? oasp_frame_find_slot(runtime->frame, name) : NULL;
    if (!binding)
        binding = oasp_binding_find(*destination, name);
    if (binding) {
        if ((constant || value->kind == OASP_VALUE_ARRAY) && binding->value.kind == OASP_VALUE_EMPTY)
            return oasp_runtime_write_binding(runtime, binding, value, constant, name);
        return true;
    }
    binding = oasp_runtime_alloc_binding(runtime);
    if (!binding)
        return false;
    memset(binding, 0, sizeof(*binding));
    binding->name.data = oasp_runtime_lower(runtime, name.data, name.length);
    if (!binding->name.data)
        return false;
    binding->name.length = name.length;
    binding->value = oasp_runtime_value_copy(runtime, value, false);
    oasp_runtime_write_barrier(runtime, binding, &binding->value);
    binding->constant = constant;
    binding->next = *destination;
    *destination = binding;
    return true;
}

static oasp_value oasp_runtime_string(oasp_runtime *runtime, const char *text, size_t length) {
    char *copy = oasp_runtime_copy(runtime, text, length);
    return copy ? oasp_value_string(copy, length) : oasp_value_empty();
}

static const char *oasp_runtime_named_error(oasp_runtime *runtime, const char *prefix, oasp_slice name) {
    size_t prefix_length = strlen(prefix);
    char *message = oasp_runtime_alloc(runtime, prefix_length + name.length + 1, _Alignof(char));
    if (!message)
        return prefix;
    memcpy(message, prefix, prefix_length);
    memcpy(message + prefix_length, name.data, name.length);
    message[prefix_length + name.length] = '\0';
    return message;
}

static bool oasp_value_numeric(const oasp_value *value) {
    return value->kind == OASP_VALUE_EMPTY || value->kind == OASP_VALUE_BOOL || value->kind == OASP_VALUE_INT || value->kind == OASP_VALUE_FLOAT;
}

static oasp_value oasp_runtime_date_value(oasp_runtime *runtime, time_t timestamp);

static bool oasp_parse_date_text(const char *text, struct tm *parts) {
    if (!text || !parts)
        return false;
    while (isspace((unsigned char)*text))
        text++;
    int year = 0;
    int month = 0;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int consumed = 0;
    bool parsed = false;
    int first = 0;
    int second_value = 0;
    int third = 0;
    if (sscanf(text, "%d-%d-%d %d:%d:%d%n", &year, &month, &day, &hour, &minute, &second, &consumed) == 6) {
        parsed = true;
    } else if (sscanf(text, "%d-%d-%d%n", &year, &month, &day, &consumed) == 3) {
        parsed = true;
    } else if (sscanf(text, "%d/%d/%d %d:%d:%d%n", &first, &second_value, &third, &hour, &minute, &second, &consumed) == 6) {
        month = first;
        day = second_value;
        year = third;
        parsed = true;
    } else if (sscanf(text, "%d/%d/%d%n", &first, &second_value, &third, &consumed) == 3) {
        month = first;
        day = second_value;
        year = third;
        parsed = true;
    } else if (sscanf(text, "%d-%d%n", &year, &month, &consumed) == 2 || sscanf(text, "%d/%d%n", &year, &month, &consumed) == 2) {
        day = 1;
        parsed = true;
    }
    if (!parsed)
        return false;
    /* Match the conventional VBScript two-digit year window used by Jet dates. */
    if (year >= 0 && year < 100)
        year += year < 30 ? 2000 : 1900;
    while (isspace((unsigned char)text[consumed]))
        consumed++;
    if (text[consumed] != '\0' || year < 1 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59)
        return false;
    memset(parts, 0, sizeof(*parts));
    parts->tm_year = year - 1900;
    parts->tm_mon = month - 1;
    parts->tm_mday = day;
    parts->tm_hour = hour;
    parts->tm_min = minute;
    parts->tm_sec = second;
    parts->tm_isdst = -1;
    struct tm original = *parts;
    time_t timestamp = mktime(parts);
    return timestamp != (time_t)-1 && parts->tm_year == original.tm_year && parts->tm_mon == original.tm_mon && parts->tm_mday == original.tm_mday &&
           parts->tm_hour == original.tm_hour && parts->tm_min == original.tm_min && parts->tm_sec == original.tm_sec;
}

static bool oasp_value_to_time(oasp_runtime *runtime, const oasp_value *value, time_t *timestamp) {
    if (!value || !timestamp || (value->kind != OASP_VALUE_DATE && value->kind != OASP_VALUE_STRING))
        return false;
    char *text = oasp_value_format(runtime->format_arena, value);
    struct tm parts;
    if (!oasp_parse_date_text(text, &parts))
        return false;
    *timestamp = mktime(&parts);
    return *timestamp != (time_t)-1;
}

static int oasp_days_in_month(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0)))
        return 29;
    return month >= 1 && month <= 12 ? days[month - 1] : 0;
}

static size_t oasp_concat_part(const oasp_value *value, char *output, size_t capacity) {
    const char *text = "";
    size_t length = 0;
    char formatted[64];
    switch (value->kind) {
    case OASP_VALUE_EMPTY:
    case OASP_VALUE_NULL:
    case OASP_VALUE_NOTHING:
        break;
    case OASP_VALUE_BOOL:
        text = value->as.boolean ? "True" : "False";
        length = value->as.boolean ? 4 : 5;
        break;
    case OASP_VALUE_INT: {
        int written = snprintf(formatted, sizeof(formatted), "%lld", (long long)value->as.integer);
        if (written < 0 || (size_t)written >= sizeof(formatted))
            return SIZE_MAX;
        text = formatted;
        length = (size_t)written;
        break;
    }
    case OASP_VALUE_FLOAT: {
        int written = snprintf(formatted, sizeof(formatted), "%.15g", value->as.floating);
        if (written < 0 || (size_t)written >= sizeof(formatted))
            return SIZE_MAX;
        text = formatted;
        length = (size_t)written;
        break;
    }
    case OASP_VALUE_STRING:
        text = value->as.string.data;
        length = value->as.string.length;
        break;
    case OASP_VALUE_OBJECT:
    case OASP_VALUE_ARRAY:
        text = "[object]";
        length = 8;
        break;
    case OASP_VALUE_DATE:
        text = value->as.date.data;
        length = value->as.date.length;
        break;
    }
    if (output && length <= capacity)
        memcpy(output, text, length);
    return length;
}

static oasp_string_builder *oasp_runtime_string_builder(oasp_binding *target, oasp_string_builder **list, oasp_arena *metadata_arena) {
    for (oasp_string_builder *builder = *list; builder; builder = builder->next)
        if (builder->target == target)
            return builder;
    oasp_string_builder *builder =
        oasp_arena_alloc(metadata_arena, sizeof(*builder), _Alignof(oasp_string_builder));
    if (!builder)
        return NULL;
    memset(builder, 0, sizeof(*builder));
    builder->target = target;
    builder->next = *list;
    *list = builder;
    return builder;
}

static bool oasp_runtime_append_binding_values(oasp_runtime *runtime, oasp_binding *target, oasp_string_builder **list, oasp_arena *metadata_arena, const oasp_value *suffixes,
                                               size_t count, size_t line) {
    if (!runtime || !target || !list || !metadata_arena || (!suffixes && count))
        return false;
    if (target->constant) {
        oasp_runtime_error(runtime, 500, "cannot assign to constant", line);
        return false;
    }
    size_t prefix_length = oasp_concat_part(&target->value, NULL, 0);
    size_t total = prefix_length;
    if (prefix_length == SIZE_MAX) {
        oasp_runtime_error(runtime, 7, "out of memory", line);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        size_t length = oasp_concat_part(&suffixes[i], NULL, 0);
        if (length == SIZE_MAX || total > SIZE_MAX - length - 1) {
            oasp_runtime_error(runtime, 7, "out of memory", line);
            return false;
        }
        total += length;
    }
    oasp_string_builder *builder = oasp_runtime_string_builder(target, list, metadata_arena);
    if (!builder)
        return false;
    bool current = target->value.kind == OASP_VALUE_STRING &&
                   target->value.as.string.data == builder->data &&
                   target->value.as.string.length == builder->length;
    if (!current)
        builder->shared = true;
    if (!current || builder->shared || total > builder->capacity) {
        size_t capacity = builder->capacity;
        if (capacity < 64)
            capacity = 64;
        while (capacity < total) {
            if (capacity > (SIZE_MAX - 1) / 2) {
                capacity = total;
                break;
            }
            capacity *= 2;
        }
        char *data = oasp_runtime_alloc(runtime, capacity + 1, _Alignof(char));
        if (!data) {
            oasp_runtime_error(runtime, 7, "out of memory", line);
            return false;
        }
        if (prefix_length)
            (void)oasp_concat_part(&target->value, data, prefix_length);
        builder->data = data;
        builder->capacity = capacity;
    }
    size_t offset = prefix_length;
    for (size_t i = 0; i < count; i++)
        offset += oasp_concat_part(&suffixes[i], builder->data + offset, total - offset);
    builder->data[total] = '\0';
    builder->length = total;
    builder->shared = false;
    target->value = oasp_value_string(builder->data, total);
    target->dirty = true;
    return true;
}

bool oasp_runtime_append_global_slot(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t slot, const oasp_value *suffix, size_t line) {
    return oasp_runtime_append_global_slot_values(runtime, bytecode, slot, suffix, 1, line);
}

bool oasp_runtime_append_global_slot_values(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t slot, const oasp_value *suffixes, size_t count,
                                            size_t line) {
    if (!runtime || !bytecode || slot >= bytecode->global_slot_count ||
        !oasp_runtime_prepare_global_slots(runtime, bytecode))
        return false;
    oasp_binding *binding = runtime->global_slots[slot];
    return binding && oasp_runtime_append_binding_values(runtime, binding, &runtime->string_builders,
                                                          runtime->arena, suffixes, count, line);
}

bool oasp_runtime_append_local_slot(oasp_runtime *runtime, size_t slot, const oasp_value *suffix, size_t line) {
    return oasp_runtime_append_local_slot_values(runtime, slot, suffix, 1, line);
}

bool oasp_runtime_append_local_slot_values(oasp_runtime *runtime, size_t slot, const oasp_value *suffixes, size_t count, size_t line) {
    if (!runtime || !runtime->frame || slot >= runtime->frame->slot_count)
        return false;
    oasp_binding *binding = oasp_binding_resolve(&runtime->frame->slots[slot]);
    oasp_arena *metadata_arena = runtime->frame->transient ? runtime->scratch : runtime->arena;
    return binding && oasp_runtime_append_binding_values(runtime, binding, &runtime->frame->string_builders,
                                                          metadata_arena, suffixes, count, line);
}

static bool oasp_runtime_write_text(oasp_runtime *runtime, const char *data, size_t length, size_t line) {
    if (!runtime || (!data && length))
        return false;
    const char *charset = oasp_request_charset(runtime->request);
    bool ascii = true;
    for (size_t i = 0; i < length; i++) {
        if ((unsigned char)data[i] >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (!length || ascii || !charset[0] || !strcasecmp(charset, "utf-8") || !strcasecmp(charset, "utf8")) {
        oasp_request_write(runtime->request, data, length);
    } else {
        oasp_buffer converted;
        if (!oasp_text_convert(&converted, data, length, "UTF-8", charset)) {
            const char *description = errno == EILSEQ || errno == EINVAL
                                          ? "response text is not valid UTF-8 or the charset is unsupported; use UTF-8 internally and a charset supported by iconv"
                                          : "response charset conversion failed; check available memory and the selected Response.Charset";
            oasp_runtime_error(runtime, 5, description, line);
            return false;
        }
        oasp_request_write(runtime->request, converted.data, converted.length);
        oasp_buffer_dispose(&converted);
    }
    if (oasp_request_failed(runtime->request)) {
        oasp_runtime_error(runtime, 5, oasp_request_error(runtime->request), line);
        return false;
    }
    return true;
}

bool oasp_runtime_write_value(oasp_runtime *runtime, const oasp_value *value, size_t line) {
    char buffer[64];
    oasp_slice text;
    if (!runtime || !value || !oasp_value_text(value, buffer, sizeof(buffer), &text))
        return false;
    return oasp_runtime_write_text(runtime, text.data, text.length, line);
}

bool oasp_runtime_write_values(oasp_runtime *runtime, const oasp_value *values, size_t count, size_t line) {
    if (!runtime || (!values && count))
        return false;
    for (size_t i = 0; i < count; i++)
        if (!oasp_runtime_write_value(runtime, &values[i], line))
            return false;
    return true;
}

bool oasp_runtime_write_htmlencoded_value(oasp_runtime *runtime, const oasp_value *value, size_t line) {
    char formatted[64];
    oasp_slice text;
    if (!runtime || !value || !oasp_value_text(value, formatted, sizeof(formatted), &text))
        return false;
    size_t start = 0;
    for (size_t i = 0; i < text.length; i++) {
        const char *escaped = text.data[i] == '&'    ? "&amp;"
                              : text.data[i] == '<'  ? "&lt;"
                              : text.data[i] == '>'  ? "&gt;"
                              : text.data[i] == '"'  ? "&quot;"
                              : text.data[i] == '\'' ? "&#39;"
                                                     : NULL;
        if (!escaped)
            continue;
        if (i > start)
            if (!oasp_runtime_write_text(runtime, text.data + start, i - start, line))
                return false;
        if (!oasp_runtime_write_text(runtime, escaped, strlen(escaped), line))
            return false;
        start = i + 1;
    }
    if (start < text.length)
        return oasp_runtime_write_text(runtime, text.data + start, text.length - start, line);
    return true;
}

oasp_value oasp_runtime_concat_values(oasp_runtime *runtime, oasp_value *values, size_t count, size_t line) {
    if (!runtime || (!values && count))
        return oasp_value_empty();
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        size_t length = oasp_concat_part(&values[i], NULL, 0);
        if (length == SIZE_MAX || total > SIZE_MAX - length - 1) {
            oasp_runtime_error(runtime, 7, "out of memory", line);
            return oasp_value_empty();
        }
        total += length;
    }
    char *result = oasp_runtime_alloc(runtime, total + 1, _Alignof(char));
    if (!result) {
        oasp_runtime_error(runtime, 7, "out of memory", line);
        return oasp_value_empty();
    }
    size_t offset = 0;
    for (size_t i = 0; i < count; i++)
        offset += oasp_concat_part(&values[i], result + offset, total - offset);
    result[total] = '\0';
    return oasp_value_string(result, total);
}

static oasp_value oasp_concat(oasp_runtime *runtime, const oasp_value *left, const oasp_value *right) {
    oasp_value values[2] = {*left, *right};
    return oasp_runtime_concat_values(runtime, values, 2, 0);
}

static int oasp_compare(oasp_runtime *runtime, const oasp_value *left, const oasp_value *right) {
    (void)runtime;
    double a, b;
    if (oasp_value_numeric(left) && oasp_value_numeric(right) && oasp_value_to_float(left, &a) && oasp_value_to_float(right, &b))
        return a < b ? -1 : a > b ? 1 : 0;
    char left_buffer[64];
    char right_buffer[64];
    oasp_slice left_text;
    oasp_slice right_text;
    if (!oasp_value_text(left, left_buffer, sizeof(left_buffer), &left_text) ||
        !oasp_value_text(right, right_buffer, sizeof(right_buffer), &right_text))
        return 0;
    size_t common = left_text.length < right_text.length ? left_text.length : right_text.length;
    int result = common ? memcmp(left_text.data, right_text.data, common) : 0;
    if (!result)
        result = left_text.length < right_text.length ? -1 : left_text.length > right_text.length ? 1 : 0;
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

static oasp_value oasp_binary_numeric(oasp_runtime *runtime, oasp_expr_kind kind, const oasp_value *left, const oasp_value *right, size_t line) {
    int64_t left_int, right_int;
    int64_t integer_result;
    double left_float, right_float;
    if (kind == OASP_EXPR_ADD && left->kind == OASP_VALUE_STRING && right->kind == OASP_VALUE_STRING)
        return oasp_concat(runtime, left, right);
    if ((kind == OASP_EXPR_ADD || kind == OASP_EXPR_SUBTRACT) && left->kind == OASP_VALUE_DATE) {
        time_t timestamp;
        double days;
        if (oasp_value_to_time(runtime, left, &timestamp) && oasp_value_to_float(right, &days))
            return oasp_runtime_date_value(runtime, timestamp + (time_t)((kind == OASP_EXPR_ADD ? days : -days) * 86400.0));
    }
    if (!oasp_value_to_float(left, &left_float) || !oasp_value_to_float(right, &right_float)) {
        oasp_runtime_error(runtime, 13, "type mismatch", line);
        return oasp_value_empty();
    }
    switch (kind) {
    case OASP_EXPR_ADD:
        if (oasp_value_to_int(left, &left_int) && oasp_value_to_int(right, &right_int) &&
            left->kind != OASP_VALUE_FLOAT && right->kind != OASP_VALUE_FLOAT) {
            if (__builtin_add_overflow(left_int, right_int, &integer_result)) {
                oasp_runtime_error(runtime, 6, "overflow", line);
                return oasp_value_empty();
            }
            return oasp_value_int(integer_result);
        }
        return oasp_value_float(left_float + right_float);
    case OASP_EXPR_SUBTRACT:
        if (oasp_value_to_int(left, &left_int) && oasp_value_to_int(right, &right_int) &&
            left->kind != OASP_VALUE_FLOAT && right->kind != OASP_VALUE_FLOAT) {
            if (__builtin_sub_overflow(left_int, right_int, &integer_result)) {
                oasp_runtime_error(runtime, 6, "overflow", line);
                return oasp_value_empty();
            }
            return oasp_value_int(integer_result);
        }
        return oasp_value_float(left_float - right_float);
    case OASP_EXPR_MULTIPLY:
        if (oasp_value_to_int(left, &left_int) && oasp_value_to_int(right, &right_int) &&
            left->kind != OASP_VALUE_FLOAT && right->kind != OASP_VALUE_FLOAT) {
            if (__builtin_mul_overflow(left_int, right_int, &integer_result)) {
                oasp_runtime_error(runtime, 6, "overflow", line);
                return oasp_value_empty();
            }
            return oasp_value_int(integer_result);
        }
        return oasp_value_float(left_float * right_float);
    case OASP_EXPR_DIVIDE:
        if (right_float == 0.0) {
            oasp_runtime_error(runtime, 11, "division by zero", line);
            return oasp_value_empty();
        }
        return oasp_value_float(left_float / right_float);
    case OASP_EXPR_INT_DIVIDE:
    case OASP_EXPR_MOD:
        if (!oasp_value_to_int(left, &left_int) || !oasp_value_to_int(right, &right_int)) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        if (!right_int) {
            oasp_runtime_error(runtime, 11, "division by zero", line);
            return oasp_value_empty();
        }
        if (left_int == INT64_MIN && right_int == -1) {
            oasp_runtime_error(runtime, 6, "overflow", line);
            return oasp_value_empty();
        }
        return oasp_value_int(kind == OASP_EXPR_MOD ? left_int % right_int : left_int / right_int);
    case OASP_EXPR_POWER:
        return oasp_value_float(pow(left_float, right_float));
    default:
        return oasp_value_empty();
    }
}

static bool oasp_split_member(oasp_slice name, oasp_slice *base, oasp_slice *member) {
    for (size_t i = 0; i < name.length; i++) {
        if (name.data[i] != '.')
            continue;
        *base = (oasp_slice){name.data, i};
        *member = (oasp_slice){name.data + i + 1, name.length - i - 1};
        return true;
    }
    return false;
}

static int oasp_routine_name_compare(oasp_slice left, oasp_slice right) {
    size_t common = left.length < right.length ? left.length : right.length;
    for (size_t i = 0; i < common; i++) {
        int difference = tolower((unsigned char)left.data[i]) - tolower((unsigned char)right.data[i]);
        if (difference)
            return difference;
    }
    return left.length < right.length ? -1 : left.length > right.length ? 1 : 0;
}

static oasp_routine_def *oasp_find_routine_id(oasp_runtime *runtime, uint32_t symbol_id, oasp_slice name) {
    if (!runtime || !runtime->routines)
        return NULL;
    size_t low = 0;
    size_t high = runtime->routines->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (runtime->routines->items[middle].symbol_id < symbol_id)
            low = middle + 1;
        else
            high = middle;
    }
    for (size_t i = low; i < runtime->routines->count && runtime->routines->items[i].symbol_id == symbol_id; i++) {
        oasp_routine_def *routine = &runtime->routines->items[i];
        oasp_slice routine_name;
        if (oasp_bytecode_routine_name_at(routine->bytecode, routine->routine_index, &routine_name) &&
            oasp_name_equal(routine_name, name))
            return routine;
    }
    return NULL;
}

static oasp_routine_def *oasp_find_routine(oasp_runtime *runtime, oasp_slice name) {
    return oasp_find_routine_id(runtime, oasp_symbol_id(name), name);
}

static void oasp_routine_table_finalize(void *context) {
    oasp_routine_table *table = context;
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->capacity = 0;
}

static bool oasp_register_routine(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t routine_index, oasp_slice name) {
    uint32_t symbol_id = oasp_symbol_id(name);
    if (oasp_find_routine_id(runtime, symbol_id, name))
        return true;
    if (!runtime->routines) {
        runtime->routines = oasp_runtime_alloc(runtime, sizeof(*runtime->routines), _Alignof(oasp_routine_table));
        if (!runtime->routines)
            return false;
        memset(runtime->routines, 0, sizeof(*runtime->routines));
        if (!oasp_runtime_defer(runtime, runtime->routines, oasp_routine_table_finalize,
                                runtime->routines))
            return false;
    }
    oasp_routine_table *table = runtime->routines;
    if (table->count == table->capacity) {
        size_t capacity = table->capacity ? table->capacity * 2 : 8;
        if (capacity < table->capacity || capacity > SIZE_MAX / sizeof(*table->items))
            return false;
        oasp_routine_def *items = realloc(table->items, capacity * sizeof(*items));
        if (!items)
            return false;
        table->items = items;
        table->capacity = capacity;
    }
    size_t position = 0;
    while (position < table->count) {
        oasp_routine_def *candidate = &table->items[position];
        if (candidate->symbol_id > symbol_id)
            break;
        if (candidate->symbol_id == symbol_id) {
            oasp_slice candidate_name;
            if (!oasp_bytecode_routine_name_at(candidate->bytecode, candidate->routine_index, &candidate_name))
                return false;
            if (oasp_routine_name_compare(candidate_name, name) > 0)
                break;
        }
        position++;
    }
    memmove(&table->items[position + 1], &table->items[position],
            (table->count - position) * sizeof(*table->items));
    table->items[position] = (oasp_routine_def){bytecode, routine_index, symbol_id, 0};
    table->count++;
    return true;
}

static oasp_class_def *oasp_find_class(const oasp_runtime *runtime, oasp_slice name) {
    for (oasp_class_def *class_def = runtime->classes; class_def; class_def = class_def->next)
        if (oasp_name_equal(class_def->name, name))
            return class_def;
    return NULL;
}

typedef struct {
    const oasp_bytecode *bytecode;
    size_t index;
} oasp_routine_ref;

static oasp_routine_ref oasp_find_method(const oasp_object *object, oasp_slice name, bool default_only) {
    oasp_routine_ref result = {0};
    if (!object || !object->class_def || !object->class_def->resolved || !object->class_def->bytecode)
        return result;
    oasp_class_bytecode class_def;
    const oasp_bytecode *bytecode = object->class_def->bytecode;
    if (!oasp_bytecode_class_at(bytecode, object->class_def->class_index, &class_def))
        return result;
    for (size_t i = 0; i < class_def.routine_count; i++) {
        size_t index = class_def.routine_offset + i;
        oasp_routine_bytecode routine;
        if (!oasp_bytecode_routine_at(bytecode, index, &routine))
            return result;
        if (default_only ? routine.is_default : oasp_name_equal(routine.name, name))
            return (oasp_routine_ref){bytecode, index};
    }
    return result;
}

static oasp_routine_ref oasp_find_accessor(const oasp_object *object, oasp_slice name, oasp_routine_kind first, oasp_routine_kind second) {
    oasp_routine_ref result = {0};
    if (!object || !object->class_def || !object->class_def->resolved || !object->class_def->bytecode)
        return result;
    if (oasp_bytecode_find_routine(object->class_def->bytecode, name, object->class_def->class_index,
                                   first, second, false, &result.index))
        result.bytecode = object->class_def->bytecode;
    return result;
}

static oasp_routine_ref oasp_find_default_accessor(const oasp_object *object, oasp_routine_kind first, oasp_routine_kind second) {
    oasp_routine_ref result = {0};
    if (!object || !object->class_def || !object->class_def->resolved || !object->class_def->bytecode)
        return result;
    const oasp_bytecode *bytecode = object->class_def->bytecode;
    oasp_class_bytecode class_def;
    if (!oasp_bytecode_class_at(bytecode, object->class_def->class_index, &class_def))
        return result;
    for (size_t i = 0; i < class_def.routine_count; i++) {
        size_t index = class_def.routine_offset + i;
        oasp_routine_bytecode routine;
        if (!oasp_bytecode_routine_at(bytecode, index, &routine))
            return result;
        if (!routine.is_default)
            continue;
        if (routine.kind == first || routine.kind == second) {
            result.bytecode = bytecode;
            result.index = index;
            return result;
        }
        if (oasp_bytecode_find_routine(bytecode, routine.name, object->class_def->class_index,
                                       first, second, false, &result.index)) {
            result.bytecode = bytecode;
            return result;
        }
    }
    return result;
}

static oasp_routine_ref oasp_find_assignment_accessor(const oasp_object *object, oasp_slice name, const oasp_value *value) {
    oasp_routine_kind first = value && value->kind == OASP_VALUE_OBJECT
                                  ? OASP_ROUTINE_PROPERTY_SET : OASP_ROUTINE_PROPERTY_LET;
    oasp_routine_kind second = first == OASP_ROUTINE_PROPERTY_SET
                                   ? OASP_ROUTINE_PROPERTY_LET : OASP_ROUTINE_PROPERTY_SET;
    oasp_routine_ref result = oasp_find_accessor(object, name, first, second);
    if (result.bytecode)
        return result;

    result = oasp_find_method(object, name, false);
    oasp_routine_bytecode routine;
    if (result.bytecode &&
        (!oasp_bytecode_routine_at(result.bytecode, result.index, &routine) ||
         (routine.kind != first && routine.kind != second))) {
        result = (oasp_routine_ref){0};
    }
    return result;
}

static oasp_routine_ref oasp_find_default_assignment_accessor(const oasp_object *object, const oasp_value *value) {
    if (value && value->kind == OASP_VALUE_OBJECT)
        return oasp_find_default_accessor(object, OASP_ROUTINE_PROPERTY_SET, OASP_ROUTINE_PROPERTY_LET);
    return oasp_find_default_accessor(object, OASP_ROUTINE_PROPERTY_LET, OASP_ROUTINE_PROPERTY_SET);
}

static void oasp_resolve_persisted_class(oasp_runtime *runtime, oasp_object *object) {
    if (!object || !object->class_def || object->class_def->resolved)
        return;
    oasp_class_def *current = oasp_find_class(runtime, object->class_def->name);
    if (current) {
        object->class_def = current;
        if (runtime->allocator)
            oasp_allocator_write_barrier(runtime->allocator, object, current);
    }
}

static oasp_value oasp_array_create(oasp_runtime *runtime, const oasp_stmt *statement, size_t declaration, size_t line) {
    size_t dimension_count = oasp_ast_statement_declaration_dimension_count(statement, declaration);
    if (!dimension_count)
        return oasp_value_empty();
    oasp_array *array = oasp_runtime_alloc_array(runtime);
    if (!array)
        return oasp_value_empty();
    memset(array, 0, sizeof(*array));
    array->rank = dimension_count;
    array->bounds = oasp_runtime_alloc(runtime, array->rank * sizeof(*array->bounds), _Alignof(int64_t));
    if (!array->bounds)
        return oasp_value_empty();
    array->length = 1;
    for (size_t i = 0; i < array->rank; i++) {
        oasp_value bound_value = oasp_runtime_eval(runtime, oasp_ast_statement_declaration_dimension(statement, declaration, i));
        int64_t upper = 0;
        if (!oasp_value_to_int(&bound_value, &upper) || upper < -1) {
            oasp_runtime_error(runtime, 9, "subscript out of range", line);
            return oasp_value_empty();
        }
        size_t extent = (size_t)(upper + 1);
        if (extent && array->length > SIZE_MAX / extent) {
            oasp_runtime_error(runtime, 9, "subscript out of range", line);
            return oasp_value_empty();
        }
        array->bounds[i] = upper;
        array->length *= extent;
    }
    if (array->length) {
        array->items = oasp_runtime_alloc_values(runtime, array->length);
        if (!array->items)
            return oasp_value_empty();
        memset(array->items, 0, array->length * sizeof(*array->items));
    }
    oasp_value value = oasp_value_empty();
    value.kind = OASP_VALUE_ARRAY;
    value.as.array = array;
    return value;
}

static bool oasp_array_offset(oasp_runtime *runtime, oasp_array *array, oasp_value *indices, size_t count, size_t line,
                              size_t *offset) {
    if (oasp_process_options.trace_array) {
        fprintf(stderr, "OpenASP array: line=%zu rank=%zu count=%zu length=%zu", line, array ? array->rank : 0, count, array ? array->length : 0);
        if (array)
            for (size_t i = 0; i < array->rank; i++)
                fprintf(stderr, " bound[%zu]=%lld", i, (long long)array->bounds[i]);
        fputc('\n', stderr);
    }
    if (!array || count != array->rank) {
        oasp_runtime_error(runtime, 9, "subscript out of range", line);
        return false;
    }
    size_t result = 0;
    for (size_t i = 0; i < count; i++) {
        int64_t index = 0;
        if (!oasp_value_to_int(&indices[i], &index) || index < 0 || index > array->bounds[i]) {
            oasp_runtime_error(runtime, 9, "subscript out of range", line);
            return false;
        }
        result = result * (size_t)(array->bounds[i] + 1) + (size_t)index;
    }
    *offset = result;
    return true;
}

static bool oasp_array_preserve(oasp_runtime *runtime, oasp_array *destination, const oasp_array *source, size_t line) {
    if (!destination || !source || destination->rank != source->rank) {
        oasp_runtime_error(runtime, 9, "subscript out of range", line);
        return false;
    }
    for (size_t i = 0; i + 1 < source->rank; i++) {
        if (destination->bounds[i] != source->bounds[i]) {
            oasp_runtime_error(runtime, 9, "subscript out of range", line);
            return false;
        }
    }
    if (!source->rank || !source->length || !destination->length)
        return true;
    size_t source_width = (size_t)(source->bounds[source->rank - 1] + 1);
    size_t destination_width = (size_t)(destination->bounds[destination->rank - 1] + 1);
    size_t copy_width = source_width < destination_width ? source_width : destination_width;
    size_t rows = source_width ? source->length / source_width : 0;
    for (size_t row = 0; row < rows; row++) {
        for (size_t column = 0; column < copy_width; column++) {
            size_t source_offset = row * source_width + column;
            size_t destination_offset = row * destination_width + column;
            destination->items[destination_offset] = oasp_runtime_value_copy(runtime, &source->items[source_offset], false);
            oasp_runtime_write_barrier(runtime, destination, &destination->items[destination_offset]);
        }
    }
    return true;
}

bool oasp_runtime_declare_array(oasp_runtime *runtime, oasp_slice name, const oasp_value *bounds, size_t dimension_count, bool preserve,
                                bool declaration, size_t line) {
    const oasp_value *previous = preserve ? oasp_runtime_get(runtime, name) : NULL;
    oasp_value value = oasp_value_empty();
    if (dimension_count) {
        oasp_array *array = oasp_runtime_alloc_array(runtime);
        if (!array)
            return false;
        memset(array, 0, sizeof(*array));
        array->rank = dimension_count;
        array->bounds = oasp_runtime_alloc(runtime, dimension_count * sizeof(*array->bounds), _Alignof(int64_t));
        if (!array->bounds)
            return false;
        array->length = 1;
        for (size_t i = 0; i < dimension_count; i++) {
            int64_t upper = 0;
            if (!oasp_value_to_int(&bounds[i], &upper) || upper < -1) {
                oasp_runtime_error(runtime, 9, "subscript out of range", line);
                return false;
            }
            size_t extent = (size_t)(upper + 1);
            if (extent && array->length > SIZE_MAX / extent) {
                oasp_runtime_error(runtime, 9, "subscript out of range", line);
                return false;
            }
            array->bounds[i] = upper;
            array->length *= extent;
        }
        if (array->length) {
            array->items = oasp_runtime_alloc_values(runtime, array->length);
            if (!array->items)
                return false;
            memset(array->items, 0, array->length * sizeof(*array->items));
        }
        value.kind = OASP_VALUE_ARRAY;
        value.as.array = array;
    }
    if (runtime->error_pending && !runtime->resume_next)
        return false;
    if (preserve && previous && previous->kind == OASP_VALUE_ARRAY &&
        !oasp_array_preserve(runtime, value.as.array, previous->as.array, line))
        return false;
    if (preserve && previous && previous->kind != OASP_VALUE_EMPTY && previous->kind != OASP_VALUE_ARRAY) {
        oasp_runtime_error(runtime, 13, "type mismatch", line);
        return false;
    }
    return declaration ? oasp_runtime_declare(runtime, name, &value, false) : oasp_runtime_set(runtime, name, &value, false);
}

/*
 * Routine calls and compatibility helpers: frame construction, ByRef/copy-back,
 * member resolution, path mapping, Unicode conversion, and request collections.
 */
static bool oasp_assign_target(oasp_runtime *runtime, const oasp_expr *target, const oasp_value *value, size_t line);

#define OASP_ARGUMENT_REFERENCE_MASK ((uintptr_t)3)
#define OASP_ARGUMENT_REFERENCE_LOCAL ((uintptr_t)1)
#define OASP_ARGUMENT_REFERENCE_GLOBAL ((uintptr_t)2)
#define OASP_ARGUMENT_REFERENCE_NAME ((uintptr_t)3)

static oasp_binding *oasp_argument_binding(oasp_runtime *runtime, oasp_frame *caller, const oasp_bytecode *bytecode, uintptr_t reference, const oasp_value *value) {
    uintptr_t kind = reference & OASP_ARGUMENT_REFERENCE_MASK;
    size_t index = reference >> 2;
    if (kind == OASP_ARGUMENT_REFERENCE_LOCAL)
        return caller && index < caller->slot_count
                   ? oasp_binding_resolve(&caller->slots[index])
                   : NULL;
    if (kind == OASP_ARGUMENT_REFERENCE_GLOBAL) {
        if (!bytecode || index >= bytecode->global_slot_count ||
            !oasp_runtime_prepare_global_slots(runtime, bytecode))
            return NULL;
        return runtime->global_slots[index];
    }
    if (kind == OASP_ARGUMENT_REFERENCE_NAME) {
        oasp_slice name;
        if (!bytecode || !oasp_bytecode_name_at(bytecode, index, &name))
            return NULL;
        oasp_frame *current = runtime->frame;
        runtime->frame = caller;
        oasp_binding *binding = oasp_runtime_find_binding(runtime, name);
        if (!binding && (!value || value->kind == OASP_VALUE_EMPTY)) {
            oasp_value empty = oasp_value_empty();
            if (oasp_runtime_set(runtime, name, &empty, false))
                binding = oasp_runtime_find_binding(runtime, name);
        }
        runtime->frame = current;
        return binding;
    }
    return NULL;
}

static const oasp_expr *oasp_argument_copyback_target(oasp_runtime *runtime, const oasp_bytecode *bytecode, uintptr_t reference) {
    if (!runtime || !bytecode || (reference & OASP_ARGUMENT_REFERENCE_MASK) != OASP_ARGUMENT_REFERENCE_NAME)
        return NULL;
    oasp_slice name;
    if (!oasp_bytecode_name_at(bytecode, reference >> 2, &name))
        return NULL;
    oasp_expr *target = oasp_arena_alloc(runtime->scratch, sizeof(*target), _Alignof(oasp_expr));
    if (!target)
        return NULL;
    memset(target, 0, sizeof(*target));
    target->kind = OASP_EXPR_IDENTIFIER;
    target->name = name;
    return target;
}

static oasp_value oasp_invoke_routine_inner(oasp_runtime *runtime, oasp_routine_ref routine_ref, oasp_object *object, oasp_expr *const *arguments, const oasp_value *prepared_arguments,
                                            const uintptr_t *encoded_references, const oasp_bytecode *argument_bytecode, size_t argument_count) {
    oasp_routine_bytecode routine;
    if (!oasp_bytecode_routine_at(routine_ref.bytecode, routine_ref.index, &routine))
        return oasp_value_empty();
    const oasp_bytecode *routine_bytecode = routine_ref.bytecode;
    int caller_error_number = runtime->error_number;
    const char *caller_error_description = runtime->error_description;
    const char *caller_error_source = runtime->error_source;
    size_t caller_error_line = runtime->error_line;
    bool caller_error_pending = runtime->error_pending;
    const oasp_value *evaluated = prepared_arguments;
    if (argument_count && !evaluated) {
        oasp_value *storage = oasp_arena_alloc(runtime->scratch, argument_count * sizeof(*storage), _Alignof(oasp_value));
        if (!storage)
            return oasp_value_empty();
        for (size_t i = 0; i < argument_count; i++)
            storage[i] = arguments[i] ? oasp_runtime_eval(runtime, arguments[i]) : oasp_value_empty();
        evaluated = storage;
    }

    oasp_frame *caller = runtime->frame;
    oasp_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.parent = runtime->frame;
    frame.object = object;
    frame.routine_bytecode = routine_bytecode;
    frame.routine = &routine;
    frame.transient = true;
    runtime->frame = &frame;

    if (routine.slot_count) {
        if (routine.slot_count > SIZE_MAX / sizeof(*frame.slots)) {
            runtime->frame = frame.parent;
            return oasp_value_empty();
        }
        frame.slots = oasp_arena_alloc(runtime->scratch, routine.slot_count * sizeof(*frame.slots),
                                       _Alignof(oasp_binding));
        if (!frame.slots) {
            runtime->frame = frame.parent;
            return oasp_value_empty();
        }
        memset(frame.slots, 0, routine.slot_count * sizeof(*frame.slots));
        frame.slot_count = routine.slot_count;
        for (size_t i = 0; i < routine.slot_count; i++) {
            oasp_slice slot_name;
            if (!oasp_bytecode_routine_slot(routine_bytecode, &routine, i, &slot_name)) {
                runtime->frame = frame.parent;
                return oasp_value_empty();
            }
            frame.slots[i].name = slot_name;
        }
    }

    for (size_t i = 0; i < routine.parameter_count; i++) {
        oasp_parameter_bytecode parameter;
        if (!oasp_bytecode_routine_parameter(routine_bytecode, &routine, i, &parameter))
            break;
        oasp_value value = i < argument_count ? evaluated[i] : oasp_value_empty();
        size_t slot = routine.kind == OASP_ROUTINE_SUB ? i : i + 1;
        if (slot >= frame.slot_count || !oasp_name_equal(frame.slots[slot].name, parameter.name))
            for (slot = 0; slot < frame.slot_count && !oasp_name_equal(frame.slots[slot].name, parameter.name); slot++)
                ;
        if (slot >= frame.slot_count)
            break;
        oasp_binding *local = &frame.slots[slot];
        if (!parameter.by_value && i < argument_count && encoded_references && encoded_references[i]) {
            oasp_binding *source =
                oasp_argument_binding(runtime, caller, argument_bytecode, encoded_references[i], &value);
            if (source) {
                local->reference = source;
                continue;
            }
            local->copyback_target =
                oasp_argument_copyback_target(runtime, argument_bytecode, encoded_references[i]);
        }
        local->value = oasp_runtime_value_copy(runtime, &value, false);
        if (!parameter.by_value && i < argument_count && arguments && arguments[i] &&
            oasp_ast_expression_kind(arguments[i]) == OASP_EXPR_IDENTIFIER) {
            oasp_binding *source = NULL;
            runtime->frame = frame.parent;
            source = oasp_runtime_find_binding(runtime, oasp_ast_expression_name(arguments[i]));
            runtime->frame = &frame;
            if (source) {
                local->reference = source;
            } else {
                local->copyback_target = arguments[i];
            }
        } else if (!parameter.by_value && i < argument_count && arguments && arguments[i] &&
                   oasp_ast_expression_kind(arguments[i]) == OASP_EXPR_MEMBER) {
            local->copyback_target = arguments[i];
        } else if (!parameter.by_value && i < argument_count && arguments && arguments[i] &&
                   oasp_ast_expression_kind(arguments[i]) == OASP_EXPR_CALL && oasp_ast_expression_left(arguments[i]) &&
                   oasp_ast_expression_kind(oasp_ast_expression_left(arguments[i])) == OASP_EXPR_IDENTIFIER) {
            runtime->frame = frame.parent;
            oasp_binding *source = oasp_runtime_find_binding(runtime, oasp_ast_expression_name(oasp_ast_expression_left(arguments[i])));
            runtime->frame = &frame;
            if (source && (source->value.kind == OASP_VALUE_ARRAY || source->value.kind == OASP_VALUE_OBJECT))
                local->copyback_target = arguments[i];
        }
    }
    bool caller_resume_next = runtime->resume_next;
    /*
     * On Error is dynamic procedure state in Classic ASP. A caller that has
     * enabled Resume Next must keep that mode while a callee executes unless
     * the callee explicitly changes it.
     */
    runtime->resume_next = caller_resume_next;
    const oasp_bytecode *caller_bytecode = runtime->bytecode;
    runtime->bytecode = routine_bytecode;
    bool ok = oasp_vm_execute_range_inline_mode(runtime, routine_bytecode, routine.entry, routine.limit,
                                                routine.error_aware);
    runtime->bytecode = caller_bytecode;
    oasp_binding *result_binding =
        routine.kind != OASP_ROUTINE_SUB && frame.slot_count ? oasp_binding_resolve(&frame.slots[0]) : NULL;
    oasp_value result = result_binding ? result_binding->value : oasp_value_empty();
    runtime->frame = frame.parent;
    runtime->resume_next = caller_resume_next;
    if (!ok)
        return oasp_value_empty();
    runtime->error_number = caller_error_number;
    runtime->error_description = caller_error_description;
    runtime->error_source = caller_error_source;
    runtime->error_line = caller_error_line;
    runtime->error_pending = caller_error_pending;
    for (size_t i = 0; i < frame.slot_count; i++) {
        oasp_binding *local = &frame.slots[i];
        if (local->copyback_target && local->dirty &&
            !oasp_assign_target(runtime, local->copyback_target, &local->value,
                                routine.entry < routine_bytecode->length ? routine_bytecode->instructions[routine.entry].line : 0))
            return oasp_value_empty();
    }
    for (oasp_binding *local = frame.locals; local; local = local->next)
        if (local->copyback_target && local->dirty &&
            !oasp_assign_target(runtime, local->copyback_target, &local->value,
                                routine.entry < routine_bytecode->length ? routine_bytecode->instructions[routine.entry].line : 0))
            return oasp_value_empty();
    return result;
}

static oasp_value oasp_invoke_routine(oasp_runtime *runtime, oasp_routine_ref routine, oasp_object *object, oasp_expr *const *arguments, size_t argument_count) {
    if (!runtime || !runtime->scratch || !routine.bytecode)
        return oasp_value_empty();
    oasp_frame *caller = runtime->frame;
    oasp_arena_checkpoint checkpoint = oasp_arena_checkpoint_save(runtime->scratch);
    oasp_value result = oasp_invoke_routine_inner(runtime, routine, object, arguments, NULL, NULL, NULL,
                                                  argument_count);
    runtime->frame = caller;
    oasp_arena_checkpoint_restore(runtime->scratch, checkpoint);
    return result;
}

static oasp_value oasp_object_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line) {
    oasp_resolve_persisted_class(runtime, object);
    oasp_value component_result;
    if (oasp_component_get(runtime, object, member, line, &component_result))
        return component_result;
    oasp_binding *field = oasp_binding_find(object->fields, member);
    if (field)
        return field->value;
    oasp_routine_ref method = oasp_find_accessor(object, member, OASP_ROUTINE_PROPERTY_GET, OASP_ROUTINE_FUNCTION);
    if (method.bytecode)
        return oasp_invoke_routine(runtime, method, object, NULL, 0);
    oasp_runtime_error(runtime, 438, "unknown object member", line);
    return oasp_value_empty();
}

static bool oasp_resolve_object_path(oasp_runtime *runtime, oasp_slice path, size_t line, oasp_object **object, oasp_slice *member) {
    oasp_slice base;
    oasp_slice remaining;
    if (!oasp_split_member(path, &base, &remaining))
        return false;
    const oasp_value *root = oasp_runtime_get(runtime, base);
    oasp_value invoked = oasp_value_empty();
    if (!root) {
        oasp_routine_def *routine = oasp_find_routine(runtime, base);
        oasp_routine_bytecode metadata;
        if (routine && oasp_bytecode_routine_at(routine->bytecode, routine->routine_index, &metadata) &&
            metadata.kind != OASP_ROUTINE_SUB) {
            invoked = oasp_invoke_routine(runtime, (oasp_routine_ref){routine->bytecode, routine->routine_index}, NULL, NULL, 0);
            root = &invoked;
        }
    }
    if (!root || root->kind != OASP_VALUE_OBJECT)
        return false;
    oasp_object *current = root->as.object;
    oasp_slice segment;
    oasp_slice tail;
    while (oasp_split_member(remaining, &segment, &tail)) {
        oasp_value child = oasp_object_get(runtime, current, segment, line);
        if (child.kind != OASP_VALUE_OBJECT) {
            oasp_runtime_error(runtime, 424, "object required", line);
            return false;
        }
        current = child.as.object;
        remaining = tail;
    }
    *object = current;
    *member = remaining;
    return true;
}

static oasp_value oasp_utf8_character(oasp_runtime *runtime, uint32_t codepoint) {
    char bytes[4];
    size_t length;
    if (codepoint <= 0x7f) {
        bytes[0] = (char)codepoint;
        length = 1;
    } else if (codepoint <= 0x7ff) {
        bytes[0] = (char)(0xc0 | (codepoint >> 6));
        bytes[1] = (char)(0x80 | (codepoint & 0x3f));
        length = 2;
    } else {
        bytes[0] = (char)(0xe0 | (codepoint >> 12));
        bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (codepoint & 0x3f));
        length = 3;
    }
    return oasp_runtime_string(runtime, bytes, length);
}

static oasp_value oasp_server_map_path(oasp_runtime *runtime, const oasp_value *argument) {
    char *virtual_path = oasp_value_format(runtime->format_arena, argument);
    if (!virtual_path)
        return oasp_value_empty();
    const char *root = oasp_request_root(runtime->request);
    const char *page = oasp_request_path(runtime->request);
    oasp_buffer path;
    if (!oasp_buffer_init(&path, strlen(root) + strlen(virtual_path) + 32))
        return oasp_value_empty();

    bool rooted = virtual_path[0] == '/' || virtual_path[0] == '\\';
    if (rooted) {
        (void)oasp_buffer_append(&path, root, strlen(root));
        while (*virtual_path == '/' || *virtual_path == '\\')
            virtual_path++;
    } else {
        const char *slash = strrchr(page, '/');
        const char *backslash = strrchr(page, '\\');
        if (!slash || (backslash && backslash > slash))
            slash = backslash;
        if (slash)
            (void)oasp_buffer_append(&path, page, (size_t)(slash - page));
        else
            (void)oasp_buffer_append(&path, root, strlen(root));
    }
    if (path.length && path.data[path.length - 1] != '/' && path.data[path.length - 1] != '\\')
        (void)oasp_buffer_append_byte(&path, '/');
    (void)oasp_buffer_append(&path, virtual_path, strlen(virtual_path));
    for (size_t i = 0; i < path.length; i++) {
#if defined(_WIN32)
        if (path.data[i] == '/')
            path.data[i] = '\\';
#else
        if (path.data[i] == '\\')
            path.data[i] = '/';
#endif
    }
    oasp_value result = oasp_runtime_string(runtime, path.data, path.length);
    oasp_buffer_dispose(&path);
    return result;
}

static oasp_value oasp_runtime_date_value(oasp_runtime *runtime, time_t timestamp) {
    struct tm *parts = localtime(&timestamp);
    if (!parts)
        return oasp_value_empty();
    char text[32];
    int length = snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", parts->tm_year + 1900, parts->tm_mon + 1, parts->tm_mday, parts->tm_hour,
                          parts->tm_min, parts->tm_sec);
    oasp_value result = oasp_value_empty();
    result.kind = OASP_VALUE_DATE;
    result.as.date.data = oasp_runtime_copy(runtime, text, (size_t)length);
    result.as.date.length = (size_t)length;
    return result;
}

static oasp_value oasp_runtime_date_parts(oasp_runtime *runtime, int year, int month, int day, int hour,
                                          int minute, int second) {
    char text[32];
    int length = snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    if (length < 0 || (size_t)length >= sizeof(text))
        return oasp_value_empty();
    oasp_value result = oasp_value_empty();
    result.kind = OASP_VALUE_DATE;
    result.as.date.data = oasp_runtime_copy(runtime, text, (size_t)length);
    result.as.date.length = (size_t)length;
    return result;
}

static int oasp_hex_value(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static size_t oasp_utf8_decode_one(const unsigned char *text, uint32_t *codepoint) {
    if (text[0] < 0x80) {
        *codepoint = text[0];
        return 1;
    }
    if ((text[0] & 0xe0) == 0xc0 && (text[1] & 0xc0) == 0x80) {
        *codepoint = ((uint32_t)(text[0] & 0x1f) << 6) | (uint32_t)(text[1] & 0x3f);
        return *codepoint >= 0x80 ? 2 : 0;
    }
    if ((text[0] & 0xf0) == 0xe0 && (text[1] & 0xc0) == 0x80 && (text[2] & 0xc0) == 0x80) {
        *codepoint = ((uint32_t)(text[0] & 0x0f) << 12) | ((uint32_t)(text[1] & 0x3f) << 6) | (uint32_t)(text[2] & 0x3f);
        return *codepoint >= 0x800 ? 3 : 0;
    }
    if ((text[0] & 0xf8) == 0xf0 && (text[1] & 0xc0) == 0x80 && (text[2] & 0xc0) == 0x80 && (text[3] & 0xc0) == 0x80) {
        *codepoint = ((uint32_t)(text[0] & 0x07) << 18) | ((uint32_t)(text[1] & 0x3f) << 12) | ((uint32_t)(text[2] & 0x3f) << 6) | (uint32_t)(text[3] & 0x3f);
        return *codepoint >= 0x10000 && *codepoint <= 0x10ffff ? 4 : 0;
    }
    return 0;
}

static size_t oasp_utf8_decode_range(const unsigned char *text, const unsigned char *end, uint32_t *codepoint) {
    size_t remaining = (size_t)(end - text);
    if (!remaining)
        return 0;
    size_t required = text[0] < 0x80 ? 1 : (text[0] & 0xe0) == 0xc0 ? 2 : (text[0] & 0xf0) == 0xe0 ? 3 : (text[0] & 0xf8) == 0xf0 ? 4 : 1;
    if (required > remaining) {
        *codepoint = text[0];
        return 1;
    }
    size_t width = oasp_utf8_decode_one(text, codepoint);
    if (width)
        return width;
    *codepoint = text[0];
    return 1;
}

static size_t oasp_utf8_character_count(const char *text, size_t length) {
    if (!text)
        return 0;
    const unsigned char *cursor = (const unsigned char *)text;
    const unsigned char *end = cursor + length;
    size_t count = 0;
    while (cursor < end) {
        uint32_t codepoint;
        cursor += oasp_utf8_decode_range(cursor, end, &codepoint);
        count++;
    }
    return count;
}

static size_t oasp_utf8_byte_offset(const char *text, size_t length, size_t character_offset) {
    const unsigned char *start = (const unsigned char *)text;
    const unsigned char *cursor = start;
    const unsigned char *end = start + length;
    while (cursor < end && character_offset) {
        uint32_t codepoint;
        cursor += oasp_utf8_decode_range(cursor, end, &codepoint);
        character_offset--;
    }
    return (size_t)(cursor - start);
}

typedef struct {
    uint32_t codepoint;
    unsigned char replacement[4];
    unsigned char replacement_length;
    bool occupied;
} oasp_character_mapping;

static oasp_value oasp_map_characters(oasp_runtime *runtime, const char *input, const char *replacement, const char *characters) {
    const unsigned char *characters_end = (const unsigned char *)characters + strlen(characters);
    size_t character_count = 0;
    for (const unsigned char *cursor = (const unsigned char *)characters; cursor < characters_end; character_count++) {
        uint32_t codepoint;
        cursor += oasp_utf8_decode_range(cursor, characters_end, &codepoint);
    }

    if (character_count > SIZE_MAX / 2)
        return oasp_value_empty();
    size_t capacity = 8;
    while (capacity < character_count * 2) {
        if (capacity > SIZE_MAX / 2)
            return oasp_value_empty();
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(oasp_character_mapping))
        return oasp_value_empty();
    oasp_character_mapping *map = oasp_runtime_alloc(runtime, capacity * sizeof(*map), _Alignof(oasp_character_mapping));
    if (!map)
        return oasp_value_empty();
    memset(map, 0, capacity * sizeof(*map));

    const unsigned char *source = (const unsigned char *)characters;
    const unsigned char *destination = (const unsigned char *)replacement;
    const unsigned char *destination_end = destination + strlen(replacement);
    while (source < characters_end) {
        uint32_t source_codepoint;
        size_t source_width = oasp_utf8_decode_range(source, characters_end, &source_codepoint);
        size_t replacement_width = 0;
        const unsigned char *replacement_start = destination;
        if (destination < destination_end) {
            uint32_t ignored;
            replacement_width = oasp_utf8_decode_range(destination, destination_end, &ignored);
            destination += replacement_width;
        }

        size_t slot = ((size_t)source_codepoint * UINT32_C(2654435761)) & (capacity - 1);
        while (map[slot].occupied && map[slot].codepoint != source_codepoint)
            slot = (slot + 1) & (capacity - 1);
        if (!map[slot].occupied) {
            map[slot].occupied = true;
            map[slot].codepoint = source_codepoint;
            map[slot].replacement_length = (unsigned char)replacement_width;
            if (replacement_width)
                memcpy(map[slot].replacement, replacement_start, replacement_width);
        }
        source += source_width;
    }

    oasp_buffer output;
    if (!oasp_buffer_init(&output, strlen(input) + 1))
        return oasp_value_empty();
    const unsigned char *input_cursor = (const unsigned char *)input;
    const unsigned char *input_end = input_cursor + strlen(input);
    while (input_cursor < input_end) {
        uint32_t codepoint;
        size_t width = oasp_utf8_decode_range(input_cursor, input_end, &codepoint);
        size_t slot = ((size_t)codepoint * UINT32_C(2654435761)) & (capacity - 1);
        while (map[slot].occupied && map[slot].codepoint != codepoint)
            slot = (slot + 1) & (capacity - 1);
        const void *bytes = input_cursor;
        size_t byte_count = width;
        if (map[slot].occupied) {
            bytes = map[slot].replacement;
            byte_count = map[slot].replacement_length;
        }
        if (!oasp_buffer_append(&output, bytes, byte_count)) {
            oasp_buffer_dispose(&output);
            return oasp_value_empty();
        }
        input_cursor += width;
    }
    oasp_value result = oasp_runtime_string(runtime, output.data, output.length);
    oasp_buffer_dispose(&output);
    return result;
}

static bool oasp_append_escape_unit(oasp_buffer *output, uint32_t unit) {
    static const char digits[] = "0123456789ABCDEF";
    char encoded[6];
    if (unit <= 0xff) {
        encoded[0] = '%';
        encoded[1] = digits[(unit >> 4) & 0x0f];
        encoded[2] = digits[unit & 0x0f];
        return oasp_buffer_append(output, encoded, 3);
    }
    encoded[0] = '%';
    encoded[1] = 'u';
    encoded[2] = digits[(unit >> 12) & 0x0f];
    encoded[3] = digits[(unit >> 8) & 0x0f];
    encoded[4] = digits[(unit >> 4) & 0x0f];
    encoded[5] = digits[unit & 0x0f];
    return oasp_buffer_append(output, encoded, sizeof(encoded));
}

static oasp_value oasp_vb_escape(oasp_runtime *runtime, const char *text) {
    oasp_buffer output;
    if (!oasp_buffer_init(&output, strlen(text) * 2 + 1))
        return oasp_value_empty();
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor) {
        uint32_t codepoint;
        size_t width = oasp_utf8_decode_one(cursor, &codepoint);
        if (!width) {
            codepoint = *cursor;
            width = 1;
        }
        bool literal = codepoint < 0x80 && (isalnum((unsigned char)codepoint) || strchr("@*_+-./", (int)codepoint));
        bool ok = literal               ? oasp_buffer_append_byte(&output, (char)codepoint)
                  : codepoint <= 0xffff ? oasp_append_escape_unit(&output, codepoint)
                                        : oasp_append_escape_unit(&output, 0xd800 + ((codepoint - 0x10000) >> 10)) &&
                                              oasp_append_escape_unit(&output, 0xdc00 + ((codepoint - 0x10000) & 0x3ff));
        if (!ok) {
            oasp_buffer_dispose(&output);
            return oasp_value_empty();
        }
        cursor += width;
    }
    oasp_value result = oasp_runtime_string(runtime, output.data, output.length);
    oasp_buffer_dispose(&output);
    return result;
}

static oasp_value oasp_vbs_unescape(oasp_runtime *runtime, const char *text) {
    oasp_buffer output;
    if (!oasp_buffer_init(&output, strlen(text) + 1))
        return oasp_value_empty();
    for (size_t i = 0; text[i]; i++) {
        if (text[i] == '%' && text[i + 1] == 'u') {
            int a = oasp_hex_value(text[i + 2]);
            int b = oasp_hex_value(text[i + 3]);
            int c = oasp_hex_value(text[i + 4]);
            int d = oasp_hex_value(text[i + 5]);
            if (a >= 0 && b >= 0 && c >= 0 && d >= 0) {
                uint32_t codepoint = (uint32_t)((a << 12) | (b << 8) | (c << 4) | d);
                oasp_value character = oasp_utf8_character(runtime, codepoint);
                (void)oasp_buffer_append(&output, character.as.string.data, character.as.string.length);
                i += 5;
                continue;
            }
        }
        if (text[i] == '%' && text[i + 1] && text[i + 2]) {
            int high = oasp_hex_value(text[i + 1]);
            int low = oasp_hex_value(text[i + 2]);
            if (high >= 0 && low >= 0) {
                (void)oasp_buffer_append_byte(&output, (char)((high << 4) | low));
                i += 2;
                continue;
            }
        }
        (void)oasp_buffer_append_byte(&output, text[i]);
    }
    oasp_value result = oasp_runtime_string(runtime, output.data, output.length);
    oasp_buffer_dispose(&output);
    return result;
}

static oasp_value oasp_request_collection_keys(oasp_runtime *runtime, const char *parameters) {
    size_t count = 0;
    for (const char *cursor = parameters; cursor && *cursor;) {
        count++;
        const char *separator = strchr(cursor, '&');
        cursor = separator ? separator + 1 : cursor + strlen(cursor);
    }
    oasp_array *array = oasp_runtime_alloc_array(runtime);
    if (!array)
        return oasp_value_empty();
    memset(array, 0, sizeof(*array));
    array->rank = 1;
    array->length = count;
    array->bounds = oasp_runtime_alloc(runtime, sizeof(*array->bounds), _Alignof(int64_t));
    if (!array->bounds)
        return oasp_value_empty();
    array->bounds[0] = count ? (int64_t)count - 1 : -1;
    if (count) {
        array->items = oasp_runtime_alloc_values(runtime, count);
        if (!array->items)
            return oasp_value_empty();
    }
    const char *cursor = parameters;
    for (size_t i = 0; i < count; i++) {
        const char *separator = strchr(cursor, '&');
        const char *end = separator ? separator : cursor + strlen(cursor);
        const char *equals = memchr(cursor, '=', (size_t)(end - cursor));
        size_t length = (size_t)((equals ? equals : end) - cursor);
        array->items[i] = oasp_runtime_string(runtime, cursor, length);
        cursor = separator ? separator + 1 : end;
    }
    oasp_value result = oasp_value_empty();
    result.kind = OASP_VALUE_ARRAY;
    result.as.array = array;
    return result;
}

/*
 * Built-in dispatch: implements intrinsic VBScript functions and ASP
 * Request/Response/Server/Session/Application operations.
 */
static size_t oasp_runtime_formatted_length(const oasp_value *value, const char *text) {
    if (!text || !value)
        return 0;
    if (value->kind == OASP_VALUE_STRING)
        return value->as.string.length;
    if (value->kind == OASP_VALUE_DATE)
        return value->as.date.length;
    return strlen(text);
}

static oasp_value oasp_builtin_call(oasp_runtime *runtime, oasp_builtin_id builtin_id, oasp_slice name, oasp_value *args, size_t count,
                                    size_t line) {
    switch (builtin_id) {
    case OASP_BUILTIN_RESPONSE_WRITE:
        goto builtin_response_write;
    case OASP_BUILTIN_CSTR:
        goto builtin_cstr;
    case OASP_BUILTIN_LEN:
        goto builtin_len;
    case OASP_BUILTIN_ISEMPTY:
        goto builtin_isempty;
    case OASP_BUILTIN_SERVER_HTMLENCODE:
        goto builtin_htmlencode;
    case OASP_BUILTIN_SERVER_URLENCODE:
        goto builtin_urlencode;
    case OASP_BUILTIN_TRIM:
        goto builtin_trim;
    case OASP_BUILTIN_LCASE:
    case OASP_BUILTIN_UCASE:
        goto builtin_case;
    case OASP_BUILTIN_REPLACE:
        goto builtin_replace;
    case OASP_BUILTIN_DYNAMIC:
    case OASP_BUILTIN_COUNT:
        break;
    }
    if (oasp_ascii_equal(name, "err.clear")) {
        oasp_runtime_clear_error(runtime);
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "err.raise")) {
        int64_t code = 5;
        const char *source = "OpenASP";
        const char *description = "runtime error";
        if (count)
            (void)oasp_value_to_int(&args[0], &code);
        if (count > 1)
            source = oasp_value_format(runtime->format_arena, &args[1]);
        if (count > 2)
            description = oasp_value_format(runtime->format_arena, &args[2]);
        oasp_runtime_error(runtime, (int)code, description, line);
        runtime->error_source = source ? oasp_runtime_copy(runtime, source, strlen(source)) : NULL;
        if (source && !runtime->error_source)
            runtime->error_source = "OpenASP";
        return oasp_value_empty();
    }
builtin_response_write:
    if (builtin_id == OASP_BUILTIN_RESPONSE_WRITE || oasp_ascii_equal(name, "response.write")) {
        for (size_t i = 0; i < count; i++)
            (void)oasp_runtime_write_value(runtime, &args[i], line);
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "response.binarywrite")) {
        if (count) {
            char *bytes = oasp_value_format(runtime->format_arena, &args[0]);
            size_t length = args[0].kind == OASP_VALUE_STRING ? args[0].as.string.length : bytes ? strlen(bytes) : 0;
            if (bytes) {
                oasp_request_binary_write(runtime->request, bytes, length);
                if (oasp_request_failed(runtime->request))
                    oasp_runtime_error(runtime, 5, oasp_request_error(runtime->request), line);
            }
        }
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "response.clear")) {
        oasp_request_truncate_output(runtime->request, 0);
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "response.flush")) {
        if (oasp_request_flush(runtime->request))
            oasp_runtime_error(runtime, 5, oasp_request_error(runtime->request), line);
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "response.end")) {
        if (oasp_process_options.trace_errors)
            fprintf(stderr, "OpenASP trace: Response.End at line %zu\n", line);
        runtime->stopped = true;
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "response.addheader")) {
        if (count < 2) {
            oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
            return oasp_value_empty();
        }
        char *header_name = oasp_value_format(runtime->format_arena, &args[0]);
        char *header_value = oasp_value_format(runtime->format_arena, &args[1]);
        if (!header_name || !header_value || oasp_request_add_header(runtime->request, header_name, header_value))
            oasp_runtime_error(runtime, 5, oasp_request_failed(runtime->request)
                                               ? oasp_request_error(runtime->request)
                                               : "invalid response header", line);
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "response.redirect")) {
        if (!count) {
            oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
            return oasp_value_empty();
        }
        char *location = oasp_value_format(runtime->format_arena, &args[0]);
        int status_result = location ? oasp_request_set_status(runtime->request, "302 Found") : -1;
        int header_result = status_result == 0 ? oasp_request_add_header(runtime->request, "Location", location) : -1;
        if (!location || status_result || header_result) {
            oasp_runtime_error(runtime, 5, "invalid redirect location", line);
            return oasp_value_empty();
        }
        if (oasp_process_options.trace_errors)
            fprintf(stderr, "OpenASP trace: Response.Redirect at line %zu\n", line);
        runtime->stopped = true;
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "server.transfer")) {
        if (!count) {
            oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
            return oasp_value_empty();
        }
        char *path = oasp_value_format(runtime->format_arena, &args[0]);
        if (!path) {
            oasp_runtime_error(runtime, 5, "invalid transfer path", line);
            return oasp_value_empty();
        }
        size_t path_length = strlen(path);
        char *normalized = oasp_runtime_alloc(runtime, path_length + 1, _Alignof(char));
        if (!normalized) {
            oasp_runtime_error(runtime, 7, "out of memory", line);
            return oasp_value_empty();
        }
        for (size_t i = 0; i < path_length; i++)
            normalized[i] = path[i] == '\\' ? '/' : path[i];
        normalized[path_length] = '\0';
        bool transferred =
            runtime->transfer_handler
                ? runtime->transfer_handler(runtime->include_context, runtime,
                                            (oasp_slice){normalized, path_length})
                : oasp_runtime_execute_include(runtime, (oasp_slice){normalized, path_length});
        if (!transferred && !runtime->error_pending)
            oasp_runtime_error(runtime, 5, "Server.Transfer failed", line);
        runtime->stopped = true;
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "application.lock") || oasp_ascii_equal(name, "application.unlock"))
        return oasp_value_empty();
    if (oasp_ascii_equal(name, "__openaspmapcharacters")) {
        if (count < 3)
            return oasp_value_empty();
        char *input = oasp_value_format(runtime->format_arena, &args[0]);
        char *replacement = oasp_value_format(runtime->format_arena, &args[1]);
        char *characters = oasp_value_format(runtime->format_arena, &args[2]);
        if (!input || !replacement || !characters)
            return oasp_value_empty();
        return oasp_map_characters(runtime, input, replacement, characters);
    }
    if (oasp_ascii_equal(name, "session.abandon")) {
        runtime->session = NULL;
        runtime->session_abandoned = true;
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "randomize")) {
        unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)runtime;
        if (count) {
            int64_t requested = 0;
            if (oasp_value_to_int(&args[0], &requested))
                seed = (unsigned int)requested;
        }
        srand(seed);
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "rnd"))
        return oasp_value_float((double)rand() / ((double)RAND_MAX + 1.0));
    if (oasp_ascii_equal(name, "session.contents.remove") || oasp_ascii_equal(name, "application.contents.remove")) {
        if (count) {
            char *key = oasp_value_format(runtime->format_arena, &args[0]);
            oasp_binding **store = oasp_ascii_equal(name, "session.contents.remove") ? &runtime->session : &runtime->application;
            if (key)
                oasp_store_remove(store, key);
        }
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "request") || oasp_ascii_equal(name, "request.querystring")) {
        if (!count)
            return oasp_request_collection_keys(runtime, oasp_request_query_string(runtime->request));
        char *key = oasp_value_format(runtime->format_arena, &args[0]);
        const char *lookup = key ? key : "";
        bool found = oasp_request_query_has(runtime->request, lookup);
        const char *text = found ? oasp_request_query(runtime->request, lookup) : "";
        if (!found && oasp_ascii_equal(name, "request")) {
            found = oasp_request_form_has(runtime->request, lookup);
            if (found)
                text = oasp_request_form(runtime->request, lookup);
        }
        if (!found)
            return oasp_value_empty();
        return oasp_runtime_string(runtime, text, strlen(text));
    }
    if (oasp_ascii_equal(name, "request.form")) {
        if (!count)
            return oasp_request_collection_keys(runtime, oasp_request_body(runtime->request));
        char *key = oasp_value_format(runtime->format_arena, &args[0]);
        const char *lookup = key ? key : "";
        const char *text = oasp_request_form(runtime->request, lookup);
        bool found = oasp_request_form_has(runtime->request, lookup);
        if (!found)
            return oasp_value_empty();
        return oasp_runtime_string(runtime, text, strlen(text));
    }
    if (oasp_ascii_equal(name, "request.binaryread")) {
        int64_t requested = 0;
        if (!count || !oasp_value_to_int(&args[0], &requested) || requested < 0) {
            oasp_runtime_error(runtime, 5, "invalid procedure call", line);
            return oasp_value_empty();
        }
        const char *body = oasp_request_body(runtime->request);
        size_t body_length = oasp_request_body_length(runtime->request);
        size_t available = runtime->binary_read_offset < body_length ? body_length - runtime->binary_read_offset : 0;
        size_t length = (uint64_t)requested < available ? (size_t)requested : available;
        oasp_value result = oasp_runtime_string(runtime, body + runtime->binary_read_offset, length);
        runtime->binary_read_offset += length;
        return result;
    }
    if (oasp_ascii_equal(name, "request.cookies")) {
        if (!count)
            return oasp_value_string("", 0);
        char *key = oasp_value_format(runtime->format_arena, &args[0]);
        const char *value = oasp_request_cookie_value(runtime->request, key ? key : "");
        return oasp_runtime_string(runtime, value, strlen(value));
    }
    if (oasp_ascii_equal(name, "request.cookies.subkey")) {
        if (count != 2) {
            oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
            return oasp_value_empty();
        }
        char *key = oasp_value_format(runtime->format_arena, &args[0]);
        char *subkey = oasp_value_format(runtime->format_arena, &args[1]);
        const char *value = oasp_request_cookie_subvalue(runtime->request, key ? key : "", subkey ? subkey : "");
        return oasp_runtime_string(runtime, value, strlen(value));
    }
    if (oasp_ascii_equal(name, "request.servervariables")) {
        if (!count)
            return oasp_value_string("", 0);
        char *key = oasp_value_format(runtime->format_arena, &args[0]);
        const char *value = NULL;
        if (key && oasp_ascii_equal_n(key, strlen(key), "path_translated"))
            value = oasp_request_path(runtime->request);
        else if (key && oasp_ascii_equal_n(key, strlen(key), "gateway_interface"))
            value = "CGI/1.1";
        else if (key && oasp_ascii_equal_n(key, strlen(key), "server_name"))
            value = oasp_request_server_variable(runtime->request, "SERVER_NAME");
        else if (key && oasp_ascii_equal_n(key, strlen(key), "server_port"))
            value = oasp_request_server_variable(runtime->request, "SERVER_PORT");
        else if (key && oasp_ascii_equal_n(key, strlen(key), "document_root"))
            value = oasp_request_root(runtime->request);
        else if (key && oasp_ascii_equal_n(key, strlen(key), "all_http"))
            value = "";
        else if (key && oasp_ascii_equal_n(key, strlen(key), "http_host"))
            value = oasp_request_server_variable(runtime->request, "HTTP_HOST");
        else if (key && oasp_ascii_equal_n(key, strlen(key), "https"))
            value = oasp_request_server_variable(runtime->request, "HTTPS");
        else if (key && oasp_ascii_equal_n(key, strlen(key), "request_method"))
            value = oasp_request_method(runtime->request);
        else if (key && oasp_ascii_equal_n(key, strlen(key), "query_string"))
            value = oasp_request_query_string(runtime->request);
        else if (key && (oasp_ascii_equal_n(key, strlen(key), "url") || oasp_ascii_equal_n(key, strlen(key), "script_name"))) {
            const char *path = oasp_request_path(runtime->request);
            const char *root = oasp_request_root(runtime->request);
            value = !strncmp(path, root, strlen(root)) ? path + strlen(root) : path;
        } else if (key)
            value = oasp_request_server_variable(runtime->request, key);
        if (!value && key && oasp_ascii_equal_n(key, strlen(key), "server_name"))
            value = "localhost";
        else if (!value && key && oasp_ascii_equal_n(key, strlen(key), "server_port"))
            value = "80";
        else if (!value && key && oasp_ascii_equal_n(key, strlen(key), "http_host"))
            value = "127.0.0.1:18088";
        return value ? oasp_runtime_string(runtime, value, strlen(value)) : oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "session") || oasp_ascii_equal(name, "application")) {
        if (!count)
            return oasp_value_empty();
        char *key = oasp_value_format(runtime->format_arena, &args[0]);
        oasp_binding *store = oasp_ascii_equal(name, "session") ? runtime->session : runtime->application;
        const oasp_value *value = oasp_store_get(store, key ? key : "");
        return value ? *value : oasp_value_empty();
    }
builtin_cstr:
    if (builtin_id == OASP_BUILTIN_CSTR || oasp_ascii_equal(name, "cstr")) {
        if (!count)
            return oasp_value_string("", 0);
        if (args[0].kind == OASP_VALUE_STRING)
            return args[0];
        char buffer[64];
        oasp_slice text;
        if (!oasp_value_text(&args[0], buffer, sizeof(buffer), &text))
            return oasp_value_empty();
        return oasp_runtime_string(runtime, text.data, text.length);
    }
builtin_isempty:
    if (builtin_id == OASP_BUILTIN_ISEMPTY || oasp_ascii_equal(name, "isempty"))
        return oasp_value_bool(count && args[0].kind == OASP_VALUE_EMPTY);
    if (oasp_ascii_equal(name, "isnull"))
        return oasp_value_bool(count && args[0].kind == OASP_VALUE_NULL);
    if (oasp_ascii_equal(name, "isarray"))
        return oasp_value_bool(count && args[0].kind == OASP_VALUE_ARRAY);
    if (oasp_ascii_equal(name, "isobject"))
        return oasp_value_bool(count && args[0].kind == OASP_VALUE_OBJECT);
    if (oasp_ascii_equal(name, "vartype")) {
        if (!count) {
            oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
            return oasp_value_empty();
        }
        int type = args[0].kind == OASP_VALUE_EMPTY    ? 0
                   : args[0].kind == OASP_VALUE_NULL   ? 1
                   : args[0].kind == OASP_VALUE_INT    ? (args[0].as.integer >= INT16_MIN && args[0].as.integer <= INT16_MAX ? 2 : 3)
                   : args[0].kind == OASP_VALUE_FLOAT  ? 5
                   : args[0].kind == OASP_VALUE_DATE   ? 7
                   : args[0].kind == OASP_VALUE_STRING ? 8
                   : args[0].kind == OASP_VALUE_OBJECT ? 9
                   : args[0].kind == OASP_VALUE_BOOL   ? 11
                   : args[0].kind == OASP_VALUE_ARRAY  ? 0x200c
                                                       : 0;
        return oasp_value_int(type);
    }
    if (oasp_ascii_equal(name, "isnumeric")) {
        if (!count || args[0].kind == OASP_VALUE_NULL || args[0].kind == OASP_VALUE_OBJECT || args[0].kind == OASP_VALUE_ARRAY)
            return oasp_value_bool(false);
        if (oasp_value_numeric(&args[0]))
            return oasp_value_bool(true);
        char *text = oasp_value_format(runtime->format_arena, &args[0]);
        char *end = NULL;
        if (!text || !*text)
            return oasp_value_bool(false);
        (void)strtod(text, &end);
        return oasp_value_bool(end && *end == '\0');
    }
    if (oasp_ascii_equal(name, "isdate")) {
        if (!count)
            return oasp_value_bool(false);
        if (args[0].kind == OASP_VALUE_DATE)
            return oasp_value_bool(true);
        if (args[0].kind != OASP_VALUE_STRING || !args[0].as.string.length)
            return oasp_value_bool(false);
        char *text = oasp_value_format(runtime->format_arena, &args[0]);
        struct tm parts;
        return oasp_value_bool(oasp_parse_date_text(text, &parts));
    }
    if (oasp_ascii_equal(name, "createobject") || oasp_ascii_equal(name, "server.createobject")) {
        if (!count || args[0].kind != OASP_VALUE_STRING) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        return oasp_component_create(runtime, args[0].as.string, line);
    }
    if (oasp_ascii_equal(name, "server.mappath")) {
        if (!count)
            return oasp_value_string("", 0);
        return oasp_server_map_path(runtime, &args[0]);
    }
    if (oasp_ascii_equal(name, "cbool")) {
        if (count != 1) {
            oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
            return oasp_value_empty();
        }
        if (args[0].kind == OASP_VALUE_NULL) {
            oasp_runtime_error(runtime, 94, "invalid use of Null", line);
            return oasp_value_empty();
        }
        if (args[0].kind == OASP_VALUE_NOTHING) {
            oasp_runtime_error(runtime, 91, "object variable not set", line);
            return oasp_value_empty();
        }
        if (args[0].kind == OASP_VALUE_OBJECT || args[0].kind == OASP_VALUE_ARRAY) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        if (args[0].kind == OASP_VALUE_BOOL)
            return args[0];
        if (args[0].kind == OASP_VALUE_EMPTY)
            return oasp_value_bool(false);
        if (args[0].kind == OASP_VALUE_DATE) {
            char *text = oasp_value_format(runtime->format_arena, &args[0]);
            return oasp_value_bool(!text || strncmp(text, "1899-12-30", 10) != 0);
        }
        double number = 0.0;
        if (args[0].kind != OASP_VALUE_STRING && oasp_value_to_float(&args[0], &number))
            return oasp_value_bool(number != 0.0);
        if (args[0].kind == OASP_VALUE_STRING) {
            const char *start = args[0].as.string.data;
            const char *end = start + args[0].as.string.length;
            while (start < end && isspace((unsigned char)*start))
                start++;
            while (end > start && isspace((unsigned char)end[-1]))
                end--;
            oasp_slice text = {start, (size_t)(end - start)};
            if (oasp_ascii_equal(text, "true"))
                return oasp_value_bool(true);
            if (oasp_ascii_equal(text, "false"))
                return oasp_value_bool(false);
            if (!text.length || memchr(text.data, '\0', text.length)) {
                oasp_runtime_error(runtime, 13, "type mismatch", line);
                return oasp_value_empty();
            }
            char *copy = oasp_runtime_copy(runtime, text.data, text.length);
            char *parse_end = NULL;
            errno = 0;
            if (text.length > 2 && text.data[0] == '&' && (text.data[1] == 'h' || text.data[1] == 'H')) {
                long long integer = strtoll(copy + 2, &parse_end, 16);
                if (!errno && parse_end == copy + text.length)
                    return oasp_value_bool(integer != 0);
            } else if (text.length > 2 && text.data[0] == '&' && (text.data[1] == 'o' || text.data[1] == 'O')) {
                long long integer = strtoll(copy + 2, &parse_end, 8);
                if (!errno && parse_end == copy + text.length)
                    return oasp_value_bool(integer != 0);
            } else {
                double floating = 0.0;
                if (text.length > 1 && text.data[0] == '0' && (text.data[1] == 'x' || text.data[1] == 'X'))
                    parse_end = copy;
                else
                    floating = strtod(copy, &parse_end);
                if (errno == ERANGE) {
                    oasp_runtime_error(runtime, 6, "overflow", line);
                    return oasp_value_empty();
                }
                if (parse_end == copy + text.length && isfinite(floating))
                    return oasp_value_bool(floating != 0.0);
            }
        }
        oasp_runtime_error(runtime, 13, "type mismatch", line);
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "cdbl")) {
        if (count != 1) {
            oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
            return oasp_value_empty();
        }
        if (args[0].kind == OASP_VALUE_NULL) {
            oasp_runtime_error(runtime, 94, "invalid use of Null", line);
            return oasp_value_empty();
        }
        double floating = 0.0;
        bool converted = oasp_value_to_float(&args[0], &floating);
        if (!converted && args[0].kind == OASP_VALUE_STRING &&
            !memchr(args[0].as.string.data, '\0', args[0].as.string.length)) {
            char *copy = oasp_runtime_copy(runtime, args[0].as.string.data, args[0].as.string.length);
            char *end = NULL;
            errno = 0;
            floating = copy ? strtod(copy, &end) : 0.0;
            converted = copy && end != copy && end == copy + args[0].as.string.length &&
                        errno != ERANGE && isfinite(floating);
        }
        if (!converted) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        return oasp_value_float(floating);
    }
    if (oasp_ascii_equal(name, "clng") || oasp_ascii_equal(name, "cint")) {
        int64_t integer = 0;
        bool converted = count && oasp_value_to_int(&args[0], &integer);
        if (!converted && count && args[0].kind == OASP_VALUE_FLOAT &&
            (!isfinite(args[0].as.floating) || args[0].as.floating < -0x1p63 ||
             args[0].as.floating >= 0x1p63)) {
            oasp_runtime_error(runtime, 6, "overflow", line);
            return oasp_value_empty();
        }
        if (!converted && count && args[0].kind == OASP_VALUE_STRING) {
            char *copy = oasp_runtime_copy(runtime, args[0].as.string.data, args[0].as.string.length);
            char *end = NULL;
            errno = 0;
            double floating = copy ? strtod(copy, &end) : 0.0;
            long double rounded = roundl((long double)floating);
            if (copy && !errno && end == copy + args[0].as.string.length && isfinite(floating) &&
                rounded >= (long double)INT64_MIN && rounded <= (long double)INT64_MAX) {
                integer = (int64_t)rounded;
                converted = true;
            }
        }
        if (!converted) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        return oasp_value_int(integer);
    }
    if (oasp_ascii_equal(name, "formatnumber")) {
        double number = 0.0;
        int64_t decimals = 2;
        if (!count || !oasp_value_to_float(&args[0], &number) || (count > 1 && !oasp_value_to_int(&args[1], &decimals))) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        if (decimals < 0)
            decimals = 2;
        if (decimals > 15)
            decimals = 15;
        char text[128];
        int length = snprintf(text, sizeof(text), "%.*f", (int)decimals, number);
        return length >= 0 && (size_t)length < sizeof(text) ? oasp_runtime_string(runtime, text, (size_t)length) : oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "space")) {
        int64_t count_value = 0;
        if (!count || !oasp_value_to_int(&args[0], &count_value) || count_value < 0) {
            oasp_runtime_error(runtime, 5, "invalid procedure call", line);
            return oasp_value_empty();
        }
        char *text = oasp_runtime_alloc(runtime, (size_t)count_value + 1, _Alignof(char));
        if (!text)
            return oasp_value_empty();
        memset(text, ' ', (size_t)count_value);
        text[count_value] = '\0';
        return oasp_value_string(text, (size_t)count_value);
    }
    if (oasp_ascii_equal(name, "string")) {
        int64_t count_value = 0;
        int64_t character = 0;
        if (count < 2 || !oasp_value_to_int(&args[0], &count_value) || count_value < 0) {
            oasp_runtime_error(runtime, 5, "invalid procedure call", line);
            return oasp_value_empty();
        }
        if (args[1].kind == OASP_VALUE_STRING && args[1].as.string.length)
            character = (unsigned char)args[1].as.string.data[0];
        else if (!oasp_value_to_int(&args[1], &character)) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        char *text = oasp_runtime_alloc(runtime, (size_t)count_value + 1, _Alignof(char));
        if (!text)
            return oasp_value_empty();
        memset(text, (unsigned char)character, (size_t)count_value);
        text[count_value] = '\0';
        return oasp_value_string(text, (size_t)count_value);
    }
    if (oasp_ascii_equal(name, "strcomp")) {
        if (count < 2) {
            oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
            return oasp_value_empty();
        }
        char *left = oasp_value_format(runtime->format_arena, &args[0]);
        char *right = oasp_value_format(runtime->format_arena, &args[1]);
        int64_t compare = 0;
        if (count > 2)
            (void)oasp_value_to_int(&args[2], &compare);
        if (!left || !right)
            return oasp_value_empty();
        int result = compare ? strcasecmp(left, right) : strcmp(left, right);
        return oasp_value_int(result < 0 ? -1 : result > 0 ? 1 : 0);
    }
builtin_len:
    if (builtin_id == OASP_BUILTIN_LEN || oasp_ascii_equal(name, "len") || oasp_ascii_equal(name, "lenb")) {
        if (!count || args[0].kind == OASP_VALUE_NULL) {
            oasp_runtime_error(runtime, 94, "invalid use of Null", line);
            return oasp_value_empty();
        }
        char *text = oasp_value_format(runtime->format_arena, &args[0]);
        size_t length = oasp_runtime_formatted_length(&args[0], text);
        return oasp_value_int((int64_t)(oasp_ascii_equal(name, "lenb") ? length : oasp_utf8_character_count(text, length)));
    }
    if (oasp_ascii_equal(name, "left") || oasp_ascii_equal(name, "leftb") || oasp_ascii_equal(name, "right") || oasp_ascii_equal(name, "rightb") ||
        oasp_ascii_equal(name, "mid") || oasp_ascii_equal(name, "midb")) {
        if (count < 2) {
            oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
            return oasp_value_empty();
        }
        char *text = oasp_value_format(runtime->format_arena, &args[0]);
        int64_t position = 0;
        if (!text || !oasp_value_to_int(&args[1], &position)) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        size_t length = oasp_runtime_formatted_length(&args[0], text);
        bool byte_function = oasp_ascii_equal(name, "leftb") || oasp_ascii_equal(name, "rightb") || oasp_ascii_equal(name, "midb");
        bool left_function = oasp_ascii_equal(name, "left") || oasp_ascii_equal(name, "leftb");
        bool right_function = oasp_ascii_equal(name, "right") || oasp_ascii_equal(name, "rightb");
        size_t unit_length = byte_function ? length : oasp_utf8_character_count(text, length);
        if (left_function || right_function) {
            if (position < 0) {
                oasp_runtime_error(runtime, 5, "invalid procedure call", line);
                return oasp_value_empty();
            }
            size_t take = (uint64_t)position > unit_length ? unit_length : (size_t)position;
            size_t start_unit = right_function ? unit_length - take : 0;
            size_t start = byte_function ? start_unit : oasp_utf8_byte_offset(text, length, start_unit);
            size_t end = byte_function ? start + take : oasp_utf8_byte_offset(text, length, start_unit + take);
            return oasp_runtime_string(runtime, text + start, end - start);
        }
        if (position < 1) {
            oasp_runtime_error(runtime, 5, "invalid procedure call", line);
            return oasp_value_empty();
        }
        size_t start_unit = (uint64_t)position > unit_length ? unit_length : (size_t)position - 1;
        size_t take = unit_length - start_unit;
        int64_t requested = 0;
        if (count > 2 && oasp_value_to_int(&args[2], &requested)) {
            if (requested < 0) {
                oasp_runtime_error(runtime, 5, "invalid procedure call", line);
                return oasp_value_empty();
            }
            if ((uint64_t)requested < take)
                take = (size_t)requested;
        }
        size_t start = byte_function ? start_unit : oasp_utf8_byte_offset(text, length, start_unit);
        size_t end = byte_function ? start + take : oasp_utf8_byte_offset(text, length, start_unit + take);
        return oasp_runtime_string(runtime, text + start, end - start);
    }
builtin_case:
    if (builtin_id == OASP_BUILTIN_LCASE || builtin_id == OASP_BUILTIN_UCASE ||
        oasp_ascii_equal(name, "lcase") || oasp_ascii_equal(name, "ucase")) {
        char *text = count ? oasp_value_format(runtime->format_arena, &args[0]) : NULL;
        if (!text)
            return oasp_value_empty();
        size_t length = strlen(text);
        char *copy = oasp_runtime_copy(runtime, text, length);
        if (!copy)
            return oasp_value_empty();
        for (size_t i = 0; i < length; i++) {
            unsigned char ch = (unsigned char)copy[i];
            copy[i] = (char)((builtin_id == OASP_BUILTIN_LCASE || oasp_ascii_equal(name, "lcase")) ? tolower(ch) : toupper(ch));
        }
        return oasp_value_string(copy, length);
    }
    if (oasp_ascii_equal(name, "instr") || oasp_ascii_equal(name, "instrb")) {
        if (count < 2 || count > 4) {
            oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
            return oasp_value_empty();
        }
        size_t first = count >= 3 ? 1 : 0;
        int64_t start = 1;
        int64_t compare = 0;
        if (count >= 3)
            (void)oasp_value_to_int(&args[0], &start);
        if (count == 4)
            (void)oasp_value_to_int(&args[3], &compare);
        char *text = count > first ? oasp_value_format(runtime->format_arena, &args[first]) : NULL;
        char *needle = count > first + 1 ? oasp_value_format(runtime->format_arena, &args[first + 1]) : NULL;
        if (!text || !needle || start < 1)
            return oasp_value_int(0);
        size_t length = oasp_runtime_formatted_length(&args[first], text);
        size_t needle_length = oasp_runtime_formatted_length(&args[first + 1], needle);
        bool byte_function = oasp_ascii_equal(name, "instrb");
        size_t unit_length = byte_function ? length : oasp_utf8_character_count(text, length);
        const char *match = NULL;
        if (needle_length <= length && (uint64_t)(start - 1) <= unit_length) {
            size_t search_offset = byte_function ? (size_t)start - 1 : oasp_utf8_byte_offset(text, length, (size_t)start - 1);
            for (size_t offset = search_offset; offset <= length - needle_length; offset++) {
                bool equal = true;
                for (size_t index = 0; index < needle_length; index++) {
                    unsigned char left = (unsigned char)text[offset + index];
                    unsigned char right = (unsigned char)needle[index];
                    if (compare) {
                        left = (unsigned char)tolower(left);
                        right = (unsigned char)tolower(right);
                    }
                    if (left != right) {
                        equal = false;
                        break;
                    }
                }
                if (equal) {
                    match = text + offset;
                    break;
                }
            }
        }
        if (!match)
            return oasp_value_int(0);
        size_t match_offset = (size_t)(match - text);
        return oasp_value_int((int64_t)(byte_function ? match_offset + 1 : oasp_utf8_character_count(text, match_offset) + 1));
    }
    if (oasp_ascii_equal(name, "instrrev")) {
        if (count < 2) {
            oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
            return oasp_value_empty();
        }
        char *text = oasp_value_format(runtime->format_arena, &args[0]);
        char *needle = oasp_value_format(runtime->format_arena, &args[1]);
        int64_t start = -1;
        int64_t compare = 0;
        if (count > 2)
            (void)oasp_value_to_int(&args[2], &start);
        if (count > 3)
            (void)oasp_value_to_int(&args[3], &compare);
        if (!text || !needle)
            return oasp_value_int(0);
        size_t text_length = strlen(text);
        size_t needle_length = strlen(needle);
        size_t limit = start < 0 || (uint64_t)start > text_length ? text_length : (size_t)start;
        if (!needle_length)
            return oasp_value_int((int64_t)limit);
        if (needle_length > limit)
            return oasp_value_int(0);
        for (size_t position = limit - needle_length + 1; position-- > 0;) {
            bool equal = true;
            for (size_t i = 0; i < needle_length; i++) {
                unsigned char left = (unsigned char)text[position + i];
                unsigned char right = (unsigned char)needle[i];
                if (compare) {
                    left = (unsigned char)tolower(left);
                    right = (unsigned char)tolower(right);
                }
                if (left != right) {
                    equal = false;
                    break;
                }
            }
            if (equal)
                return oasp_value_int((int64_t)position + 1);
        }
        return oasp_value_int(0);
    }
    if (oasp_ascii_equal(name, "split")) {
        char *text = count ? oasp_value_format(runtime->format_arena, &args[0]) : NULL;
        char *separator = count > 1 ? oasp_value_format(runtime->format_arena, &args[1]) : " ";
        if (!text || !separator)
            return oasp_value_empty();
        size_t separator_length = strlen(separator);
        size_t item_count = 1;
        if (separator_length)
            for (const char *scan = text; (scan = strstr(scan, separator)) != NULL; scan += separator_length)
                item_count++;
        oasp_array *array = oasp_runtime_alloc_array(runtime);
        if (!array)
            return oasp_value_empty();
        array->rank = 1;
        array->length = item_count;
        array->bounds = oasp_runtime_alloc(runtime, sizeof(*array->bounds), _Alignof(int64_t));
        array->items = oasp_runtime_alloc_values(runtime, item_count);
        if (!array->bounds || !array->items)
            return oasp_value_empty();
        array->bounds[0] = (int64_t)item_count - 1;
        const char *cursor = text;
        for (size_t i = 0; i < item_count; i++) {
            const char *next = separator_length ? strstr(cursor, separator) : NULL;
            size_t part_length = next ? (size_t)(next - cursor) : strlen(cursor);
            array->items[i] = oasp_runtime_string(runtime, cursor, part_length);
            cursor = next ? next + separator_length : cursor + part_length;
        }
        oasp_value result = oasp_value_empty();
        result.kind = OASP_VALUE_ARRAY;
        result.as.array = array;
        return result;
    }
    if (oasp_ascii_equal(name, "timer")) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return oasp_value_float(0);
        return oasp_value_float((double)now.tv_sec + now.tv_nsec / 1000000000.0);
    }
    if (oasp_ascii_equal(name, "now") || oasp_ascii_equal(name, "date"))
        return oasp_runtime_date_value(runtime, time(NULL));
    if (oasp_ascii_equal(name, "dateserial") && count >= 3) {
        int64_t year;
        int64_t month;
        int64_t day;
        if (!oasp_value_to_int(&args[0], &year) || !oasp_value_to_int(&args[1], &month) || !oasp_value_to_int(&args[2], &day)) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        if (year >= 100 && month >= 1 && month <= 12 && day >= 1 && day <= oasp_days_in_month((int)year, (int)month))
            return oasp_runtime_date_parts(runtime, (int)year, (int)month, (int)day, 0, 0, 0);
        struct tm parts;
        memset(&parts, 0, sizeof(parts));
        parts.tm_year = (int)year - 1900;
        parts.tm_mon = (int)month - 1;
        parts.tm_mday = (int)day;
        parts.tm_isdst = -1;
        return oasp_runtime_date_value(runtime, mktime(&parts));
    }
    if (oasp_ascii_equal(name, "cdate") && count) {
        time_t timestamp;
        if (!oasp_value_to_time(runtime, &args[0], &timestamp)) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        return oasp_runtime_date_value(runtime, timestamp);
    }
    if (oasp_ascii_equal(name, "asc") || oasp_ascii_equal(name, "ascb") || oasp_ascii_equal(name, "ascw")) {
        char *text = count ? oasp_value_format(runtime->format_arena, &args[0]) : NULL;
        size_t text_length = count ? oasp_runtime_formatted_length(&args[0], text) : 0;
        if (!text || !text_length) {
            oasp_runtime_error(runtime, 5, "invalid procedure call", line);
            return oasp_value_empty();
        }
        if (oasp_ascii_equal(name, "ascw")) {
            const unsigned char *bytes = (const unsigned char *)text;
            uint32_t codepoint = bytes[0];
            if ((bytes[0] & 0xe0) == 0xc0 && bytes[1])
                codepoint = ((uint32_t)(bytes[0] & 0x1f) << 6) | (uint32_t)(bytes[1] & 0x3f);
            else if ((bytes[0] & 0xf0) == 0xe0 && bytes[1] && bytes[2])
                codepoint = ((uint32_t)(bytes[0] & 0x0f) << 12) | ((uint32_t)(bytes[1] & 0x3f) << 6) | (uint32_t)(bytes[2] & 0x3f);
            return oasp_value_int((int64_t)codepoint);
        }
        return oasp_value_int((unsigned char)text[0]);
    }
    if (oasp_ascii_equal(name, "sgn") || oasp_ascii_equal(name, "abs") || oasp_ascii_equal(name, "int") || oasp_ascii_equal(name, "fix") ||
        oasp_ascii_equal(name, "atn") || oasp_ascii_equal(name, "sin") || oasp_ascii_equal(name, "cos") || oasp_ascii_equal(name, "tan") ||
        oasp_ascii_equal(name, "exp") || oasp_ascii_equal(name, "log")) {
        double number;
        if (!count || !oasp_value_to_float(&args[0], &number)) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        if (oasp_ascii_equal(name, "sgn"))
            return oasp_value_int(number < 0 ? -1 : number > 0 ? 1 : 0);
        if (oasp_ascii_equal(name, "abs")) {
            if (args[0].kind == OASP_VALUE_INT) {
                if (args[0].as.integer == INT64_MIN) {
                    oasp_runtime_error(runtime, 6, "overflow", line);
                    return oasp_value_empty();
                }
                return oasp_value_int(args[0].as.integer < 0 ? -args[0].as.integer : args[0].as.integer);
            }
            return oasp_value_float(fabs(number));
        }
        if (oasp_ascii_equal(name, "int"))
            return oasp_value_float(floor(number));
        if (oasp_ascii_equal(name, "fix"))
            return oasp_value_float(trunc(number));
        if (oasp_ascii_equal(name, "atn"))
            return oasp_value_float(atan(number));
        if (oasp_ascii_equal(name, "sin"))
            return oasp_value_float(sin(number));
        if (oasp_ascii_equal(name, "cos"))
            return oasp_value_float(cos(number));
        if (oasp_ascii_equal(name, "tan"))
            return oasp_value_float(tan(number));
        if (oasp_ascii_equal(name, "exp"))
            return oasp_value_float(exp(number));
        if (number <= 0) {
            oasp_runtime_error(runtime, 5, "invalid procedure call", line);
            return oasp_value_empty();
        }
        return oasp_value_float(log(number));
    }
    if (oasp_ascii_equal(name, "hex")) {
        int64_t integer = 0;
        if (!count || !oasp_value_to_int(&args[0], &integer)) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        char text[32];
        int length = snprintf(text, sizeof(text), "%llX", (unsigned long long)integer);
        return length > 0 ? oasp_runtime_string(runtime, text, (size_t)length) : oasp_value_empty();
    }
    if (oasp_ascii_equal(name, "dateadd") && count >= 3) {
        char *interval = oasp_value_format(runtime->format_arena, &args[0]);
        int64_t amount = 0;
        time_t timestamp;
        if (!interval || !oasp_value_to_int(&args[1], &amount) || !oasp_value_to_time(runtime, &args[2], &timestamp))
            return args[2];
        struct tm *local = localtime(&timestamp);
        if (!local)
            return args[2];
        struct tm parts = *local;
        if (oasp_ascii_equal_n(interval, strlen(interval), "h"))
            parts.tm_hour += (int)amount;
        else if (oasp_ascii_equal_n(interval, strlen(interval), "d"))
            parts.tm_mday += (int)amount;
        else if (oasp_ascii_equal_n(interval, strlen(interval), "n"))
            parts.tm_min += (int)amount;
        else if (oasp_ascii_equal_n(interval, strlen(interval), "s"))
            parts.tm_sec += (int)amount;
        else if (oasp_ascii_equal_n(interval, strlen(interval), "m")) {
            int original_day = parts.tm_mday;
            parts.tm_mday = 1;
            parts.tm_mon += (int)amount;
            (void)mktime(&parts);
            int maximum = oasp_days_in_month(parts.tm_year + 1900, parts.tm_mon + 1);
            parts.tm_mday = original_day < maximum ? original_day : maximum;
        } else if (oasp_ascii_equal_n(interval, strlen(interval), "yyyy")) {
            int original_day = parts.tm_mday;
            parts.tm_mday = 1;
            parts.tm_year += (int)amount;
            int maximum = oasp_days_in_month(parts.tm_year + 1900, parts.tm_mon + 1);
            parts.tm_mday = original_day < maximum ? original_day : maximum;
        }
        return oasp_runtime_date_value(runtime, mktime(&parts));
    }
    if (oasp_ascii_equal(name, "weekday") && count) {
        char *input = oasp_value_format(runtime->format_arena, &args[0]);
        int year = 0, month = 0, day = 0;
        if (!input || sscanf(input, "%d-%d-%d", &year, &month, &day) != 3)
            return oasp_value_int(1);
        struct tm parts;
        memset(&parts, 0, sizeof(parts));
        parts.tm_year = year - 1900;
        parts.tm_mon = month - 1;
        parts.tm_mday = day;
        if (mktime(&parts) == (time_t)-1)
            return oasp_value_int(1);
        return oasp_value_int(parts.tm_wday + 1);
    }
    if (oasp_ascii_equal(name, "formatdatetime") && count) {
        char *input = oasp_value_format(runtime->format_arena, &args[0]);
        int64_t format = 0;
        if (count > 1)
            (void)oasp_value_to_int(&args[1], &format);
        if (!input)
            return oasp_value_empty();
        const char *space = strchr(input, ' ');
        if ((format == 1 || format == 2) && space)
            return oasp_runtime_string(runtime, input, (size_t)(space - input));
        if ((format == 3 || format == 4) && space)
            return oasp_runtime_string(runtime, space + 1, strlen(space + 1));
        return oasp_runtime_string(runtime, input, strlen(input));
    }
    if (oasp_ascii_equal(name, "datediff") && count >= 3) {
        char *interval = oasp_value_format(runtime->format_arena, &args[0]);
        time_t left;
        time_t right;
        if (!interval || !oasp_value_to_time(runtime, &args[1], &left) || !oasp_value_to_time(runtime, &args[2], &right))
            return oasp_value_int(0);
        double seconds = difftime(right, left);
        if (oasp_ascii_equal_n(interval, strlen(interval), "d"))
            seconds /= 86400.0;
        else if (oasp_ascii_equal_n(interval, strlen(interval), "h"))
            seconds /= 3600.0;
        else if (oasp_ascii_equal_n(interval, strlen(interval), "n"))
            seconds /= 60.0;
        return oasp_value_int((int64_t)seconds);
    }
    if (oasp_ascii_equal(name, "datepart") && count >= 2) {
        char *interval = oasp_value_format(runtime->format_arena, &args[0]);
        time_t timestamp;
        if (!interval || !oasp_value_to_time(runtime, &args[1], &timestamp)) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        struct tm *parts = localtime(&timestamp);
        if (!parts)
            return oasp_value_empty();
        if (oasp_ascii_equal_n(interval, strlen(interval), "yyyy"))
            return oasp_value_int(parts->tm_year + 1900);
        if (oasp_ascii_equal_n(interval, strlen(interval), "q"))
            return oasp_value_int(parts->tm_mon / 3 + 1);
        if (oasp_ascii_equal_n(interval, strlen(interval), "m"))
            return oasp_value_int(parts->tm_mon + 1);
        if (oasp_ascii_equal_n(interval, strlen(interval), "d"))
            return oasp_value_int(parts->tm_mday);
        if (oasp_ascii_equal_n(interval, strlen(interval), "h"))
            return oasp_value_int(parts->tm_hour);
        if (oasp_ascii_equal_n(interval, strlen(interval), "n"))
            return oasp_value_int(parts->tm_min);
        if (oasp_ascii_equal_n(interval, strlen(interval), "s"))
            return oasp_value_int(parts->tm_sec);
        return oasp_value_int(0);
    }
    if (oasp_ascii_equal(name, "unescape") || oasp_ascii_equal(name, "vbsunescape")) {
        char *text = count ? oasp_value_format(runtime->format_arena, &args[0]) : "";
        return oasp_vbs_unescape(runtime, text ? text : "");
    }
    if (oasp_ascii_equal(name, "escape")) {
        char *text = count ? oasp_value_format(runtime->format_arena, &args[0]) : "";
        return oasp_vb_escape(runtime, text ? text : "");
    }
    if (oasp_ascii_equal(name, "vbsescape")) {
        char *text = count ? oasp_value_format(runtime->format_arena, &args[0]) : "";
        return oasp_vb_escape(runtime, text ? text : "");
    }
    if (oasp_ascii_equal(name, "chr") || oasp_ascii_equal(name, "chrb") || oasp_ascii_equal(name, "chrw")) {
        int64_t codepoint = 0;
        if (!count || !oasp_value_to_int(&args[0], &codepoint) || codepoint < 0 || codepoint > 0xffff) {
            oasp_runtime_error(runtime, 5, "invalid procedure call", line);
            return oasp_value_empty();
        }
        if (oasp_ascii_equal(name, "chrb")) {
            char byte = (char)(unsigned char)codepoint;
            return oasp_runtime_string(runtime, &byte, 1);
        }
        return oasp_utf8_character(runtime, (uint32_t)codepoint);
    }
builtin_replace:
    if (builtin_id == OASP_BUILTIN_REPLACE || oasp_ascii_equal(name, "replace")) {
        if (count < 3) {
            oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
            return oasp_value_empty();
        }
        char *input = oasp_value_format(runtime->format_arena, &args[0]);
        char *find = oasp_value_format(runtime->format_arena, &args[1]);
        char *replacement = oasp_value_format(runtime->format_arena, &args[2]);
        if (!input || !find || !replacement)
            return oasp_value_empty();
        if (!*find)
            return oasp_runtime_string(runtime, input, strlen(input));
        oasp_buffer output;
        if (!oasp_buffer_init(&output, strlen(input) + 16))
            return oasp_value_empty();
        size_t find_length = strlen(find);
        const char *cursor = input;
        const char *match;
        while ((match = strstr(cursor, find))) {
            (void)oasp_buffer_append(&output, cursor, (size_t)(match - cursor));
            (void)oasp_buffer_append(&output, replacement, strlen(replacement));
            cursor = match + find_length;
        }
        (void)oasp_buffer_append(&output, cursor, strlen(cursor));
        oasp_value result = oasp_runtime_string(runtime, output.data, output.length);
        oasp_buffer_dispose(&output);
        return result;
    }
builtin_trim:
    if (builtin_id == OASP_BUILTIN_TRIM || oasp_ascii_equal(name, "trim")) {
        if (!count || args[0].kind == OASP_VALUE_EMPTY)
            return oasp_value_string("", 0);
        if (args[0].kind == OASP_VALUE_NULL)
            return oasp_value_null();
        char *text = oasp_value_format(runtime->format_arena, &args[0]);
        if (!text)
            return oasp_value_empty();
        size_t start = 0;
        size_t end = strlen(text);
        while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n'))
            start++;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
            end--;
        return oasp_runtime_string(runtime, text + start, end - start);
    }
    if (oasp_ascii_equal(name, "array")) {
        oasp_array *array = oasp_runtime_alloc_array(runtime);
        if (!array)
            return oasp_value_empty();
        memset(array, 0, sizeof(*array));
        array->rank = 1;
        array->length = count;
        array->bounds = oasp_runtime_alloc(runtime, sizeof(*array->bounds), _Alignof(int64_t));
        if (!array->bounds)
            return oasp_value_empty();
        array->bounds[0] = count ? (int64_t)count - 1 : -1;
        if (count) {
            array->items = oasp_runtime_alloc_values(runtime, count);
            if (!array->items)
                return oasp_value_empty();
            for (size_t i = 0; i < count; i++)
                array->items[i] = oasp_runtime_value_copy(runtime, &args[i], false);
        }
        oasp_value result = oasp_value_empty();
        result.kind = OASP_VALUE_ARRAY;
        result.as.array = array;
        return result;
    }
    if (oasp_ascii_equal(name, "lbound") || oasp_ascii_equal(name, "ubound")) {
        if (!count || args[0].kind != OASP_VALUE_ARRAY || !args[0].as.array->rank) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        size_t rank = 0;
        if (count > 1) {
            int64_t requested = 0;
            if (!oasp_value_to_int(&args[1], &requested) || requested < 1 || (size_t)requested > args[0].as.array->rank) {
                oasp_runtime_error(runtime, 9, "subscript out of range", line);
                return oasp_value_empty();
            }
            rank = (size_t)requested - 1;
        }
        return oasp_ascii_equal(name, "lbound") ? oasp_value_int(0) : oasp_value_int(args[0].as.array->bounds[rank]);
    }
    if (oasp_ascii_equal(name, "join")) {
        if (!count || args[0].kind != OASP_VALUE_ARRAY || args[0].as.array->rank != 1) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        const char *separator = " ";
        if (count > 1) {
            separator = oasp_value_format(runtime->format_arena, &args[1]);
            if (!separator)
                return oasp_value_empty();
        }
        oasp_buffer output;
        if (!oasp_buffer_init(&output, 64))
            return oasp_value_empty();
        for (size_t i = 0; i < args[0].as.array->length; i++) {
            char *text = oasp_value_format(runtime->format_arena, &args[0].as.array->items[i]);
            if (i)
                oasp_buffer_append(&output, separator, strlen(separator));
            if (text)
                oasp_buffer_append(&output, text, strlen(text));
        }
        oasp_value result = oasp_runtime_string(runtime, output.data, output.length);
        oasp_buffer_dispose(&output);
        return result;
    }
    if (oasp_ascii_equal(name, "execute") || oasp_ascii_equal(name, "executeglobal") || oasp_ascii_equal(name, "eval")) {
        if (!count || args[0].kind != OASP_VALUE_STRING) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        oasp_parser parser;
        if (!oasp_parser_init(&parser, runtime->arena, args[0].as.string.data, args[0].as.string.length)) {
            oasp_parser_dispose(&parser);
            oasp_runtime_error(runtime, 1002, "syntax error", line);
            return oasp_value_empty();
        }
        if (oasp_ascii_equal(name, "eval")) {
            oasp_expr *dynamic_expression = oasp_parse_expression(&parser);
            if (!dynamic_expression || parser.error) {
                oasp_parser_dispose(&parser);
                oasp_runtime_error(runtime, 1002, "syntax error", line);
                return oasp_value_empty();
            }
            oasp_parser_dispose(&parser);
            return oasp_runtime_eval(runtime, dynamic_expression);
        }
        oasp_block dynamic_program;
        if (!oasp_parse_program(&parser, &dynamic_program)) {
            oasp_parser_dispose(&parser);
            oasp_runtime_error(runtime, 1002, "syntax error", line);
            return oasp_value_empty();
        }
        oasp_parser_dispose(&parser);
        oasp_frame *saved_frame = runtime->frame;
        if (oasp_ascii_equal(name, "executeglobal")) {
            runtime->frame = NULL;
            (void)oasp_runtime_execute(runtime, &dynamic_program);
        } else {
            (void)oasp_runtime_execute_dynamic(runtime, &dynamic_program);
        }
        runtime->frame = saved_frame;
        return oasp_value_empty();
    }
builtin_htmlencode:
    if (builtin_id == OASP_BUILTIN_SERVER_HTMLENCODE || oasp_ascii_equal(name, "server.htmlencode")) {
        if (!count)
            return oasp_value_string("", 0);
        char formatted[64];
        oasp_slice raw;
        if (!oasp_value_text(&args[0], formatted, sizeof(formatted), &raw))
            return oasp_value_empty();
        size_t raw_length = strnlen(raw.data, raw.length);
        size_t output_length = 0;
        for (size_t i = 0; i < raw_length; i++) {
            size_t escaped_length = raw.data[i] == '&'                     ? 5
                                  : raw.data[i] == '"'                     ? 6
                                  : raw.data[i] == '<' || raw.data[i] == '>' ? 4
                                  : raw.data[i] == '\''                    ? 5
                                                                          : 1;
            if (escaped_length > SIZE_MAX - output_length)
                return oasp_value_empty();
            output_length += escaped_length;
        }
        if (output_length == SIZE_MAX)
            return oasp_value_empty();
        char *output = oasp_runtime_alloc(runtime, output_length + 1, _Alignof(char));
        if (!output)
            return oasp_value_empty();
        size_t offset = 0;
        for (size_t i = 0; i < raw_length; i++) {
            const char *escaped = raw.data[i] == '&'    ? "&amp;"
                                  : raw.data[i] == '<'  ? "&lt;"
                                  : raw.data[i] == '>'  ? "&gt;"
                                  : raw.data[i] == '"'  ? "&quot;"
                                  : raw.data[i] == '\'' ? "&#39;"
                                                         : NULL;
            if (escaped) {
                size_t escaped_length = strlen(escaped);
                memcpy(output + offset, escaped, escaped_length);
                offset += escaped_length;
            } else {
                output[offset++] = raw.data[i];
            }
        }
        output[offset] = '\0';
        return oasp_value_string(output, output_length);
    }
builtin_urlencode:
    if (builtin_id == OASP_BUILTIN_SERVER_URLENCODE || oasp_ascii_equal(name, "server.urlencode")) {
        char *raw = count ? oasp_value_format(runtime->format_arena, &args[0]) : "";
        if (!raw)
            return oasp_value_empty();
        static const char digits[] = "0123456789ABCDEF";
        oasp_buffer output;
        if (!oasp_buffer_init(&output, strlen(raw) * 3 + 1))
            return oasp_value_empty();
        for (size_t i = 0; raw[i]; i++) {
            unsigned char ch = (unsigned char)raw[i];
            if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '*') {
                (void)oasp_buffer_append_byte(&output, (char)ch);
            } else if (ch == ' ') {
                (void)oasp_buffer_append_byte(&output, '+');
            } else {
                char escaped[3] = {'%', digits[ch >> 4], digits[ch & 0x0f]};
                (void)oasp_buffer_append(&output, escaped, sizeof(escaped));
            }
        }
        oasp_value result = oasp_runtime_string(runtime, output.data, output.length);
        oasp_buffer_dispose(&output);
        return result;
    }
    if (oasp_ascii_equal(name, "typename") && count) {
        const char *type = args[0].kind == OASP_VALUE_DATE      ? "Date"
                           : args[0].kind == OASP_VALUE_STRING  ? "String"
                           : args[0].kind == OASP_VALUE_BOOL    ? "Boolean"
                           : args[0].kind == OASP_VALUE_INT     ? (args[0].as.integer >= INT16_MIN && args[0].as.integer <= INT16_MAX ? "Integer" : "Long")
                           : args[0].kind == OASP_VALUE_FLOAT   ? "Double"
                           : args[0].kind == OASP_VALUE_OBJECT  ? "Object"
                           : args[0].kind == OASP_VALUE_ARRAY   ? "Variant()"
                           : args[0].kind == OASP_VALUE_NULL    ? "Null"
                           : args[0].kind == OASP_VALUE_NOTHING ? "Nothing"
                                                                : "Empty";
        return oasp_runtime_string(runtime, type, strlen(type));
    }
    if (count && (args[0].kind == OASP_VALUE_DATE || args[0].kind == OASP_VALUE_STRING) &&
        (oasp_ascii_equal(name, "year") || oasp_ascii_equal(name, "month") || oasp_ascii_equal(name, "day") || oasp_ascii_equal(name, "hour") ||
         oasp_ascii_equal(name, "minute") || oasp_ascii_equal(name, "second"))) {
        time_t timestamp;
        if (oasp_value_to_time(runtime, &args[0], &timestamp)) {
            struct tm *parts = localtime(&timestamp);
            if (!parts)
                return oasp_value_empty();
            if (oasp_ascii_equal(name, "year"))
                return oasp_value_int(parts->tm_year + 1900);
            if (oasp_ascii_equal(name, "month"))
                return oasp_value_int(parts->tm_mon + 1);
            if (oasp_ascii_equal(name, "day"))
                return oasp_value_int(parts->tm_mday);
            if (oasp_ascii_equal(name, "hour"))
                return oasp_value_int(parts->tm_hour);
            if (oasp_ascii_equal(name, "minute"))
                return oasp_value_int(parts->tm_min);
            return oasp_value_int(parts->tm_sec);
        }
    }
    oasp_runtime_error(runtime, 438, oasp_runtime_named_error(runtime, "unknown function or object member: ", name), line);
    return oasp_value_empty();
}

/*
 * Expression and assignment dispatch: resolves script classes, components,
 * members, optimized symbol ids, calls, operators, and assignable targets.
 */
static oasp_value oasp_object_create(oasp_runtime *runtime, oasp_slice class_name, size_t line) {
    oasp_class_def *class_def = oasp_find_class(runtime, class_name);
    if (!class_def) {
        return oasp_component_create(runtime, class_name, line);
    }
    oasp_object *object = oasp_runtime_alloc_object(runtime);
    if (!object)
        return oasp_value_empty();
    memset(object, 0, sizeof(*object));
    object->class_def = class_def;

    oasp_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.parent = runtime->frame;
    frame.object = object;
    runtime->frame = &frame;
    const oasp_bytecode *previous_bytecode = runtime->bytecode;
    runtime->bytecode = class_def->bytecode;
    oasp_class_bytecode class_metadata;
    bool ok = oasp_bytecode_class_at(class_def->bytecode, class_def->class_index, &class_metadata) &&
              oasp_vm_execute_range_mode(runtime, class_def->bytecode, class_metadata.entry,
                                         class_metadata.limit, class_metadata.error_aware);
    runtime->bytecode = previous_bytecode;
    object->fields = frame.locals;
    runtime->frame = frame.parent;
    if (!ok)
        return oasp_value_empty();
    oasp_slice initializer_name = {"Class_Initialize", 16};
    oasp_routine_ref initializer = oasp_find_method(object, initializer_name, false);
    if (initializer.bytecode)
        (void)oasp_invoke_routine(runtime, initializer, object, NULL, 0);
    if (runtime->error_pending && !runtime->resume_next)
        return oasp_value_empty();

    oasp_value value = oasp_value_empty();
    value.kind = OASP_VALUE_OBJECT;
    value.as.object = object;
    return value;
}

static oasp_value *oasp_runtime_eval_arguments(oasp_runtime *runtime, oasp_expr *const *expressions, size_t count) {
    if (!count)
        return NULL;
    oasp_value *arguments = oasp_arena_alloc(runtime->scratch, count * sizeof(*arguments), _Alignof(oasp_value));
    if (!arguments)
        return NULL;
    for (size_t i = 0; i < count; i++)
        arguments[i] = expressions[i] ? oasp_runtime_eval(runtime, expressions[i]) : oasp_value_empty();
    return arguments;
}

static oasp_value oasp_invoke_routine_values(oasp_runtime *runtime, oasp_routine_ref routine, oasp_object *object, const oasp_value *arguments, oasp_expr *const *references,
                                             size_t count) {
    if (!runtime || !runtime->scratch || !routine.bytecode)
        return oasp_value_empty();
    oasp_frame *caller = runtime->frame;
    oasp_arena_checkpoint checkpoint = oasp_arena_checkpoint_save(runtime->scratch);
    oasp_value result = oasp_invoke_routine_inner(runtime, routine, object, references, arguments, NULL, NULL, count);
    runtime->frame = caller;
    oasp_arena_checkpoint_restore(runtime->scratch, checkpoint);
    return result;
}

static oasp_value oasp_invoke_routine_encoded(oasp_runtime *runtime, oasp_routine_ref routine, oasp_object *object, const oasp_value *arguments, const uintptr_t *references,
                                              const oasp_bytecode *argument_bytecode, size_t count) {
    if (!runtime || !runtime->scratch || !routine.bytecode)
        return oasp_value_empty();
    oasp_frame *caller = runtime->frame;
    oasp_arena_checkpoint checkpoint = oasp_arena_checkpoint_save(runtime->scratch);
    oasp_value result = oasp_invoke_routine_inner(runtime, routine, object, NULL, arguments, references,
                                                  argument_bytecode, count);
    runtime->frame = caller;
    oasp_arena_checkpoint_restore(runtime->scratch, checkpoint);
    return result;
}

oasp_value oasp_runtime_call_routine(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t routine_index, const oasp_value *arguments, oasp_expr *const *references,
                                     size_t count) {
    if (!runtime || !bytecode || routine_index >= bytecode->routine_count)
        return oasp_value_empty();
    return oasp_invoke_routine_values(runtime, (oasp_routine_ref){bytecode, routine_index}, NULL,
                                      arguments, references, count);
}

oasp_value oasp_runtime_call_routine_view(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t routine_index, const oasp_value *arguments, const uintptr_t *references,
                                          size_t count) {
    if (!runtime || !runtime->scratch || !bytecode || routine_index >= bytecode->routine_count)
        return oasp_value_empty();
    return oasp_invoke_routine_encoded(runtime, (oasp_routine_ref){bytecode, routine_index}, NULL,
                                       arguments, references, bytecode, count);
}

oasp_value oasp_runtime_get_member(oasp_runtime *runtime, const oasp_value *receiver, oasp_slice member, size_t line) {
    if (!runtime || !receiver)
        return oasp_value_empty();
    if (receiver->kind == OASP_VALUE_OBJECT)
        return oasp_object_get(runtime, receiver->as.object, member, line);
    if (oasp_ascii_equal(member, "value") || oasp_ascii_equal(member, "item"))
        return *receiver;
    oasp_runtime_error(runtime, 424, "object required", line);
    return oasp_value_empty();
}

oasp_value oasp_runtime_get_member_id(oasp_runtime *runtime, const oasp_value *receiver, uint16_t member_id, oasp_slice member, size_t line) {
    if (!runtime || !receiver)
        return oasp_value_empty();
    if (member_id && receiver->kind == OASP_VALUE_OBJECT && receiver->as.object &&
        receiver->as.object->component_kind != OASP_COMPONENT_NONE) {
        oasp_value result = oasp_value_empty();
        if (oasp_component_get_id(runtime, receiver->as.object, member_id, member, line, &result))
            return result;
    }
    return oasp_runtime_get_member(runtime, receiver, member, line);
}

oasp_value oasp_runtime_get_name_id(oasp_runtime *runtime, oasp_slice name, uint16_t member_id, size_t line) {
    if (runtime && member_id) {
        if (name.length > 1 && name.data[0] == '.' && runtime->with_object) {
            oasp_value receiver = oasp_value_empty();
            receiver.kind = OASP_VALUE_OBJECT;
            receiver.as.object = runtime->with_object;
            return oasp_runtime_get_member_id(runtime, &receiver, member_id,
                                              (oasp_slice){name.data + 1, name.length - 1}, line);
        }
        oasp_object *object = NULL;
        oasp_slice member;
        if (oasp_resolve_object_path(runtime, name, line, &object, &member)) {
            oasp_value receiver = oasp_value_empty();
            receiver.kind = OASP_VALUE_OBJECT;
            receiver.as.object = object;
            return oasp_runtime_get_member_id(runtime, &receiver, member_id, member, line);
        }
    }
    oasp_expr expression;
    memset(&expression, 0, sizeof(expression));
    expression.kind = OASP_EXPR_IDENTIFIER;
    expression.name = name;
    expression.line = line;
    return oasp_runtime_eval(runtime, &expression);
}

static oasp_value oasp_runtime_call_default_arguments(oasp_runtime *runtime, const oasp_bytecode *argument_bytecode, const oasp_value *receiver, const oasp_value *arguments, oasp_expr *const *references,
                                                      const uintptr_t *encoded_references, size_t count, size_t line);

static oasp_value oasp_runtime_call_member_arguments(oasp_runtime *runtime, const oasp_bytecode *argument_bytecode, const oasp_value *receiver, uint16_t member_id, oasp_slice member,
                                                     const oasp_value *arguments, oasp_expr *const *references, const uintptr_t *encoded_references, size_t count, size_t line) {
    if (!runtime || !receiver)
        return oasp_value_empty();
    if (receiver->kind == OASP_VALUE_OBJECT) {
        oasp_object *object = receiver->as.object;
        oasp_resolve_persisted_class(runtime, object);
        oasp_routine_ref method = oasp_find_method(object, member, false);
        if (method.bytecode)
            return encoded_references
                       ? oasp_invoke_routine_encoded(runtime, method, object, arguments, encoded_references,
                                                     argument_bytecode, count)
                       : oasp_invoke_routine_values(runtime, method, object, arguments, references, count);
        if (object->component_kind != OASP_COMPONENT_NONE) {
            oasp_value result = oasp_value_empty();
            if (oasp_component_call_id(runtime, object, member_id, member, (oasp_value *)arguments, count, line, &result))
                return result;
        }
        oasp_binding *field = oasp_binding_find(object->fields, member);
        if (field && (field->value.kind == OASP_VALUE_ARRAY || field->value.kind == OASP_VALUE_OBJECT))
            return oasp_runtime_call_default_arguments(runtime, argument_bytecode, &field->value, arguments,
                                                       references, encoded_references, count, line);
    }
    oasp_runtime_error(runtime, 438, oasp_runtime_named_error(runtime, "unknown function or object member: ", member), line);
    return oasp_value_empty();
}

oasp_value oasp_runtime_call_member_id(oasp_runtime *runtime, const oasp_value *receiver, uint16_t member_id, oasp_slice member, const oasp_value *arguments,
                                       oasp_expr *const *references, size_t count, size_t line) {
    return oasp_runtime_call_member_arguments(runtime, NULL, receiver, member_id, member, arguments, references,
                                              NULL, count, line);
}

oasp_value oasp_runtime_call_member_view(oasp_runtime *runtime, const oasp_bytecode *bytecode, const oasp_value *receiver, uint16_t member_id, oasp_slice member,
                                         const oasp_value *arguments, const uintptr_t *references, size_t count, size_t line) {
    return oasp_runtime_call_member_arguments(runtime, bytecode, receiver, member_id, member, arguments, NULL,
                                              references, count, line);
}

oasp_value oasp_runtime_call_member(oasp_runtime *runtime, const oasp_value *receiver, oasp_slice member, const oasp_value *arguments, oasp_expr *const *references,
                                    size_t count, size_t line) {
    return oasp_runtime_call_member_id(runtime, receiver, 0, member, arguments, references, count, line);
}

static oasp_value oasp_runtime_call_default_arguments(oasp_runtime *runtime, const oasp_bytecode *argument_bytecode, const oasp_value *receiver, const oasp_value *arguments, oasp_expr *const *references,
                                                      const uintptr_t *encoded_references, size_t count, size_t line) {
    if (!runtime || !receiver)
        return oasp_value_empty();
    if (receiver->kind == OASP_VALUE_OBJECT) {
        oasp_object *object = receiver->as.object;
        oasp_resolve_persisted_class(runtime, object);
        oasp_routine_ref method = oasp_find_method(object, (oasp_slice){"", 0}, true);
        if (method.bytecode)
            return encoded_references
                       ? oasp_invoke_routine_encoded(runtime, method, object, arguments, encoded_references,
                                                     argument_bytecode, count)
                       : oasp_invoke_routine_values(runtime, method, object, arguments, references, count);
        oasp_value result = oasp_value_empty();
        if (object->component_kind != OASP_COMPONENT_NONE &&
            oasp_component_default(runtime, object, (oasp_value *)arguments, count, line, &result))
            return result;
    } else if (receiver->kind == OASP_VALUE_ARRAY) {
        size_t offset = 0;
        if (oasp_array_offset(runtime, receiver->as.array, (oasp_value *)arguments, count, line, &offset))
            return receiver->as.array->items[offset];
        return oasp_value_empty();
    }
    oasp_runtime_error(runtime, 438, "object does not support call", line);
    return oasp_value_empty();
}

oasp_value oasp_runtime_call_default(oasp_runtime *runtime, const oasp_value *receiver, const oasp_value *arguments, oasp_expr *const *references, size_t count,
                                     size_t line) {
    return oasp_runtime_call_default_arguments(runtime, NULL, receiver, arguments, references, NULL, count, line);
}

oasp_value oasp_runtime_call_default_view(oasp_runtime *runtime, const oasp_bytecode *bytecode, const oasp_value *receiver, const oasp_value *arguments, const uintptr_t *references,
                                          size_t count, size_t line) {
    return oasp_runtime_call_default_arguments(runtime, bytecode, receiver, arguments, NULL, references, count, line);
}

static oasp_value oasp_runtime_call_name_arguments(oasp_runtime *runtime, const oasp_bytecode *argument_bytecode, oasp_slice name, uint32_t symbol_id, uint16_t builtin_id,
                                                   uint16_t member_id, const oasp_value *arguments, oasp_expr *const *references, const uintptr_t *encoded_references, size_t count, size_t line) {
    if (!runtime)
        return oasp_value_empty();
    if (builtin_id != OASP_BUILTIN_DYNAMIC)
        return oasp_builtin_call(runtime, (oasp_builtin_id)builtin_id, name, (oasp_value *)arguments, count, line);
    if (name.length > 1 && name.data[0] == '.' && runtime->with_object) {
        if (runtime->with_object->component_name &&
            oasp_ascii_equal((oasp_slice){runtime->with_object->component_name,
                                           strlen(runtime->with_object->component_name)}, "Response")) {
            static const char prefix[] = "response";
            char *full_name = oasp_runtime_alloc(runtime, sizeof(prefix) - 1 + name.length + 1, _Alignof(char));
            if (!full_name)
                return oasp_value_empty();
            memcpy(full_name, prefix, sizeof(prefix) - 1);
            memcpy(full_name + sizeof(prefix) - 1, name.data, name.length);
            full_name[sizeof(prefix) - 1 + name.length] = '\0';
            return oasp_builtin_call(runtime, OASP_BUILTIN_DYNAMIC,
                                     (oasp_slice){full_name, sizeof(prefix) - 1 + name.length},
                                     (oasp_value *)arguments, count, line);
        }
        oasp_slice member = {name.data + 1, name.length - 1};
        oasp_value receiver = oasp_value_empty();
        receiver.kind = OASP_VALUE_OBJECT;
        receiver.as.object = runtime->with_object;
        return oasp_runtime_call_member_arguments(runtime, argument_bytecode, &receiver, member_id, member,
                                                  arguments, references, encoded_references, count, line);
    }
    if (runtime->frame && runtime->frame->object) {
        oasp_resolve_persisted_class(runtime, runtime->frame->object);
        oasp_routine_ref method = oasp_find_method(runtime->frame->object, name, false);
        if (method.bytecode)
            return encoded_references
                       ? oasp_invoke_routine_encoded(runtime, method, runtime->frame->object, arguments,
                                                     encoded_references, argument_bytecode, count)
                       : oasp_invoke_routine_values(runtime, method, runtime->frame->object, arguments,
                                                    references, count);
    }
    oasp_routine_def *routine =
        symbol_id ? oasp_find_routine_id(runtime, symbol_id, name) : oasp_find_routine(runtime, name);
    if (routine)
        return encoded_references
                   ? oasp_invoke_routine_encoded(runtime,
                                                 (oasp_routine_ref){routine->bytecode, routine->routine_index},
                                                 NULL, arguments, encoded_references, argument_bytecode, count)
                   : oasp_invoke_routine_values(runtime,
                                                (oasp_routine_ref){routine->bytecode, routine->routine_index},
                                                NULL, arguments, references, count);

    oasp_slice base;
    oasp_slice member;
    if (oasp_split_member(name, &base, &member)) {
        oasp_object *receiver_object = NULL;
        oasp_slice path_member;
        const oasp_value *host_receiver = oasp_runtime_get(runtime, base);
        bool response_wrapper = oasp_ascii_equal(base, "response") && host_receiver &&
                                host_receiver->kind == OASP_VALUE_OBJECT && host_receiver->as.object &&
                                host_receiver->as.object->component_name &&
                                oasp_ascii_equal((oasp_slice){host_receiver->as.object->component_name,
                                                               strlen(host_receiver->as.object->component_name)}, "Response");
        if (!response_wrapper && oasp_resolve_object_path(runtime, name, line, &receiver_object, &path_member)) {
            oasp_routine_ref method = oasp_find_method(receiver_object, path_member, false);
            if (method.bytecode)
                return encoded_references
                           ? oasp_invoke_routine_encoded(runtime, method, receiver_object, arguments,
                                                         encoded_references, argument_bytecode, count)
                           : oasp_invoke_routine_values(runtime, method, receiver_object, arguments,
                                                        references, count);
            if (receiver_object->component_kind != OASP_COMPONENT_NONE) {
                oasp_value result = oasp_value_empty();
                if (oasp_component_call_id(runtime, receiver_object, member_id, path_member,
                                           (oasp_value *)arguments, count, line, &result))
                    return result;
            } else {
                oasp_binding *field = oasp_binding_find(receiver_object->fields, path_member);
                if (field && (field->value.kind == OASP_VALUE_ARRAY || field->value.kind == OASP_VALUE_OBJECT))
                    return oasp_runtime_call_default_arguments(runtime, argument_bytecode, &field->value,
                                                               arguments, references, encoded_references,
                                                               count, line);
            }
        }
        const oasp_value *receiver = oasp_runtime_get(runtime, base);
        bool host_builtin = oasp_ascii_equal(base, "request") || oasp_ascii_equal(base, "response") ||
                            oasp_ascii_equal(base, "server") || oasp_ascii_equal(base, "session") ||
                            oasp_ascii_equal(base, "application") || oasp_ascii_equal(base, "err");
        if ((!receiver || receiver->kind != OASP_VALUE_OBJECT) && !host_builtin) {
            oasp_runtime_error(runtime, receiver && receiver->kind == OASP_VALUE_NOTHING ? 91 : 424,
                               receiver && receiver->kind == OASP_VALUE_NOTHING ? "object variable not set" : "object required",
                               line);
            return oasp_value_empty();
        }
    }

    if (oasp_ascii_equal(name, "request") || oasp_ascii_equal(name, "session") || oasp_ascii_equal(name, "application"))
        return oasp_builtin_call(runtime, OASP_BUILTIN_DYNAMIC, name, (oasp_value *)arguments, count, line);
    const oasp_value *callee = oasp_runtime_get(runtime, name);
    if (callee && (callee->kind == OASP_VALUE_OBJECT || callee->kind == OASP_VALUE_ARRAY))
        return oasp_runtime_call_default_arguments(runtime, argument_bytecode, callee, arguments,
                                                   references, encoded_references, count, line);
    return oasp_builtin_call(runtime, (oasp_builtin_id)builtin_id, name, (oasp_value *)arguments, count, line);
}

oasp_value oasp_runtime_call_builtin(oasp_runtime *runtime, uint16_t builtin_id, oasp_slice name, const oasp_value *arguments, size_t count,
                                     size_t line) {
    if (!runtime)
        return oasp_value_empty();
    return oasp_builtin_call(runtime, (oasp_builtin_id)builtin_id, name, (oasp_value *)arguments, count, line);
}

oasp_value oasp_runtime_call_name_id(oasp_runtime *runtime, oasp_slice name, uint32_t symbol_id, uint16_t builtin_id, uint16_t member_id,
                                     const oasp_value *arguments, oasp_expr *const *references, size_t count, size_t line) {
    return oasp_runtime_call_name_arguments(runtime, NULL, name, symbol_id, builtin_id, member_id, arguments,
                                            references, NULL, count, line);
}

oasp_value oasp_runtime_call_name_view(oasp_runtime *runtime, const oasp_bytecode *bytecode, oasp_slice name, uint32_t symbol_id, uint16_t builtin_id,
                                       uint16_t member_id, const oasp_value *arguments, const uintptr_t *references, size_t count, size_t line) {
    return oasp_runtime_call_name_arguments(runtime, bytecode, name, symbol_id, builtin_id, member_id, arguments,
                                            NULL, references, count, line);
}

oasp_value oasp_runtime_call_name(oasp_runtime *runtime, oasp_slice name, const oasp_value *arguments, oasp_expr *const *references, size_t count,
                                  size_t line) {
    return oasp_runtime_call_name_id(runtime, name, 0, OASP_BUILTIN_DYNAMIC, OASP_MEMBER_DYNAMIC,
                                     arguments, references, count, line);
}

oasp_value oasp_runtime_apply_unary(oasp_runtime *runtime, oasp_expr_kind kind, const oasp_value *operand, size_t line) {
    if (!runtime || !operand)
        return oasp_value_empty();
    int64_t integer;
    double floating;
    if (kind == OASP_EXPR_NOT) {
        if (!oasp_value_to_int(operand, &integer)) {
            oasp_runtime_error(runtime, 13, "type mismatch", line);
            return oasp_value_empty();
        }
        return oasp_value_int(~integer);
    }
    if (kind != OASP_EXPR_NEGATE && kind != OASP_EXPR_POSITIVE)
        return oasp_value_empty();
    if (operand->kind == OASP_VALUE_FLOAT && oasp_value_to_float(operand, &floating))
        return oasp_value_float(kind == OASP_EXPR_NEGATE ? -floating : floating);
    if (oasp_value_to_int(operand, &integer)) {
        if (kind == OASP_EXPR_NEGATE && integer == INT64_MIN) {
            oasp_runtime_error(runtime, 6, "overflow", line);
            return oasp_value_empty();
        }
        return oasp_value_int(kind == OASP_EXPR_NEGATE ? -integer : integer);
    }
    oasp_runtime_error(runtime, 13, "type mismatch", line);
    return oasp_value_empty();
}

oasp_value oasp_runtime_apply_binary(oasp_runtime *runtime, oasp_expr_kind kind, const oasp_value *left, const oasp_value *right, size_t line) {
    if (!runtime || !left || !right)
        return oasp_value_empty();
    if (kind == OASP_EXPR_CONCAT)
        return oasp_concat(runtime, left, right);
    if (kind >= OASP_EXPR_EQ && kind <= OASP_EXPR_GE) {
        int comparison = oasp_compare(runtime, left, right);
        bool result = kind == OASP_EXPR_EQ   ? comparison == 0
                      : kind == OASP_EXPR_NE ? comparison != 0
                      : kind == OASP_EXPR_LT ? comparison < 0
                      : kind == OASP_EXPR_LE ? comparison <= 0
                      : kind == OASP_EXPR_GT ? comparison > 0
                                             : comparison >= 0;
        return oasp_value_bool(result);
    }
    if (kind >= OASP_EXPR_ADD && kind <= OASP_EXPR_POWER)
        return oasp_binary_numeric(runtime, kind, left, right, line);
    if (kind == OASP_EXPR_IS || kind == OASP_EXPR_IS_NOT) {
        bool same = (left->kind == OASP_VALUE_NOTHING && right->kind == OASP_VALUE_NOTHING) ||
                    (left->kind == OASP_VALUE_OBJECT && right->kind == OASP_VALUE_OBJECT && left->as.object == right->as.object);
        return oasp_value_bool(kind == OASP_EXPR_IS ? same : !same);
    }
    int64_t a, b;
    if (!oasp_value_to_int(left, &a) || !oasp_value_to_int(right, &b)) {
        oasp_runtime_error(runtime, 13, "type mismatch", line);
        return oasp_value_empty();
    }
    switch (kind) {
    case OASP_EXPR_AND:
        return oasp_value_int(a & b);
    case OASP_EXPR_OR:
        return oasp_value_int(a | b);
    case OASP_EXPR_XOR:
        return oasp_value_int(a ^ b);
    case OASP_EXPR_EQV:
        return oasp_value_int(~(a ^ b));
    case OASP_EXPR_IMP:
        return oasp_value_int((~a) | b);
    default:
        return oasp_value_empty();
    }
}

oasp_value oasp_runtime_eval(oasp_runtime *runtime, const oasp_expr *expr) {
    if (!runtime || !expr)
        return oasp_value_empty();
    if (oasp_ast_expression_kind(expr) == OASP_EXPR_LITERAL)
        return oasp_ast_expression_literal(expr);
    if (oasp_ast_expression_kind(expr) == OASP_EXPR_NEW)
        return oasp_object_create(runtime, oasp_ast_expression_name(expr), oasp_ast_expression_line(expr));
    if (oasp_ast_expression_kind(expr) == OASP_EXPR_MEMBER) {
        if (oasp_ast_expression_left(expr) && oasp_ast_expression_kind(oasp_ast_expression_left(expr)) == OASP_EXPR_IDENTIFIER && oasp_ascii_equal(oasp_ast_expression_name(oasp_ast_expression_left(expr)), "err")) {
            if (oasp_ascii_equal(oasp_ast_expression_name(expr), "number"))
                return oasp_value_int(runtime->error_number);
            if (oasp_ascii_equal(oasp_ast_expression_name(expr), "description")) {
                const char *text = runtime->error_description ? runtime->error_description : "";
                return oasp_runtime_string(runtime, text, strlen(text));
            }
            if (oasp_ascii_equal(oasp_ast_expression_name(expr), "source")) {
                const char *text = runtime->error_source ? runtime->error_source : "";
                return oasp_runtime_string(runtime, text, strlen(text));
            }
            if (oasp_ascii_equal(oasp_ast_expression_name(expr), "clear")) {
                oasp_runtime_clear_error(runtime);
                return oasp_value_empty();
            }
        }
        oasp_value receiver = oasp_runtime_eval(runtime, oasp_ast_expression_left(expr));
        if (receiver.kind == OASP_VALUE_OBJECT)
            return oasp_object_get(runtime, receiver.as.object, oasp_ast_expression_name(expr), oasp_ast_expression_line(expr));
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "value") ||
            oasp_ascii_equal(oasp_ast_expression_name(expr), "item"))
            return receiver;
        oasp_runtime_error(runtime, 424, "object required", oasp_ast_expression_line(expr));
        return oasp_value_empty();
    }
    if (oasp_ast_expression_kind(expr) == OASP_EXPR_IDENTIFIER) {
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "me") && runtime->frame && runtime->frame->object) {
            oasp_value self = oasp_value_empty();
            self.kind = OASP_VALUE_OBJECT;
            self.as.object = runtime->frame->object;
            return self;
        }
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "response")) {
            oasp_object *response = oasp_runtime_alloc_object(runtime);
            if (!response)
                return oasp_value_empty();
            memset(response, 0, sizeof(*response));
            response->component_name = "Response";
            oasp_value value = oasp_value_empty();
            value.kind = OASP_VALUE_OBJECT;
            value.as.object = response;
            return value;
        }
        if (oasp_ast_expression_name(expr).length > 1 && oasp_ast_expression_name(expr).data[0] == '.' && runtime->with_object) {
            oasp_slice member = {oasp_ast_expression_name(expr).data + 1, oasp_ast_expression_name(expr).length - 1};
            return oasp_object_get(runtime, runtime->with_object, member, oasp_ast_expression_line(expr));
        }
        oasp_slice base;
        oasp_slice member;
        if (oasp_split_member(oasp_ast_expression_name(expr), &base, &member)) {
            if (oasp_ascii_equal(base, "err")) {
                if (oasp_ascii_equal(member, "number"))
                    return oasp_value_int(runtime->error_number);
                if (oasp_ascii_equal(member, "description")) {
                    const char *text = runtime->error_description ? runtime->error_description : "";
                    return oasp_runtime_string(runtime, text, strlen(text));
                }
                if (oasp_ascii_equal(member, "source")) {
                    const char *text = runtime->error_source ? runtime->error_source : "";
                    return oasp_runtime_string(runtime, text, strlen(text));
                }
                if (oasp_ascii_equal(member, "clear")) {
                    oasp_runtime_clear_error(runtime);
                    return oasp_value_empty();
                }
            }
            if (oasp_ascii_equal(oasp_ast_expression_name(expr), "request.form") || oasp_ascii_equal(oasp_ast_expression_name(expr), "request.querystring"))
                return oasp_builtin_call(runtime, OASP_BUILTIN_DYNAMIC, oasp_ast_expression_name(expr), NULL, 0,
                                         oasp_ast_expression_line(expr));
            if (oasp_ascii_equal(oasp_ast_expression_name(expr), "request.form.count"))
                return oasp_value_int((int64_t)oasp_request_form_count(runtime->request));
            if (oasp_ascii_equal(oasp_ast_expression_name(expr), "request.querystring.count"))
                return oasp_value_int((int64_t)oasp_request_query_count(runtime->request));
            if (oasp_ascii_equal(oasp_ast_expression_name(expr), "request.totalbytes"))
                return oasp_value_int((int64_t)oasp_request_body_length(runtime->request));
            if (oasp_ascii_equal(oasp_ast_expression_name(expr), "session.timeout"))
                return oasp_value_int(runtime->session_timeout);
            if (oasp_ascii_equal(oasp_ast_expression_name(expr), "response.buffer"))
                return oasp_value_bool(oasp_request_buffered(runtime->request));
            if (oasp_ascii_equal(oasp_ast_expression_name(expr), "response.charset")) {
                const char *charset = oasp_request_charset(runtime->request);
                return oasp_runtime_string(runtime, charset, strlen(charset));
            }
            oasp_object *path_object = NULL;
            oasp_slice path_member;
            if (oasp_resolve_object_path(runtime, oasp_ast_expression_name(expr), oasp_ast_expression_line(expr), &path_object, &path_member))
                return oasp_object_get(runtime, path_object, path_member, oasp_ast_expression_line(expr));
            const oasp_value *receiver = oasp_runtime_get(runtime, base);
            if (receiver) {
                oasp_runtime_error(runtime, receiver->kind == OASP_VALUE_NOTHING ? 91 : 424,
                                   receiver->kind == OASP_VALUE_NOTHING ? "object variable not set" : "object required", oasp_ast_expression_line(expr));
                return oasp_value_empty();
            }
        }
        const oasp_value *value = oasp_runtime_get(runtime, oasp_ast_expression_name(expr));
        if (value)
            return *value;
        if (runtime->frame && runtime->frame->object) {
            oasp_routine_ref method =
                oasp_find_accessor(runtime->frame->object, oasp_ast_expression_name(expr), OASP_ROUTINE_PROPERTY_GET, OASP_ROUTINE_FUNCTION);
            if (method.bytecode)
                return oasp_invoke_routine(runtime, method, runtime->frame->object, NULL, 0);
        }
        oasp_routine_def *routine = oasp_find_routine(runtime, oasp_ast_expression_name(expr));
        oasp_routine_bytecode routine_metadata;
        if (routine && oasp_bytecode_routine_at(routine->bytecode, routine->routine_index, &routine_metadata) &&
            routine_metadata.kind != OASP_ROUTINE_SUB)
            return oasp_invoke_routine(runtime, (oasp_routine_ref){routine->bytecode, routine->routine_index}, NULL, NULL, 0);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "response.clear") || oasp_ascii_equal(oasp_ast_expression_name(expr), "response.flush") || oasp_ascii_equal(oasp_ast_expression_name(expr), "response.end") ||
            oasp_ascii_equal(oasp_ast_expression_name(expr), "application.lock") || oasp_ascii_equal(oasp_ast_expression_name(expr), "application.unlock") ||
            oasp_ascii_equal(oasp_ast_expression_name(expr), "session.abandon") || oasp_ascii_equal(oasp_ast_expression_name(expr), "randomize") || oasp_ascii_equal(oasp_ast_expression_name(expr), "rnd"))
            return oasp_builtin_call(runtime, OASP_BUILTIN_DYNAMIC, oasp_ast_expression_name(expr), NULL, 0,
                                     oasp_ast_expression_line(expr));
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "vbcrlf"))
            return oasp_value_string("\r\n", 2);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "vbnewline"))
            return oasp_value_string("\r\n", 2);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "vbcr"))
            return oasp_value_string("\r", 1);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "vblf"))
            return oasp_value_string("\n", 1);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "err"))
            return oasp_value_int(runtime->error_number);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "now") || oasp_ascii_equal(oasp_ast_expression_name(expr), "date"))
            return oasp_runtime_date_value(runtime, time(NULL));
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "vbgeneraldate"))
            return oasp_value_int(0);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "vblongdate"))
            return oasp_value_int(1);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "vbshortdate"))
            return oasp_value_int(2);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "vblongtime"))
            return oasp_value_int(3);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "vbshorttime"))
            return oasp_value_int(4);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "vbbinarycompare"))
            return oasp_value_int(0);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "vbtextcompare"))
            return oasp_value_int(1);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "scriptengine"))
            return oasp_value_string("VBScript", 8);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "scriptenginemajorversion"))
            return oasp_value_int(5);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "scriptengineminorversion"))
            return oasp_value_int(8);
        if (oasp_ascii_equal(oasp_ast_expression_name(expr), "scriptenginebuildversion"))
            return oasp_value_int(0);
        if (runtime->option_explicit)
            oasp_runtime_error(runtime, 500, oasp_runtime_named_error(runtime, "undefined variable: ", oasp_ast_expression_name(expr)), oasp_ast_expression_line(expr));
        return oasp_value_empty();
    }
    if (oasp_ast_expression_kind(expr) == OASP_EXPR_CALL) {
        if (oasp_ast_expression_left(expr) && oasp_ast_expression_kind(oasp_ast_expression_left(expr)) == OASP_EXPR_CALL &&
            oasp_ast_expression_left(oasp_ast_expression_left(expr)) &&
            oasp_ast_expression_kind(oasp_ast_expression_left(oasp_ast_expression_left(expr))) == OASP_EXPR_IDENTIFIER &&
            oasp_ascii_equal(oasp_ast_expression_name(oasp_ast_expression_left(oasp_ast_expression_left(expr))), "request.cookies") &&
            oasp_ast_expression_arg_count(oasp_ast_expression_left(expr)) == 1 &&
            oasp_ast_expression_arg_count(expr) == 1) {
            oasp_value arguments[2];
            arguments[0] = oasp_runtime_eval(runtime, oasp_ast_expression_arg(oasp_ast_expression_left(expr), 0));
            arguments[1] = oasp_runtime_eval(runtime, oasp_ast_expression_arg(expr, 0));
            return oasp_builtin_call(runtime, OASP_BUILTIN_DYNAMIC, (oasp_slice){"request.cookies.subkey", 22},
                                     arguments, 2, oasp_ast_expression_line(expr));
        }
        if (oasp_ast_expression_left(expr) && oasp_ast_expression_kind(oasp_ast_expression_left(expr)) == OASP_EXPR_MEMBER) {
            if (oasp_ast_expression_left(oasp_ast_expression_left(expr)) && oasp_ast_expression_kind(oasp_ast_expression_left(oasp_ast_expression_left(expr))) == OASP_EXPR_IDENTIFIER && oasp_ascii_equal(oasp_ast_expression_name(oasp_ast_expression_left(oasp_ast_expression_left(expr))), "err")) {
                if (oasp_ascii_equal(oasp_ast_expression_name(oasp_ast_expression_left(expr)), "clear")) {
                    oasp_runtime_clear_error(runtime);
                    return oasp_value_empty();
                }
                if (oasp_ascii_equal(oasp_ast_expression_name(oasp_ast_expression_left(expr)), "raise")) {
                    oasp_value number = oasp_ast_expression_arg_count(expr) && oasp_ast_expression_arg(expr, 0) ? oasp_runtime_eval(runtime, oasp_ast_expression_arg(expr, 0)) : oasp_value_int(5);
                    int64_t code = 5;
                    (void)oasp_value_to_int(&number, &code);
                    const char *source = "OpenASP";
                    const char *description = "runtime error";
                    if (oasp_ast_expression_arg_count(expr) > 1 && oasp_ast_expression_arg(expr, 1)) {
                        oasp_value value = oasp_runtime_eval(runtime, oasp_ast_expression_arg(expr, 1));
                        source = oasp_value_format(runtime->format_arena, &value);
                    }
                    if (oasp_ast_expression_arg_count(expr) > 2 && oasp_ast_expression_arg(expr, 2)) {
                        oasp_value value = oasp_runtime_eval(runtime, oasp_ast_expression_arg(expr, 2));
                        description = oasp_value_format(runtime->format_arena, &value);
                    }
                    oasp_runtime_error(runtime, (int)code, description, oasp_ast_expression_line(expr));
                    runtime->error_source = source ? oasp_runtime_copy(runtime, source, strlen(source)) : NULL;
                    if (source && !runtime->error_source)
                        runtime->error_source = "OpenASP";
                    return oasp_value_empty();
                }
            }
            oasp_value receiver = oasp_runtime_eval(runtime, oasp_ast_expression_left(oasp_ast_expression_left(expr)));
            if (receiver.kind == OASP_VALUE_OBJECT) {
                oasp_resolve_persisted_class(runtime, receiver.as.object);
                oasp_routine_ref method =
                    oasp_find_method(receiver.as.object, oasp_ast_expression_name(oasp_ast_expression_left(expr)), false);
                if (method.bytecode)
                    return oasp_invoke_routine(runtime, method, receiver.as.object, oasp_ast_expression_arguments(runtime->scratch, expr), oasp_ast_expression_arg_count(expr));
                if (receiver.as.object->component_kind != OASP_COMPONENT_NONE) {
                    oasp_value *arguments = oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, expr), oasp_ast_expression_arg_count(expr));
                    oasp_value result = oasp_value_empty();
                    if ((!oasp_ast_expression_arg_count(expr) || arguments) &&
                        oasp_component_call(runtime, receiver.as.object, oasp_ast_expression_name(oasp_ast_expression_left(expr)), arguments, oasp_ast_expression_arg_count(expr), oasp_ast_expression_line(expr), &result))
                        return result;
                }
                oasp_binding *field =
                    oasp_binding_find(receiver.as.object->fields, oasp_ast_expression_name(oasp_ast_expression_left(expr)));
                if (field && (field->value.kind == OASP_VALUE_ARRAY || field->value.kind == OASP_VALUE_OBJECT)) {
                    oasp_value *arguments =
                        oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, expr),
                                                    oasp_ast_expression_arg_count(expr));
                    if (!oasp_ast_expression_arg_count(expr) || arguments)
                        return oasp_runtime_call_default(runtime, &field->value, arguments,
                                                         oasp_ast_expression_arguments(runtime->scratch, expr),
                                                         oasp_ast_expression_arg_count(expr),
                                                         oasp_ast_expression_line(expr));
                }
            }
            oasp_runtime_error(runtime, 438, oasp_runtime_named_error(runtime, "unknown function or object member: ", oasp_ast_expression_name(oasp_ast_expression_left(expr))), oasp_ast_expression_line(expr));
            return oasp_value_empty();
        }
        if (!oasp_ast_expression_left(expr) || oasp_ast_expression_kind(oasp_ast_expression_left(expr)) != OASP_EXPR_IDENTIFIER) {
            oasp_value callee = oasp_ast_expression_left(expr) ? oasp_runtime_eval(runtime, oasp_ast_expression_left(expr)) : oasp_value_empty();
            oasp_value *arguments = oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, expr), oasp_ast_expression_arg_count(expr));
            if (callee.kind == OASP_VALUE_OBJECT) {
                oasp_value result = oasp_value_empty();
                if ((!oasp_ast_expression_arg_count(expr) || arguments) && oasp_component_default(runtime, callee.as.object, arguments, oasp_ast_expression_arg_count(expr), oasp_ast_expression_line(expr), &result))
                    return result;
            }
            if (callee.kind == OASP_VALUE_ARRAY) {
                size_t offset = 0;
                return oasp_array_offset(runtime, callee.as.array, arguments, oasp_ast_expression_arg_count(expr), oasp_ast_expression_line(expr), &offset) ? callee.as.array->items[offset]
                                                                                                                    : oasp_value_empty();
            }
            oasp_runtime_error(runtime, 438, "object does not support call", oasp_ast_expression_line(expr));
            return oasp_value_empty();
        }
        oasp_slice call_name = oasp_ast_expression_name(oasp_ast_expression_left(expr));
        if (call_name.length > 1 && call_name.data[0] == '.' && runtime->with_object) {
            oasp_slice member = {call_name.data + 1, call_name.length - 1};
            oasp_resolve_persisted_class(runtime, runtime->with_object);
            oasp_routine_ref method = oasp_find_method(runtime->with_object, member, false);
            if (method.bytecode)
                return oasp_invoke_routine(runtime, method, runtime->with_object, oasp_ast_expression_arguments(runtime->scratch, expr), oasp_ast_expression_arg_count(expr));
            if (runtime->with_object->component_kind != OASP_COMPONENT_NONE) {
                oasp_value *arguments = oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, expr), oasp_ast_expression_arg_count(expr));
                oasp_value result = oasp_value_empty();
                if ((!oasp_ast_expression_arg_count(expr) || arguments) &&
                    oasp_component_call(runtime, runtime->with_object, member, arguments, oasp_ast_expression_arg_count(expr), oasp_ast_expression_line(expr), &result))
                    return result;
            }
        }
        if (runtime->frame && runtime->frame->object) {
            oasp_resolve_persisted_class(runtime, runtime->frame->object);
            oasp_routine_ref method = oasp_find_method(runtime->frame->object, call_name, false);
            if (method.bytecode)
                return oasp_invoke_routine(runtime, method, runtime->frame->object, oasp_ast_expression_arguments(runtime->scratch, expr), oasp_ast_expression_arg_count(expr));
        }
        oasp_routine_def *routine = oasp_find_routine(runtime, call_name);
        if (routine)
            return oasp_invoke_routine(runtime, (oasp_routine_ref){routine->bytecode, routine->routine_index}, NULL,
                                       oasp_ast_expression_arguments(runtime->scratch, expr),
                                       oasp_ast_expression_arg_count(expr));

        oasp_slice base;
        oasp_slice member;
        if (oasp_split_member(call_name, &base, &member)) {
            oasp_object *receiver_object = NULL;
            oasp_slice path_member;
            if (oasp_resolve_object_path(runtime, call_name, oasp_ast_expression_line(expr), &receiver_object, &path_member)) {
                oasp_routine_ref method = oasp_find_method(receiver_object, path_member, false);
                if (method.bytecode)
                    return oasp_invoke_routine(runtime, method, receiver_object, oasp_ast_expression_arguments(runtime->scratch, expr), oasp_ast_expression_arg_count(expr));
                if (receiver_object->component_kind != OASP_COMPONENT_NONE) {
                    oasp_value *arguments = oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, expr), oasp_ast_expression_arg_count(expr));
                    oasp_value result = oasp_value_empty();
                    if ((!oasp_ast_expression_arg_count(expr) || arguments) &&
                        oasp_component_call(runtime, receiver_object, path_member, arguments, oasp_ast_expression_arg_count(expr), oasp_ast_expression_line(expr), &result))
                        return result;
                } else {
                    oasp_binding *field = oasp_binding_find(receiver_object->fields, path_member);
                    oasp_value *arguments = oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, expr), oasp_ast_expression_arg_count(expr));
                    if (field && field->value.kind == OASP_VALUE_ARRAY) {
                        size_t offset = 0;
                        return oasp_array_offset(runtime, field->value.as.array, arguments, oasp_ast_expression_arg_count(expr), oasp_ast_expression_line(expr), &offset)
                                   ? field->value.as.array->items[offset]
                                   : oasp_value_empty();
                    }
                    if (field && field->value.kind == OASP_VALUE_OBJECT) {
                        oasp_value result = oasp_value_empty();
                        if (oasp_component_default(runtime, field->value.as.object, arguments, oasp_ast_expression_arg_count(expr), oasp_ast_expression_line(expr), &result))
                            return result;
                    }
                }
            }
            const oasp_value *receiver = oasp_runtime_get(runtime, base);
            bool host_builtin = oasp_ascii_equal(base, "request") || oasp_ascii_equal(base, "response") || oasp_ascii_equal(base, "server") ||
                                oasp_ascii_equal(base, "session") || oasp_ascii_equal(base, "application") || oasp_ascii_equal(base, "err");
            if ((!receiver || receiver->kind != OASP_VALUE_OBJECT) && !host_builtin) {
                oasp_runtime_error(runtime, receiver && receiver->kind == OASP_VALUE_NOTHING ? 91 : 424,
                                   receiver && receiver->kind == OASP_VALUE_NOTHING ? "object variable not set" : "object required", oasp_ast_expression_line(expr));
                return oasp_value_empty();
            }
        }
        const oasp_value *callee = oasp_runtime_get(runtime, call_name);
        if (callee && callee->kind == OASP_VALUE_OBJECT) {
            oasp_routine_ref method = oasp_find_method(callee->as.object, call_name, true);
            if (method.bytecode)
                return oasp_invoke_routine(runtime, method, callee->as.object, oasp_ast_expression_arguments(runtime->scratch, expr), oasp_ast_expression_arg_count(expr));
            if (callee->as.object->component_kind != OASP_COMPONENT_NONE) {
                oasp_value *arguments = oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, expr), oasp_ast_expression_arg_count(expr));
                oasp_value result = oasp_value_empty();
                if ((!oasp_ast_expression_arg_count(expr) || arguments) && oasp_component_default(runtime, callee->as.object, arguments, oasp_ast_expression_arg_count(expr), oasp_ast_expression_line(expr), &result))
                    return result;
            }
        }
        oasp_value *args = oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, expr), oasp_ast_expression_arg_count(expr));
        if (oasp_ast_expression_arg_count(expr) && !args)
            return oasp_value_empty();
        if (callee && callee->kind == OASP_VALUE_ARRAY) {
            size_t offset = 0;
            return oasp_array_offset(runtime, callee->as.array, args, oasp_ast_expression_arg_count(expr), oasp_ast_expression_line(expr), &offset) ? callee->as.array->items[offset]
                                                                                                            : oasp_value_empty();
        }
        return oasp_builtin_call(runtime, OASP_BUILTIN_DYNAMIC, call_name, args,
                                 oasp_ast_expression_arg_count(expr), oasp_ast_expression_line(expr));
    }
    if (oasp_ast_expression_kind(expr) == OASP_EXPR_NEGATE || oasp_ast_expression_kind(expr) == OASP_EXPR_POSITIVE || oasp_ast_expression_kind(expr) == OASP_EXPR_NOT) {
        oasp_value operand = oasp_runtime_eval(runtime, oasp_ast_expression_left(expr));
        if (runtime->error_pending)
            return oasp_value_empty();
        return oasp_runtime_apply_unary(runtime, oasp_ast_expression_kind(expr), &operand, oasp_ast_expression_line(expr));
    }
    oasp_value left = oasp_runtime_eval(runtime, oasp_ast_expression_left(expr));
    oasp_value right = oasp_runtime_eval(runtime, oasp_ast_expression_right(expr));
    if (runtime->error_pending && !runtime->resume_next)
        return oasp_value_empty();
    return oasp_runtime_apply_binary(runtime, oasp_ast_expression_kind(expr), &left, &right, oasp_ast_expression_line(expr));
}

static bool oasp_object_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value) {
    oasp_resolve_persisted_class(runtime, object);
    if (oasp_component_set(runtime, object, member, value, 0))
        return true;
    oasp_routine_ref property = oasp_find_assignment_accessor(object, member, value);
    if (property.bytecode) {
        oasp_expr argument;
        memset(&argument, 0, sizeof(argument));
        argument.kind = OASP_EXPR_LITERAL;
        argument.literal = *value;
        oasp_expr *arguments[] = {&argument};
        (void)oasp_invoke_routine(runtime, property, object, arguments, 1);
        return !runtime->error_number || runtime->resume_next;
    }
    oasp_binding *binding = oasp_binding_find(object->fields, member);
    if (!binding) {
        binding = oasp_runtime_alloc_binding(runtime);
        if (!binding)
            return false;
        memset(binding, 0, sizeof(*binding));
        binding->name.data = oasp_runtime_lower(runtime, member.data, member.length);
        if (!binding->name.data)
            return false;
        binding->name.length = member.length;
        binding->next = object->fields;
        object->fields = binding;
        if (runtime->allocator)
            oasp_allocator_write_barrier(runtime->allocator, object, binding);
    }
    if (binding->constant) {
        oasp_runtime_error(runtime, 500, "cannot assign to constant", 0);
        return false;
    }
    binding->value = oasp_runtime_value_copy(runtime, value, false);
    oasp_runtime_write_barrier(runtime, binding, &binding->value);
    return true;
}

static bool oasp_assign_target(oasp_runtime *runtime, const oasp_expr *target, const oasp_value *value, size_t line) {
    if (!target)
        return false;
    if (oasp_ast_expression_kind(target) == OASP_EXPR_IDENTIFIER) {
        if (oasp_ascii_equal(oasp_ast_expression_name(target), "session.timeout")) {
            int64_t timeout = 0;
            if (!oasp_value_to_int(value, &timeout) || timeout <= 0 || timeout > INT_MAX) {
                oasp_runtime_error(runtime, 5, "invalid session timeout", line);
                return false;
            }
            runtime->session_timeout = (int)timeout;
            return true;
        }
        if (oasp_ascii_equal(oasp_ast_expression_name(target), "response.buffer")) {
            if (oasp_request_set_buffered(runtime->request, oasp_value_truthy(value))) {
                oasp_runtime_error(runtime, 5, oasp_request_error(runtime->request), line);
                return false;
            }
            return true;
        }
        if (oasp_ascii_equal(oasp_ast_expression_name(target), "response.codepage")) {
            int64_t codepage = 0;
            if (!oasp_value_to_int(value, &codepage) || codepage <= 0 || codepage > INT_MAX ||
                oasp_request_set_codepage(runtime->request, (int)codepage)) {
                oasp_runtime_error(runtime, 5, "unsupported response code page", line);
                return false;
            }
            return true;
        }
        if (oasp_ascii_equal(oasp_ast_expression_name(target), "response.status") ||
            oasp_ascii_equal(oasp_ast_expression_name(target), "response.contenttype") ||
            oasp_ascii_equal(oasp_ast_expression_name(target), "response.charset")) {
            char *text = oasp_value_format(runtime->format_arena, value);
            int result = oasp_ascii_equal(oasp_ast_expression_name(target), "response.status")
                             ? oasp_request_set_status(runtime->request, text)
                             : oasp_ascii_equal(oasp_ast_expression_name(target), "response.charset")
                                   ? oasp_request_set_charset(runtime->request, text)
                                   : oasp_request_set_content_type(runtime->request, text);
            if (!text || result) {
                oasp_runtime_error(runtime, oasp_request_failed(runtime->request) ? 5 : 7,
                                   oasp_request_failed(runtime->request)
                                       ? oasp_request_error(runtime->request)
                                       : "out of memory", line);
                return false;
            }
            return true;
        }
        if (oasp_ast_expression_name(target).length > 1 && oasp_ast_expression_name(target).data[0] == '.' && runtime->with_object) {
            oasp_slice member = {oasp_ast_expression_name(target).data + 1, oasp_ast_expression_name(target).length - 1};
            return oasp_object_set(runtime, runtime->with_object, member, value);
        }
        oasp_slice base;
        oasp_slice member;
        if (oasp_split_member(oasp_ast_expression_name(target), &base, &member)) {
            const oasp_value *receiver = oasp_runtime_get(runtime, base);
            if (receiver && receiver->kind == OASP_VALUE_OBJECT)
                return oasp_object_set(runtime, receiver->as.object, member, value);
        }
        if (runtime->frame && runtime->frame->object &&
            !oasp_binding_find(runtime->frame->locals, oasp_ast_expression_name(target)) &&
            !oasp_frame_find_slot(runtime->frame, oasp_ast_expression_name(target))) {
            oasp_routine_ref property =
                oasp_find_assignment_accessor(runtime->frame->object, oasp_ast_expression_name(target), value);
            if (property.bytecode)
                return oasp_object_set(runtime, runtime->frame->object, oasp_ast_expression_name(target), value);
        }
        return oasp_runtime_set(runtime, oasp_ast_expression_name(target), value, false);
    }
    if (oasp_ast_expression_kind(target) == OASP_EXPR_MEMBER) {
        if (oasp_ast_expression_left(target) && oasp_ast_expression_kind(oasp_ast_expression_left(target)) == OASP_EXPR_CALL && oasp_ast_expression_left(oasp_ast_expression_left(target)) && oasp_ast_expression_kind(oasp_ast_expression_left(oasp_ast_expression_left(target))) == OASP_EXPR_IDENTIFIER &&
            oasp_ascii_equal(oasp_ast_expression_name(oasp_ast_expression_left(oasp_ast_expression_left(target))), "response.cookies")) {
            const oasp_expr *cookie_call = oasp_ast_expression_left(target);
            if (oasp_ast_expression_arg_count(cookie_call) != 1) {
                oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
                return false;
            }
            oasp_value key_value = oasp_runtime_eval(runtime, oasp_ast_expression_arg(cookie_call, 0));
            char *key = oasp_value_format(runtime->format_arena, &key_value);
            char *text = oasp_value_format(runtime->format_arena, value);
            char *attribute = oasp_runtime_copy(runtime, oasp_ast_expression_name(target).data, oasp_ast_expression_name(target).length);
            if (!key || !text || !attribute || oasp_request_set_cookie_attribute(runtime->request, key, attribute, text)) {
                oasp_runtime_error(runtime, 5, oasp_request_failed(runtime->request)
                                                   ? oasp_request_error(runtime->request)
                                                   : "invalid response cookie attribute", line);
                return false;
            }
            return true;
        }
        oasp_value receiver = oasp_runtime_eval(runtime, oasp_ast_expression_left(target));
        if (receiver.kind == OASP_VALUE_OBJECT)
            return oasp_object_set(runtime, receiver.as.object, oasp_ast_expression_name(target), value);
        oasp_runtime_error(runtime, 424, "object required", line);
        return false;
    }
    if (oasp_ast_expression_kind(target) == OASP_EXPR_CALL && oasp_ast_expression_left(target) &&
        oasp_ast_expression_kind(oasp_ast_expression_left(target)) == OASP_EXPR_CALL &&
        oasp_ast_expression_left(oasp_ast_expression_left(target)) &&
        oasp_ast_expression_kind(oasp_ast_expression_left(oasp_ast_expression_left(target))) == OASP_EXPR_IDENTIFIER &&
        oasp_ascii_equal(oasp_ast_expression_name(oasp_ast_expression_left(oasp_ast_expression_left(target))), "response.cookies") &&
        oasp_ast_expression_arg_count(oasp_ast_expression_left(target)) == 1 &&
        oasp_ast_expression_arg_count(target) == 1) {
        oasp_value key_value = oasp_runtime_eval(runtime, oasp_ast_expression_arg(oasp_ast_expression_left(target), 0));
        oasp_value subkey_value = oasp_runtime_eval(runtime, oasp_ast_expression_arg(target, 0));
        char *key = oasp_value_format(runtime->format_arena, &key_value);
        char *subkey = oasp_value_format(runtime->format_arena, &subkey_value);
        char *text = oasp_value_format(runtime->format_arena, value);
        if (!key || !subkey || !text || oasp_request_set_cookie_subvalue(runtime->request, key, subkey, text)) {
            oasp_runtime_error(runtime, 5, oasp_request_failed(runtime->request)
                                               ? oasp_request_error(runtime->request)
                                               : "invalid response cookie", line);
            return false;
        }
        return true;
    }
    if (oasp_ast_expression_kind(target) == OASP_EXPR_CALL && oasp_ast_expression_left(target) && oasp_ast_expression_kind(oasp_ast_expression_left(target)) == OASP_EXPR_MEMBER) {
        oasp_value receiver = oasp_runtime_eval(runtime, oasp_ast_expression_left(oasp_ast_expression_left(target)));
        if (receiver.kind != OASP_VALUE_OBJECT) {
            oasp_runtime_error(runtime, 424, "object required", line);
            return false;
        }
        oasp_object *object = receiver.as.object;
        oasp_resolve_persisted_class(runtime, object);
        oasp_routine_ref property =
            oasp_find_assignment_accessor(object, oasp_ast_expression_name(oasp_ast_expression_left(target)), value);
        if (property.bytecode) {
            size_t count = oasp_ast_expression_arg_count(target) + 1;
            oasp_expr **arguments = oasp_arena_alloc(runtime->scratch, count * sizeof(*arguments), _Alignof(oasp_expr *));
            oasp_expr *literal = oasp_arena_alloc(runtime->scratch, sizeof(*literal), _Alignof(oasp_expr));
            if (!arguments || !literal)
                return false;
            if (oasp_ast_expression_arg_count(target))
                memcpy(arguments, oasp_ast_expression_arguments(runtime->scratch, target), oasp_ast_expression_arg_count(target) * sizeof(*arguments));
            memset(literal, 0, sizeof(*literal));
            literal->kind = OASP_EXPR_LITERAL;
            literal->literal = *value;
            arguments[oasp_ast_expression_arg_count(target)] = literal;
            (void)oasp_invoke_routine(runtime, property, object, arguments, count);
            return !runtime->error_number || runtime->resume_next;
        }
        oasp_binding *field = oasp_binding_find(object->fields, oasp_ast_expression_name(oasp_ast_expression_left(target)));
        if (field && field->value.kind == OASP_VALUE_ARRAY) {
            oasp_value *indices = oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, target), oasp_ast_expression_arg_count(target));
            size_t offset = 0;
            if ((oasp_ast_expression_arg_count(target) && !indices) || !oasp_array_offset(runtime, field->value.as.array, indices, oasp_ast_expression_arg_count(target), line, &offset))
                return false;
            field->value.as.array->items[offset] = oasp_runtime_value_copy(runtime, value, false);
            oasp_runtime_write_barrier(runtime, field->value.as.array,
                                       &field->value.as.array->items[offset]);
            return true;
        }
        if (object->component_kind != OASP_COMPONENT_NONE) {
            oasp_value *arguments = oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, target), oasp_ast_expression_arg_count(target));
            if (oasp_ast_expression_arg_count(target) && !arguments)
                return false;
            return oasp_component_assign(runtime, object, oasp_ast_expression_name(oasp_ast_expression_left(target)), arguments, oasp_ast_expression_arg_count(target), value, line);
        }
    }
    if (oasp_ast_expression_kind(target) == OASP_EXPR_CALL && oasp_ast_expression_left(target) && oasp_ast_expression_kind(oasp_ast_expression_left(target)) == OASP_EXPR_IDENTIFIER) {
        oasp_slice name = oasp_ast_expression_name(oasp_ast_expression_left(target));
        if (oasp_ascii_equal(name, "response.cookies.subkey")) {
            if (oasp_ast_expression_arg_count(target) != 2) {
                oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
                return false;
            }
            oasp_value key_value = oasp_runtime_eval(runtime, oasp_ast_expression_arg(target, 0));
            oasp_value subkey_value = oasp_runtime_eval(runtime, oasp_ast_expression_arg(target, 1));
            char *key = oasp_value_format(runtime->format_arena, &key_value);
            char *subkey = oasp_value_format(runtime->format_arena, &subkey_value);
            char *text = oasp_value_format(runtime->format_arena, value);
            if (!key || !subkey || !text || oasp_request_set_cookie_subvalue(runtime->request, key, subkey, text)) {
                oasp_runtime_error(runtime, 5, oasp_request_failed(runtime->request)
                                                   ? oasp_request_error(runtime->request)
                                                   : "invalid response cookie", line);
                return false;
            }
            return true;
        }
        if (oasp_ascii_equal(name, "response.cookies")) {
            if (oasp_ast_expression_arg_count(target) != 1) {
                oasp_runtime_error(runtime, 450, "wrong number of arguments", line);
                return false;
            }
            oasp_value key_value = oasp_runtime_eval(runtime, oasp_ast_expression_arg(target, 0));
            char *key = oasp_value_format(runtime->format_arena, &key_value);
            char *text = oasp_value_format(runtime->format_arena, value);
            if (!key || !text || oasp_request_set_cookie(runtime->request, key, text)) {
                oasp_runtime_error(runtime, 5, oasp_request_failed(runtime->request)
                                                   ? oasp_request_error(runtime->request)
                                                   : "invalid response cookie", line);
                return false;
            }
            return true;
        }
        if (oasp_ast_expression_arg_count(target) == 1 && (oasp_ascii_equal(name, "session") || oasp_ascii_equal(name, "application"))) {
            oasp_value key_value = oasp_runtime_eval(runtime, oasp_ast_expression_arg(target, 0));
            char *key = oasp_value_format(runtime->format_arena, &key_value);
            oasp_binding **store = oasp_ascii_equal(name, "session") ? &runtime->session : &runtime->application;
            return key && oasp_store_set(runtime, store, key, value);
        }
        oasp_binding *binding = oasp_runtime_find_binding(runtime, name);
        if (binding && binding->value.kind == OASP_VALUE_ARRAY) {
            oasp_value *indices = oasp_runtime_alloc(runtime, oasp_ast_expression_arg_count(target) * sizeof(*indices), _Alignof(oasp_value));
            if (!indices)
                return false;
            for (size_t i = 0; i < oasp_ast_expression_arg_count(target); i++)
                indices[i] = oasp_runtime_eval(runtime, oasp_ast_expression_arg(target, i));
            size_t offset = 0;
            if (!oasp_array_offset(runtime, binding->value.as.array, indices, oasp_ast_expression_arg_count(target), line, &offset))
                return false;
            binding->value.as.array->items[offset] = oasp_runtime_value_copy(runtime, value, false);
            oasp_runtime_write_barrier(runtime, binding->value.as.array,
                                       &binding->value.as.array->items[offset]);
            return true;
        }
        oasp_slice base;
        oasp_slice member = {"", 0};
        oasp_object *receiver_object = NULL;
        if (oasp_split_member(name, &base, &member))
            (void)oasp_resolve_object_path(runtime, name, line, &receiver_object, &member);
        else {
            const oasp_value *receiver = oasp_runtime_get(runtime, name);
            if (receiver && receiver->kind == OASP_VALUE_OBJECT)
                receiver_object = receiver->as.object;
        }
        if (receiver_object && member.length) {
            oasp_resolve_persisted_class(runtime, receiver_object);
            oasp_routine_ref property =
                oasp_find_assignment_accessor(receiver_object, member, value);
            if (property.bytecode) {
                size_t count = oasp_ast_expression_arg_count(target) + 1;
                oasp_expr **arguments = oasp_arena_alloc(runtime->scratch, count * sizeof(*arguments), _Alignof(oasp_expr *));
                oasp_expr *literal = oasp_arena_alloc(runtime->scratch, sizeof(*literal), _Alignof(oasp_expr));
                if (!arguments || !literal)
                    return false;
                if (oasp_ast_expression_arg_count(target))
                    memcpy(arguments, oasp_ast_expression_arguments(runtime->scratch, target), oasp_ast_expression_arg_count(target) * sizeof(*arguments));
                memset(literal, 0, sizeof(*literal));
                literal->kind = OASP_EXPR_LITERAL;
                literal->literal = *value;
                arguments[oasp_ast_expression_arg_count(target)] = literal;
                (void)oasp_invoke_routine(runtime, property, receiver_object, arguments, count);
                return !runtime->error_number || runtime->resume_next;
            }
            oasp_binding *field = oasp_binding_find(receiver_object->fields, member);
            if (field && field->value.kind == OASP_VALUE_ARRAY) {
                oasp_value *indices = oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, target), oasp_ast_expression_arg_count(target));
                size_t offset = 0;
                if ((oasp_ast_expression_arg_count(target) && !indices) || !oasp_array_offset(runtime, field->value.as.array, indices, oasp_ast_expression_arg_count(target), line, &offset))
                    return false;
                field->value.as.array->items[offset] = oasp_runtime_value_copy(runtime, value, false);
                oasp_runtime_write_barrier(runtime, field->value.as.array,
                                           &field->value.as.array->items[offset]);
                return true;
            }
        }
        if (receiver_object && receiver_object->component_kind != OASP_COMPONENT_NONE) {
            oasp_value *arguments = oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, target), oasp_ast_expression_arg_count(target));
            if (oasp_ast_expression_arg_count(target) && !arguments)
                return false;
            return oasp_component_assign(runtime, receiver_object, member, arguments, oasp_ast_expression_arg_count(target), value, line);
        }
    }
    if (oasp_ast_expression_kind(target) == OASP_EXPR_CALL && oasp_ast_expression_left(target)) {
        oasp_value receiver = oasp_runtime_eval(runtime, oasp_ast_expression_left(target));
        size_t argument_count = oasp_ast_expression_arg_count(target);
        if (runtime->error_pending && !runtime->resume_next)
            return false;
        if (receiver.kind == OASP_VALUE_ARRAY) {
            oasp_value *indices =
                oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, target),
                                            argument_count);
            size_t offset = 0;
            if ((argument_count && !indices) ||
                !oasp_array_offset(runtime, receiver.as.array, indices, argument_count, line, &offset))
                return false;
            receiver.as.array->items[offset] = oasp_runtime_value_copy(runtime, value, false);
            oasp_runtime_write_barrier(runtime, receiver.as.array,
                                       &receiver.as.array->items[offset]);
            return true;
        }
        if (receiver.kind == OASP_VALUE_OBJECT) {
            oasp_object *object = receiver.as.object;
            oasp_resolve_persisted_class(runtime, object);
            oasp_routine_ref property = oasp_find_default_assignment_accessor(object, value);
            if (property.bytecode) {
                size_t count = argument_count + 1;
                oasp_expr **arguments =
                    oasp_arena_alloc(runtime->scratch, count * sizeof(*arguments), _Alignof(oasp_expr *));
                oasp_expr *literal = oasp_arena_alloc(runtime->scratch, sizeof(*literal), _Alignof(oasp_expr));
                if (!arguments || !literal)
                    return false;
                if (argument_count)
                    memcpy(arguments, oasp_ast_expression_arguments(runtime->scratch, target),
                           argument_count * sizeof(*arguments));
                memset(literal, 0, sizeof(*literal));
                literal->kind = OASP_EXPR_LITERAL;
                literal->literal = *value;
                arguments[argument_count] = literal;
                (void)oasp_invoke_routine(runtime, property, object, arguments, count);
                return !runtime->error_number || runtime->resume_next;
            }
            if (object->component_kind != OASP_COMPONENT_NONE) {
                oasp_value *indices =
                    oasp_runtime_eval_arguments(runtime, oasp_ast_expression_arguments(runtime->scratch, target),
                                                argument_count);
                if (argument_count && !indices)
                    return false;
                return oasp_component_assign(runtime, object, (oasp_slice){0}, indices, argument_count,
                                             value, line);
            }
        }
        oasp_runtime_error(runtime, receiver.kind == OASP_VALUE_NOTHING ? 91 : 424,
                           receiver.kind == OASP_VALUE_NOTHING ? "object variable not set"
                                                               : "assignment target is not indexable",
                           line);
        return false;
    }
    oasp_runtime_error(runtime, 424, "assignment target is not supported", line);
    return false;
}

bool oasp_runtime_assign_value(oasp_runtime *runtime, const oasp_expr *target, const oasp_value *value, size_t line) {
    return oasp_assign_target(runtime, target, value, line);
}

/*
 * AST/bytecode execution orchestration: walks statements, handles structured
 * control flow, registers definitions, prepares globals, and enters the VM.
 */
static bool oasp_runtime_execute_range(oasp_runtime *runtime, const oasp_stmt *first, const oasp_stmt *last) {
    for (const oasp_stmt *stmt = first; stmt != last && stmt && !runtime->stopped && !runtime->loop_exit && !(runtime->frame && runtime->frame->exit_routine);
         stmt = oasp_ast_statement_next(stmt)) {
        if (!oasp_runtime_safepoint(runtime, oasp_ast_statement_line(stmt)))
            return false;
        switch (oasp_ast_statement_kind(stmt)) {
        case OASP_STMT_OPTION_EXPLICIT:
            runtime->option_explicit = true;
            break;
        case OASP_STMT_ON_ERROR_RESUME_NEXT:
            runtime->resume_next = true;
            break;
        case OASP_STMT_ON_ERROR_GOTO_0:
            runtime->resume_next = false;
            oasp_runtime_clear_error(runtime);
            break;
        case OASP_STMT_NOOP:
            break;
        case OASP_STMT_DIM:
        case OASP_STMT_REDIM:
            for (size_t i = 0; i < oasp_ast_statement_declaration_count(stmt); i++) {
                const oasp_value *previous = oasp_ast_statement_preserve(stmt) ? oasp_runtime_get(runtime, oasp_ast_statement_declaration_name(stmt, i)) : NULL;
                oasp_value value = oasp_array_create(runtime, stmt, i, oasp_ast_statement_line(stmt));
                if (runtime->error_pending && !runtime->resume_next)
                    return false;
                if (oasp_ast_statement_preserve(stmt) && previous && previous->kind == OASP_VALUE_ARRAY &&
                    !oasp_array_preserve(runtime, value.as.array, previous->as.array, oasp_ast_statement_line(stmt)))
                    return false;
                if (oasp_ast_statement_preserve(stmt) && previous && previous->kind != OASP_VALUE_EMPTY && previous->kind != OASP_VALUE_ARRAY) {
                    oasp_runtime_error(runtime, 13, "type mismatch", oasp_ast_statement_line(stmt));
                    return false;
                }
                bool ok = oasp_ast_statement_kind(stmt) == OASP_STMT_DIM ? oasp_runtime_declare(runtime, oasp_ast_statement_declaration_name(stmt, i), &value, false)
                                                      : oasp_runtime_set(runtime, oasp_ast_statement_declaration_name(stmt, i), &value, false);
                if (!ok)
                    return false;
            }
            break;
        case OASP_STMT_CONST: {
            for (size_t i = 0; i < oasp_ast_statement_constant_count(stmt); i++) {
                oasp_value value = oasp_runtime_eval(runtime, oasp_ast_statement_constant_value(stmt, i));
                if (!oasp_runtime_declare(runtime, oasp_ast_statement_constant_name(stmt, i), &value, true))
                    return false;
            }
            break;
        }
        case OASP_STMT_ERASE:
            for (size_t i = 0; i < oasp_ast_statement_expression_count(stmt); i++) {
                oasp_value empty = oasp_value_empty();
                if (!oasp_assign_target(runtime, oasp_ast_statement_expression(stmt, i), &empty, oasp_ast_statement_line(stmt)))
                    return false;
            }
            break;
        case OASP_STMT_ROUTINE:
            break;
        case OASP_STMT_CLASS:
            break;
        case OASP_STMT_ASSIGN:
        case OASP_STMT_SET_ASSIGN: {
            uint64_t error_generation = runtime->error_generation;
            oasp_value value = oasp_runtime_eval(runtime, oasp_ast_statement_value(stmt));
            if (runtime->error_generation == error_generation)
                (void)oasp_assign_target(runtime, oasp_ast_statement_target(stmt), &value, oasp_ast_statement_line(stmt));
            break;
        }
        case OASP_STMT_CALL:
            (void)oasp_runtime_eval(runtime, oasp_ast_statement_value(stmt));
            break;
        case OASP_STMT_IF: {
            uint64_t error_generation = runtime->error_generation;
            oasp_value condition = oasp_runtime_eval(runtime, oasp_ast_statement_condition(stmt));
            if (runtime->error_generation != error_generation)
                break;
            const oasp_stmt *branch =
                oasp_value_truthy(&condition) ? oasp_ast_statement_body_first(stmt) : oasp_ast_statement_alternative_first(stmt);
            if (!oasp_runtime_execute_range(runtime, branch, NULL))
                return false;
            break;
        }
        case OASP_STMT_WHILE: {
            for (;;) {
                if (!oasp_runtime_safepoint(runtime, oasp_ast_statement_line(stmt)))
                    return false;
                uint64_t error_generation = runtime->error_generation;
                oasp_value condition = oasp_runtime_eval(runtime, oasp_ast_statement_condition(stmt));
                if (runtime->error_generation != error_generation)
                    break;
                if (!oasp_value_truthy(&condition))
                    break;
                if (!oasp_runtime_execute_range(runtime, oasp_ast_statement_body_first(stmt), NULL))
                    return false;
                if (runtime->loop_exit == 2) {
                    runtime->loop_exit = 0;
                    break;
                }
                if (runtime->stopped || (runtime->frame && runtime->frame->exit_routine))
                    break;
            }
            break;
        }
        case OASP_STMT_DO: {
            for (;;) {
                if (!oasp_runtime_safepoint(runtime, oasp_ast_statement_line(stmt)))
                    return false;
                if (!oasp_ast_statement_post_condition(stmt) && oasp_ast_statement_condition(stmt)) {
                    uint64_t error_generation = runtime->error_generation;
                    oasp_value condition = oasp_runtime_eval(runtime, oasp_ast_statement_condition(stmt));
                    if (runtime->error_generation != error_generation)
                        break;
                    bool truth = oasp_value_truthy(&condition);
                    if (oasp_ast_statement_until(stmt) ? truth : !truth)
                        break;
                }
                if (!oasp_runtime_execute_range(runtime, oasp_ast_statement_body_first(stmt), NULL))
                    return false;
                if (runtime->loop_exit == 2) {
                    runtime->loop_exit = 0;
                    break;
                }
                if (runtime->stopped || (runtime->frame && runtime->frame->exit_routine))
                    break;
                if (oasp_ast_statement_post_condition(stmt) && oasp_ast_statement_condition(stmt)) {
                    uint64_t error_generation = runtime->error_generation;
                    oasp_value condition = oasp_runtime_eval(runtime, oasp_ast_statement_condition(stmt));
                    if (runtime->error_generation != error_generation)
                        break;
                    bool truth = oasp_value_truthy(&condition);
                    if (oasp_ast_statement_until(stmt) ? truth : !truth)
                        break;
                }
            }
            break;
        }
        case OASP_STMT_FOR: {
            oasp_value start_value = oasp_runtime_eval(runtime, oasp_ast_statement_value(stmt));
            oasp_value limit_value = oasp_runtime_eval(runtime, oasp_ast_statement_limit(stmt));
            oasp_value step_value = oasp_ast_statement_step(stmt) ? oasp_runtime_eval(runtime, oasp_ast_statement_step(stmt)) : oasp_value_int(1);
            double current, limit, step;
            if (!oasp_value_to_float(&start_value, &current) || !oasp_value_to_float(&limit_value, &limit) || !oasp_value_to_float(&step_value, &step) ||
                step == 0.0) {
                oasp_runtime_error(runtime, 13, "type mismatch", oasp_ast_statement_line(stmt));
                return false;
            }
            while (step > 0.0 ? current <= limit : current >= limit) {
                if (!oasp_runtime_safepoint(runtime, oasp_ast_statement_line(stmt)))
                    return false;
                oasp_value loop_value = current == (double)(int64_t)current ? oasp_value_int((int64_t)current) : oasp_value_float(current);
                if (!oasp_runtime_set(runtime, oasp_ast_statement_name(stmt), &loop_value, false) || !oasp_runtime_execute_range(runtime, oasp_ast_statement_body_first(stmt), NULL))
                    return false;
                if (runtime->loop_exit == 1) {
                    runtime->loop_exit = 0;
                    break;
                }
                if (runtime->stopped || (runtime->frame && runtime->frame->exit_routine))
                    break;
                current += step;
            }
            break;
        }
        case OASP_STMT_FOR_EACH: {
            oasp_value collection = oasp_runtime_eval(runtime, oasp_ast_statement_value(stmt));
            if (collection.kind == OASP_VALUE_OBJECT) {
                oasp_value enumerated = oasp_value_empty();
                if (oasp_component_enumerate(runtime, collection.as.object, &enumerated))
                    collection = enumerated;
            }
            if (collection.kind != OASP_VALUE_ARRAY) {
                oasp_runtime_error(runtime, 13, "type mismatch", oasp_ast_statement_line(stmt));
                return false;
            }
            for (size_t i = 0; i < collection.as.array->length; i++) {
                if (!oasp_runtime_safepoint(runtime, oasp_ast_statement_line(stmt)))
                    return false;
                if (!oasp_runtime_set(runtime, oasp_ast_statement_name(stmt), &collection.as.array->items[i], false) ||
                    !oasp_runtime_execute_range(runtime, oasp_ast_statement_body_first(stmt), NULL))
                    return false;
                if (runtime->loop_exit == 1) {
                    runtime->loop_exit = 0;
                    break;
                }
                if (runtime->stopped || (runtime->frame && runtime->frame->exit_routine))
                    break;
            }
            break;
        }
        case OASP_STMT_SELECT: {
            uint64_t error_generation = runtime->error_generation;
            oasp_value selected = oasp_runtime_eval(runtime, oasp_ast_statement_value(stmt));
            if (runtime->error_generation != error_generation)
                break;
            for (const oasp_case_branch *branch = oasp_ast_statement_cases(stmt); branch; branch = oasp_ast_case_next(branch)) {
                bool matches = oasp_ast_case_is_else(branch);
                for (size_t i = 0; i < oasp_ast_case_value_count(branch) && !matches; i++) {
                    oasp_value candidate = oasp_runtime_eval(runtime, oasp_ast_case_value(branch, i));
                    matches = oasp_compare(runtime, &selected, &candidate) == 0;
                }
                if (matches) {
                    if (!oasp_runtime_execute_range(runtime, oasp_ast_case_body_first(branch), NULL))
                        return false;
                    break;
                }
            }
            break;
        }
        case OASP_STMT_WITH: {
            oasp_value receiver = oasp_runtime_eval(runtime, oasp_ast_statement_value(stmt));
            if (receiver.kind != OASP_VALUE_OBJECT) {
                oasp_runtime_error(runtime, 424, "object required", oasp_ast_statement_line(stmt));
                return false;
            }
            oasp_object *previous = runtime->with_object;
            runtime->with_object = receiver.as.object;
            bool ok = oasp_runtime_execute_range(runtime, oasp_ast_statement_body_first(stmt), NULL);
            runtime->with_object = previous;
            if (!ok)
                return false;
            break;
        }
        case OASP_STMT_EXIT:
            if (oasp_ascii_equal(oasp_ast_statement_name(stmt), "for"))
                runtime->loop_exit = 1;
            else if (oasp_ascii_equal(oasp_ast_statement_name(stmt), "do"))
                runtime->loop_exit = 2;
            else if (runtime->frame &&
                     (oasp_ascii_equal(oasp_ast_statement_name(stmt), "function") || oasp_ascii_equal(oasp_ast_statement_name(stmt), "sub") || oasp_ascii_equal(oasp_ast_statement_name(stmt), "property")))
                runtime->frame->exit_routine = true;
            else {
                oasp_runtime_error(runtime, 500, "invalid Exit statement", oasp_ast_statement_line(stmt));
                return false;
            }
            break;
        default:
            oasp_runtime_error(runtime, 500, "statement is not implemented", oasp_ast_statement_line(stmt));
            break;
        }
        if (runtime->error_pending) {
            if (!runtime->resume_next)
                return false;
            runtime->error_pending = false;
        }
    }
    return true;
}

bool oasp_runtime_execute_statement(oasp_runtime *runtime, const oasp_stmt *statement) {
    return statement && oasp_runtime_execute_range(runtime, statement, oasp_ast_statement_next(statement));
}

bool oasp_runtime_execute_include(oasp_runtime *runtime, oasp_slice path) {
    if (!runtime || !runtime->include_handler)
        return false;
    if (runtime->include_depth >= 64) {
        oasp_runtime_error(runtime, 500, "include nesting exceeds 64 levels", 0);
        return false;
    }
    runtime->include_depth++;
    bool ok = runtime->include_handler(runtime->include_context, runtime, path);
    runtime->include_depth--;
    return ok;
}

static bool oasp_runtime_register_definitions(oasp_runtime *runtime, const oasp_bytecode *bytecode) {
    if (!runtime->frame) {
        for (size_t i = 0; i < bytecode->routine_count; i++) {
            oasp_routine_bytecode compiled;
            if (!oasp_bytecode_routine_at(bytecode, i, &compiled))
                return false;
            if (compiled.class_index == SIZE_MAX && !oasp_register_routine(runtime, bytecode, i, compiled.name))
                return false;
        }
        for (size_t i = 0; i < bytecode->class_count; i++) {
            oasp_class_bytecode compiled;
            if (!oasp_bytecode_class_at(bytecode, i, &compiled))
                return false;
            if (!oasp_find_class(runtime, compiled.name)) {
                oasp_class_def *class_def = oasp_runtime_alloc_class_def(runtime);
                if (!class_def)
                    return false;
                class_def->name = compiled.name;
                class_def->bytecode = bytecode;
                class_def->class_index = i;
                class_def->resolved = true;
                class_def->next = runtime->classes;
                runtime->classes = class_def;
            }
        }
    }
    return true;
}

bool oasp_runtime_register_bytecode_definitions(oasp_runtime *runtime, const oasp_bytecode *bytecode) {
    if (!runtime || !bytecode)
        return false;
    const oasp_bytecode *previous = runtime->bytecode;
    runtime->bytecode = bytecode;
    bool ok = oasp_runtime_register_definitions(runtime, bytecode);
    runtime->bytecode = previous;
    return ok;
}

bool oasp_runtime_prepare_bytecode_globals(oasp_runtime *runtime, const oasp_bytecode *bytecode) {
    return runtime && bytecode && oasp_runtime_prepare_global_slots(runtime, bytecode);
}

bool oasp_runtime_execute_bytecode(oasp_runtime *runtime, const oasp_bytecode *bytecode) {
    if (!runtime || !bytecode)
        return false;
    const oasp_bytecode *previous = runtime->bytecode;
    runtime->bytecode = bytecode;
    bool definitions = oasp_runtime_register_bytecode_definitions(runtime, bytecode);
    bool globals = definitions && oasp_runtime_prepare_global_slots(runtime, bytecode);
    bool executed = globals && oasp_vm_execute(runtime, bytecode);
    if (oasp_process_options.trace_vm && (!definitions || !globals || !executed))
        fprintf(stderr, "OpenASP VM setup: definitions=%d globals=%d executed=%d instructions=%zu routines=%zu classes=%zu\n",
                definitions, globals, executed, bytecode->length, bytecode->routine_count, bytecode->class_count);
    bool ok = definitions && globals && executed;
    runtime->bytecode = previous;
    return ok;
}

bool oasp_runtime_execute(oasp_runtime *runtime, const oasp_block *program) {
    if (!runtime || !program)
        return false;
    oasp_bytecode *bytecode = oasp_runtime_alloc(runtime, sizeof(*bytecode), _Alignof(oasp_bytecode));
    if (!bytecode || !oasp_bytecode_compile(runtime->arena, program, bytecode))
        return false;
    return oasp_runtime_execute_bytecode(runtime, bytecode);
}

bool oasp_runtime_execute_dynamic(oasp_runtime *runtime, const oasp_block *program) {
    if (!runtime || !program)
        return false;
    oasp_bytecode *bytecode = oasp_runtime_alloc(runtime, sizeof(*bytecode), _Alignof(oasp_bytecode));
    if (!bytecode || !oasp_bytecode_compile_module(runtime->arena, program, bytecode))
        return false;
    const oasp_bytecode *previous = runtime->bytecode;
    runtime->bytecode = bytecode;
    bool definitions = oasp_runtime_register_definitions(runtime, bytecode);
    bool globals = definitions && oasp_runtime_prepare_global_slots(runtime, bytecode);
    bool executed = globals &&
                    oasp_vm_execute_range_inline_mode(runtime, bytecode, 0, bytecode->length,
                                                      bytecode->root_error_aware || runtime->resume_next);
    runtime->bytecode = previous;
    return definitions && globals && executed;
}
