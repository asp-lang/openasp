// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ext_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return 0;
    if (now.tv_sec > INT64_MAX / 1000)
        return INT64_MAX;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void oasp_external_context_init(oasp_ext_call_context *context, oasp_runtime *runtime, oasp_external_module *module, oasp_external_object *object, oasp_ext_handle_table *handles,
                                size_t line) {
    *context = (oasp_ext_call_context){
        .runtime = runtime,
        .module = module,
        .object = object,
        .resource_owner = object,
        .handles = handles,
        .line = line,
        .deadline_milliseconds = -1
    };
    if (runtime && runtime->request) {
        int64_t remaining = oasp_request_deadline_milliseconds(runtime->request);
        if (remaining >= 0)
            oasp_external_context_set_timeout(context, (uint64_t)remaining);
    }
    if (handles) {
        handles->runtime = runtime;
        handles->owner = object;
    }
}

void oasp_external_context_set_timeout(oasp_ext_call_context *context, uint64_t timeout_milliseconds) {
    if (!context)
        return;
    int64_t now = monotonic_milliseconds();
    context->deadline_milliseconds =
        timeout_milliseconds > (uint64_t)(INT64_MAX - now) ? INT64_MAX : now + (int64_t)timeout_milliseconds;
}

bool oasp_external_context_cancelled(oasp_ext_call_context *context) {
    if (!context || context->cancelled)
        return true;
    if (context->deadline_milliseconds >= 0 &&
        monotonic_milliseconds() >= context->deadline_milliseconds) {
        context->cancelled = true;
        return true;
    }
    if (context->runtime && context->runtime->request &&
        oasp_request_deadline_expired(context->runtime->request)) {
        context->cancelled = true;
        return true;
    }
    return false;
}

int64_t oasp_external_context_deadline(oasp_ext_call_context *context) {
    if (!context)
        return 0;
    int64_t remaining = context->deadline_milliseconds < 0
                            ? -1 : context->deadline_milliseconds - monotonic_milliseconds();
    if (remaining < 0 && context->deadline_milliseconds >= 0)
        remaining = 0;
    if (context->runtime && context->runtime->request) {
        int64_t request_remaining = oasp_request_deadline_milliseconds(context->runtime->request);
        if (request_remaining >= 0 && (remaining < 0 || request_remaining < remaining))
            remaining = request_remaining;
    }
    return remaining;
}

static bool handle_valid(oasp_ext_call_context *context, const oasp_ext_value *value) {
    return context && value && value->references && value->table == context->handles && !context->handles->closing;
}

oasp_ext_value *oasp_external_value_create(oasp_ext_call_context *context, oasp_value value, oasp_ext_value_kind kind) {
    if (!context || !context->handles || context->handles->closing)
        return NULL;
    oasp_ext_value *handle = calloc(1, sizeof(*handle));
    if (!handle)
        return NULL;
    handle->value = value;
    handle->public_kind = kind;
    handle->references = 1;
    handle->table = context->handles;
    handle->next = context->handles->first;
    if (handle->next)
        handle->next->previous = handle;
    context->handles->first = handle;
    return handle;
}

static void handle_destroy(oasp_ext_value *handle) {
    if (!handle)
        return;
    if (handle->previous)
        handle->previous->next = handle->next;
    else if (handle->table)
        handle->table->first = handle->next;
    if (handle->next)
        handle->next->previous = handle->previous;
    for (size_t i = 0; i < handle->map_count; i++)
        free(handle->map_entries[i].key);
    free(handle->map_entries);
    free(handle->owned_data);
    free(handle);
}

void oasp_external_value_table_dispose(oasp_ext_handle_table *table) {
    if (!table)
        return;
    table->closing = true;
    while (table->first)
        handle_destroy(table->first);
}

static void trace_value(oasp_allocator *allocator, const oasp_value *value) {
    if (value->kind == OASP_VALUE_STRING)
        oasp_allocator_trace(allocator, value->as.string.data);
    else if (value->kind == OASP_VALUE_DATE)
        oasp_allocator_trace(allocator, value->as.date.data);
    else if (value->kind == OASP_VALUE_ARRAY)
        oasp_allocator_trace(allocator, value->as.array);
    else if (value->kind == OASP_VALUE_OBJECT)
        oasp_allocator_trace(allocator, value->as.object);
}

static oasp_ext_value_kind public_kind(const oasp_value *value) {
    switch (value->kind) {
    case OASP_VALUE_NULL: return OASP_EXT_VALUE_NULL;
    case OASP_VALUE_BOOL: return OASP_EXT_VALUE_BOOL;
    case OASP_VALUE_INT: return OASP_EXT_VALUE_I64;
    case OASP_VALUE_FLOAT: return OASP_EXT_VALUE_F64;
    case OASP_VALUE_DATE: return OASP_EXT_VALUE_DATE;
    case OASP_VALUE_STRING: return OASP_EXT_VALUE_STRING;
    case OASP_VALUE_ARRAY: return OASP_EXT_VALUE_ARRAY;
    case OASP_VALUE_OBJECT: return OASP_EXT_VALUE_OBJECT;
    default: return OASP_EXT_VALUE_EMPTY;
    }
}

void oasp_external_value_table_trace(oasp_allocator *allocator, const oasp_ext_handle_table *table) {
    if (!allocator || !table)
        return;
    for (const oasp_ext_value *handle = table->first; handle; handle = handle->next) {
        trace_value(allocator, &handle->value);
        for (size_t i = 0; i < handle->map_count; i++)
            trace_value(allocator, &handle->map_entries[i].value);
    }
}

static oasp_ext_value_kind host_kind(oasp_ext_call_context *context, const oasp_ext_value *handle) {
    return handle_valid(context, handle) ? handle->public_kind : OASP_EXT_VALUE_EMPTY;
}

static int host_get_bool(oasp_ext_call_context *context, const oasp_ext_value *handle, int *result) {
    if (!handle_valid(context, handle) || !result)
        return OASP_EXT_ERROR;
    *result = oasp_value_truthy(&handle->value);
    return OASP_EXT_OK;
}

static int host_get_i64(oasp_ext_call_context *context, const oasp_ext_value *handle, int64_t *result) {
    return handle_valid(context, handle) && result && oasp_value_to_int(&handle->value, result) ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int host_get_f64(oasp_ext_call_context *context, const oasp_ext_value *handle, double *result) {
    return handle_valid(context, handle) && result && oasp_value_to_float(&handle->value, result) ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int host_get_data(oasp_ext_call_context *context, const oasp_ext_value *handle, oasp_ext_bytes *result) {
    if (!handle_valid(context, handle) || !result)
        return OASP_EXT_ERROR;
    if (handle->public_kind == OASP_EXT_VALUE_BYTES) {
        *result = (oasp_ext_bytes){handle->owned_data, handle->owned_length};
        return OASP_EXT_OK;
    }
    if (handle->value.kind != OASP_VALUE_STRING && handle->value.kind != OASP_VALUE_DATE)
        return OASP_EXT_ERROR;
    oasp_slice slice = handle->value.kind == OASP_VALUE_STRING ? handle->value.as.string : handle->value.as.date;
    *result = (oasp_ext_bytes){slice.data, slice.length};
    return OASP_EXT_OK;
}

static oasp_ext_value *new_scalar(oasp_ext_call_context *context, oasp_value value, oasp_ext_value_kind kind) {
    return oasp_external_value_create(context, value, kind);
}

static oasp_ext_value *host_new_empty(oasp_ext_call_context *context) {
    return new_scalar(context, oasp_value_empty(), OASP_EXT_VALUE_EMPTY);
}

static oasp_ext_value *host_new_null(oasp_ext_call_context *context) {
    return new_scalar(context, oasp_value_null(), OASP_EXT_VALUE_NULL);
}

static oasp_ext_value *host_new_bool(oasp_ext_call_context *context, int value) {
    return new_scalar(context, oasp_value_bool(value != 0), OASP_EXT_VALUE_BOOL);
}

static oasp_ext_value *host_new_i64(oasp_ext_call_context *context, int64_t value) {
    return new_scalar(context, oasp_value_int(value), OASP_EXT_VALUE_I64);
}

static oasp_ext_value *host_new_f64(oasp_ext_call_context *context, double value) {
    return new_scalar(context, oasp_value_float(value), OASP_EXT_VALUE_F64);
}

static oasp_ext_value *new_text(oasp_ext_call_context *context, const void *data, size_t length, bool date) {
    if (!context || (!data && length))
        return NULL;
    char *copy = oasp_runtime_copy(context->runtime, data ? data : "", length);
    if (!copy)
        return NULL;
    oasp_value value = date ? (oasp_value){.kind = OASP_VALUE_DATE, .as.date = {copy, length}}
                            : oasp_value_string(copy, length);
    return new_scalar(context, value, date ? OASP_EXT_VALUE_DATE : OASP_EXT_VALUE_STRING);
}

static oasp_ext_value *host_new_date(oasp_ext_call_context *context, const void *data, size_t length) {
    return new_text(context, data, length, true);
}

static oasp_ext_value *host_new_string(oasp_ext_call_context *context, const void *data, size_t length) {
    return new_text(context, data, length, false);
}

static oasp_ext_value *host_new_bytes(oasp_ext_call_context *context, const void *data, size_t length) {
    if (!context || (!data && length))
        return NULL;
    oasp_ext_value *value = new_scalar(context, oasp_value_empty(), OASP_EXT_VALUE_BYTES);
    if (!value)
        return NULL;
    value->owned_data = length ? malloc(length) : NULL;
    if (length && !value->owned_data) {
        handle_destroy(value);
        return NULL;
    }
    if (length)
        memcpy(value->owned_data, data, length);
    value->owned_length = length;
    return value;
}

static bool array_length(const int64_t *bounds, size_t rank, size_t *length) {
    if (!bounds || !rank || rank > 32)
        return false;
    size_t total = 1;
    for (size_t i = 0; i < rank; i++) {
        if (bounds[i] < -1)
            return false;
        size_t dimension = bounds[i] < 0 ? 0 : (size_t)bounds[i] + 1;
        if (dimension && total > SIZE_MAX / dimension)
            return false;
        total *= dimension;
    }
    *length = total;
    return true;
}

static oasp_ext_value *host_new_array(oasp_ext_call_context *context, const int64_t *upper_bounds, size_t rank) {
    size_t length = 0;
    if (!context || !array_length(upper_bounds, rank, &length))
        return NULL;
    oasp_array *array = oasp_runtime_alloc_array(context->runtime);
    int64_t *bounds = oasp_runtime_alloc(context->runtime, rank * sizeof(*bounds), _Alignof(int64_t));
    oasp_value *items = length ? oasp_runtime_alloc_values(context->runtime, length) : NULL;
    if (!array || !bounds || (length && !items))
        return NULL;
    memcpy(bounds, upper_bounds, rank * sizeof(*bounds));
    for (size_t i = 0; i < length; i++)
        items[i] = oasp_value_empty();
    *array = (oasp_array){.rank = rank, .length = length, .bounds = bounds, .items = items};
    return new_scalar(context, (oasp_value){.kind = OASP_VALUE_ARRAY, .as.array = array}, OASP_EXT_VALUE_ARRAY);
}

static size_t host_array_rank(oasp_ext_call_context *context, const oasp_ext_value *value) {
    return handle_valid(context, value) && value->value.kind == OASP_VALUE_ARRAY && value->value.as.array
               ? value->value.as.array->rank : 0;
}

static int host_array_bounds(oasp_ext_call_context *context, const oasp_ext_value *value, int64_t *upper_bounds, size_t rank) {
    if (!handle_valid(context, value) || value->value.kind != OASP_VALUE_ARRAY || !value->value.as.array ||
        !upper_bounds || rank != value->value.as.array->rank)
        return OASP_EXT_ERROR;
    memcpy(upper_bounds, value->value.as.array->bounds, rank * sizeof(*upper_bounds));
    return OASP_EXT_OK;
}

static bool array_offset(const oasp_array *array, const int64_t *indices, size_t rank, size_t *offset) {
    if (!array || !indices || rank != array->rank)
        return false;
    size_t flat = 0;
    for (size_t i = 0; i < rank; i++) {
        if (indices[i] < 0 || indices[i] > array->bounds[i])
            return false;
        size_t dimension = array->bounds[i] < 0 ? 0 : (size_t)array->bounds[i] + 1;
        flat = flat * dimension + (size_t)indices[i];
    }
    *offset = flat;
    return flat < array->length;
}

static int host_array_get(oasp_ext_call_context *context, const oasp_ext_value *value, const int64_t *indices, size_t rank, oasp_ext_value **result) {
    size_t offset = 0;
    if (!result || !handle_valid(context, value) || value->value.kind != OASP_VALUE_ARRAY ||
        !array_offset(value->value.as.array, indices, rank, &offset))
        return OASP_EXT_ERROR;
    *result = oasp_external_value_create(context, value->value.as.array->items[offset],
                                         public_kind(&value->value.as.array->items[offset]));
    return *result ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int host_array_set(oasp_ext_call_context *context, oasp_ext_value *value, const int64_t *indices, size_t rank, const oasp_ext_value *item) {
    size_t offset = 0;
    if (!handle_valid(context, value) || !handle_valid(context, item) || value->value.kind != OASP_VALUE_ARRAY ||
        !array_offset(value->value.as.array, indices, rank, &offset))
        return OASP_EXT_ERROR;
    value->value.as.array->items[offset] = item->value;
    oasp_runtime_write_barrier(context->runtime, value->value.as.array, &item->value);
    return OASP_EXT_OK;
}

static oasp_ext_value *host_new_map(oasp_ext_call_context *context) {
    return new_scalar(context, oasp_value_empty(), OASP_EXT_VALUE_MAP);
}

static size_t host_map_size(oasp_ext_call_context *context, const oasp_ext_value *value) {
    return handle_valid(context, value) && value->public_kind == OASP_EXT_VALUE_MAP ? value->map_count : 0;
}

static ptrdiff_t map_find(const oasp_ext_value *value, const void *key, size_t key_length) {
    for (size_t i = 0; i < value->map_count; i++)
        if (value->map_entries[i].key_length == key_length && !memcmp(value->map_entries[i].key, key, key_length))
            return (ptrdiff_t)i;
    return -1;
}

static int host_map_get(oasp_ext_call_context *context, const oasp_ext_value *value, const void *key, size_t key_length, oasp_ext_value **result) {
    if (!result || !key || !handle_valid(context, value) || value->public_kind != OASP_EXT_VALUE_MAP)
        return OASP_EXT_ERROR;
    ptrdiff_t index = map_find(value, key, key_length);
    if (index < 0)
        return OASP_EXT_END;
    *result = oasp_external_value_create(context, value->map_entries[index].value,
                                         public_kind(&value->map_entries[index].value));
    return *result ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int host_map_set(oasp_ext_call_context *context, oasp_ext_value *value, const void *key, size_t key_length, const oasp_ext_value *item) {
    if (!key || !handle_valid(context, value) || !handle_valid(context, item) || value->public_kind != OASP_EXT_VALUE_MAP)
        return OASP_EXT_ERROR;
    ptrdiff_t found = map_find(value, key, key_length);
    if (found >= 0) {
        value->map_entries[found].value = item->value;
        return OASP_EXT_OK;
    }
    if (value->map_count == value->map_capacity) {
        size_t capacity = value->map_capacity ? value->map_capacity * 2 : 8;
        if (capacity < value->map_capacity || capacity > SIZE_MAX / sizeof(*value->map_entries))
            return OASP_EXT_ERROR;
        oasp_ext_map_entry *entries = realloc(value->map_entries, capacity * sizeof(*entries));
        if (!entries)
            return OASP_EXT_ERROR;
        value->map_entries = entries;
        value->map_capacity = capacity;
    }
    char *copy = malloc(key_length + 1);
    if (!copy)
        return OASP_EXT_ERROR;
    memcpy(copy, key, key_length);
    copy[key_length] = '\0';
    value->map_entries[value->map_count++] = (oasp_ext_map_entry){copy, key_length, item->value};
    return OASP_EXT_OK;
}

static int host_retain(oasp_ext_call_context *context, const oasp_ext_value *value) {
    oasp_ext_value *handle = (oasp_ext_value *)value;
    if (!handle_valid(context, handle) || handle->references == SIZE_MAX)
        return OASP_EXT_ERROR;
    handle->references++;
    return OASP_EXT_OK;
}

static void host_release(oasp_ext_call_context *context, const oasp_ext_value *value) {
    oasp_ext_value *handle = (oasp_ext_value *)value;
    if (handle_valid(context, handle) && --handle->references == 0)
        handle_destroy(handle);
}

static int object_get(oasp_ext_call_context *context, const oasp_ext_value *object, const char *member, size_t member_length, oasp_ext_value **result,
                      oasp_ext_error *error) {
    oasp_value output = oasp_value_empty();
    bool error_pending = context && context->runtime && context->runtime->error_pending;
    int error_number = context && context->runtime ? context->runtime->error_number : 0;
    if (result && member && handle_valid(context, object) && object->value.kind == OASP_VALUE_OBJECT)
        output = oasp_runtime_get_member(context->runtime, &object->value, (oasp_slice){member, member_length}, context->line);
    if (!result || !member || !handle_valid(context, object) || object->value.kind != OASP_VALUE_OBJECT ||
        (!error_pending && context->runtime->error_pending) ||
        (error_pending && context->runtime->error_number != error_number)) {
        if (error)
            *error = (oasp_ext_error){.struct_size = sizeof(*error), .code = 438, .category = OASP_EXT_ERROR_RUNTIME,
                                      .message = "object property access failed", .message_length = 29};
        return OASP_EXT_ERROR;
    }
    *result = oasp_external_value_create(context, output, public_kind(&output));
    return *result ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int object_set(oasp_ext_call_context *context, const oasp_ext_value *object, const char *member, size_t member_length, const oasp_ext_value *value,
                      oasp_ext_error *error) {
    if (!member || !handle_valid(context, object) || !handle_valid(context, value) || object->value.kind != OASP_VALUE_OBJECT ||
        !oasp_runtime_set_member(context->runtime, &object->value, (oasp_slice){member, member_length}, &value->value, context->line)) {
        if (error)
            *error = (oasp_ext_error){.struct_size = sizeof(*error), .code = 438, .category = OASP_EXT_ERROR_RUNTIME,
                                      .message = "object property assignment failed", .message_length = 33};
        return OASP_EXT_ERROR;
    }
    return OASP_EXT_OK;
}

static bool unwrap_arguments(oasp_ext_call_context *context, const oasp_ext_value *const *arguments, size_t count, oasp_value **result) {
    *result = count ? oasp_runtime_alloc_values(context->runtime, count) : NULL;
    if (count && !*result)
        return false;
    for (size_t i = 0; i < count; i++)
        if (!handle_valid(context, arguments[i])) return false;
        else (*result)[i] = arguments[i]->value;
    return true;
}

static int object_call(oasp_ext_call_context *context, const oasp_ext_value *object, const char *member, size_t member_length, const oasp_ext_value *const *arguments,
                       size_t argument_count, oasp_ext_value **result, oasp_ext_error *error) {
    oasp_value *values = NULL;
    oasp_value output = oasp_value_empty();
    bool error_pending = context && context->runtime && context->runtime->error_pending;
    int error_number = context && context->runtime ? context->runtime->error_number : 0;
    if (result && member && handle_valid(context, object) && object->value.kind == OASP_VALUE_OBJECT &&
        unwrap_arguments(context, arguments, argument_count, &values))
        output = oasp_runtime_call_member(context->runtime, &object->value, (oasp_slice){member, member_length},
                                          values, NULL, argument_count, context->line);
    if (!result || !member || !handle_valid(context, object) || object->value.kind != OASP_VALUE_OBJECT ||
        (argument_count && !values) || (!error_pending && context->runtime->error_pending) ||
        (error_pending && context->runtime->error_number != error_number)) {
        if (error)
            *error = (oasp_ext_error){.struct_size = sizeof(*error), .code = 438, .category = OASP_EXT_ERROR_RUNTIME,
                                      .message = "object method call failed", .message_length = 25};
        return OASP_EXT_ERROR;
    }
    *result = oasp_external_value_create(context, output, public_kind(&output));
    return *result ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int object_default(oasp_ext_call_context *context, const oasp_ext_value *object, const oasp_ext_value *const *arguments, size_t argument_count, oasp_ext_value **result,
                          oasp_ext_error *error) {
    oasp_value *values = NULL;
    oasp_value output = oasp_value_empty();
    if (!result || !handle_valid(context, object) || object->value.kind != OASP_VALUE_OBJECT ||
        !unwrap_arguments(context, arguments, argument_count, &values) ||
        !oasp_component_default(context->runtime, object->value.as.object, values, argument_count, context->line, &output)) {
        if (error)
            *error = (oasp_ext_error){.struct_size = sizeof(*error), .code = 438, .category = OASP_EXT_ERROR_RUNTIME,
                                      .message = "object default invocation failed", .message_length = 32};
        return OASP_EXT_ERROR;
    }
    *result = oasp_external_value_create(context, output, public_kind(&output));
    return *result ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int object_new_owned(oasp_ext_call_context *context, uint32_t class_id, void *instance, oasp_ext_value **result, oasp_ext_error *error) {
    return oasp_external_object_wrap_owned(context, class_id, instance, result, error);
}

static int object_get_native(oasp_ext_call_context *context, const oasp_ext_value *object, uint32_t class_id, void **instance, oasp_ext_error *error) {
    return oasp_external_object_get_native(context, object, class_id, instance, error);
}

bool oasp_external_value_unwrap(oasp_ext_call_context *context, const oasp_ext_value *value, oasp_value *result) {
    if (!result || !handle_valid(context, value))
        return false;
    if (value->public_kind == OASP_EXT_VALUE_BYTES) {
        char *copy = oasp_runtime_copy(context->runtime, value->owned_data ? value->owned_data : "", value->owned_length);
        if (!copy)
            return false;
        *result = oasp_value_string(copy, value->owned_length);
        return true;
    }
    if (value->public_kind == OASP_EXT_VALUE_MAP) {
        oasp_value dictionary = oasp_component_create(context->runtime, (oasp_slice){"Scripting.Dictionary", 20}, context->line);
        if (dictionary.kind != OASP_VALUE_OBJECT)
            return false;
        for (size_t i = 0; i < value->map_count; i++) {
            oasp_value transient_key = oasp_value_string(value->map_entries[i].key, value->map_entries[i].key_length);
            oasp_value key = oasp_runtime_value_copy(context->runtime, &transient_key, true);
            oasp_value item = oasp_runtime_value_copy(context->runtime, &value->map_entries[i].value, true);
            if (!oasp_component_assign(context->runtime, dictionary.as.object, (oasp_slice){"Item", 4}, &key, 1,
                                       &item, context->line))
                return false;
        }
        *result = dictionary;
        return true;
    }
    *result = value->value;
    return true;
}

static const oasp_ext_value_api value_api = {
    .struct_size = sizeof(value_api),
    .version = OASP_EXT_API_VALUE_VERSION,
    .kind = host_kind,
    .get_bool = host_get_bool,
    .get_i64 = host_get_i64,
    .get_f64 = host_get_f64,
    .get_data = host_get_data,
    .new_empty = host_new_empty,
    .new_null = host_new_null,
    .new_bool = host_new_bool,
    .new_i64 = host_new_i64,
    .new_f64 = host_new_f64,
    .new_date = host_new_date,
    .new_string = host_new_string,
    .new_bytes = host_new_bytes,
    .new_array = host_new_array,
    .array_rank = host_array_rank,
    .array_bounds = host_array_bounds,
    .array_get = host_array_get,
    .array_set = host_array_set,
    .new_map = host_new_map,
    .map_size = host_map_size,
    .map_get = host_map_get,
    .map_set = host_map_set,
    .retain = host_retain,
    .release = host_release
};

static const oasp_ext_object_api object_api = {
    .struct_size = sizeof(object_api),
    .version = OASP_EXT_API_OBJECT_VERSION,
    .get = object_get,
    .set = object_set,
    .call = object_call,
    .invoke_default = object_default,
    .new_owned = object_new_owned,
    .get_native = object_get_native
};

const oasp_ext_value_api *oasp_external_value_api(void) {
    return &value_api;
}

const oasp_ext_object_api *oasp_external_object_api(void) {
    return &object_api;
}
