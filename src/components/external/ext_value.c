// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ext_internal.h"

#include <stdint.h>

static _Thread_local oasp_runtime *current_runtime;
static _Thread_local void *current_external_owner;

void oasp_external_call_enter(oasp_runtime *runtime, void *owner) {
    current_runtime = runtime;
    current_external_owner = owner;
}

void oasp_external_call_leave(void) {
    current_external_owner = NULL;
    current_runtime = NULL;
}

static oasp_ext_value_kind host_value_kind(const oasp_value_handle *handle) {
    const oasp_value_handle_impl *value = (const oasp_value_handle_impl *)handle;
    if (!value)
        return OASP_EXT_VALUE_EMPTY;
    switch (value->value.kind) {
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

static int host_value_get_bool(const oasp_value_handle *handle, int *result) {
    const oasp_value_handle_impl *value = (const oasp_value_handle_impl *)handle;
    if (!value || !result)
        return -1;
    *result = oasp_value_truthy(&value->value);
    return 0;
}

static int host_value_get_i64(const oasp_value_handle *handle, int64_t *result) {
    const oasp_value_handle_impl *value = (const oasp_value_handle_impl *)handle;
    return value && oasp_value_to_int(&value->value, result) ? 0 : -1;
}

static int host_value_get_f64(const oasp_value_handle *handle, double *result) {
    const oasp_value_handle_impl *value = (const oasp_value_handle_impl *)handle;
    return value && oasp_value_to_float(&value->value, result) ? 0 : -1;
}

static int host_value_get_bytes(const oasp_value_handle *handle, oasp_ext_bytes *result) {
    const oasp_value_handle_impl *value = (const oasp_value_handle_impl *)handle;
    if (!value || !result || (value->value.kind != OASP_VALUE_STRING && value->value.kind != OASP_VALUE_DATE))
        return -1;
    oasp_slice slice = value->value.kind == OASP_VALUE_STRING ? value->value.as.string : value->value.as.date;
    result->data = slice.data;
    result->length = slice.length;
    return 0;
}

oasp_value_handle *oasp_external_value_create(oasp_value value) {
    oasp_value_handle_impl *handle = current_runtime
        ? oasp_runtime_alloc(current_runtime, sizeof(*handle), _Alignof(oasp_value_handle_impl)) : NULL;
    if (!handle)
        return NULL;
    handle->value = value;
    handle->references = 1;
    handle->released = false;
    return (oasp_value_handle *)handle;
}

static oasp_value_handle *host_new_empty(void) { return oasp_external_value_create(oasp_value_empty()); }
static oasp_value_handle *host_new_null(void) { return oasp_external_value_create(oasp_value_null()); }
static oasp_value_handle *host_new_bool(int value) { return oasp_external_value_create(oasp_value_bool(value != 0)); }
static oasp_value_handle *host_new_i64(int64_t value) { return oasp_external_value_create(oasp_value_int(value)); }
static oasp_value_handle *host_new_f64(double value) { return oasp_external_value_create(oasp_value_float(value)); }

static oasp_value_handle *host_new_string(const void *data, size_t length) {
    char *copy = current_runtime && data ? oasp_runtime_copy(current_runtime, data, length) : NULL;
    return copy || !length ? oasp_external_value_create(oasp_value_string(copy ? copy : "", length)) : NULL;
}

static oasp_value_handle *host_new_array(size_t length) {
    if (!current_runtime || length > SIZE_MAX / sizeof(oasp_value))
        return NULL;
    oasp_array *array = oasp_runtime_alloc_array(current_runtime);
    oasp_value *items = length ? oasp_runtime_alloc_values(current_runtime, length) : NULL;
    int64_t *bounds = oasp_runtime_alloc(current_runtime, sizeof(*bounds), _Alignof(int64_t));
    if (!array || (length && !items) || !bounds)
        return NULL;
    *bounds = length ? (int64_t)length - 1 : -1;
    array->rank = 1;
    array->length = length;
    array->bounds = bounds;
    array->items = items;
    for (size_t i = 0; i < length; i++)
        items[i] = oasp_value_empty();
    return oasp_external_value_create((oasp_value){.kind = OASP_VALUE_ARRAY, .as.array = array});
}

static int host_array_get(const oasp_value_handle *value, size_t index, oasp_value_handle **result) {
    const oasp_value_handle_impl *handle = (const oasp_value_handle_impl *)value;
    if (!handle || handle->released || !result || handle->value.kind != OASP_VALUE_ARRAY ||
        !handle->value.as.array || index >= handle->value.as.array->length)
        return -1;
    *result = oasp_external_value_create(handle->value.as.array->items[index]);
    return *result ? 0 : -1;
}

static int host_array_set(oasp_value_handle *value, size_t index, const oasp_value_handle *item) {
    oasp_value_handle_impl *handle = (oasp_value_handle_impl *)value;
    const oasp_value_handle_impl *source = (const oasp_value_handle_impl *)item;
    if (!handle || !source || handle->released || source->released || handle->value.kind != OASP_VALUE_ARRAY ||
        !handle->value.as.array || index >= handle->value.as.array->length)
        return -1;
    handle->value.as.array->items[index] = source->value;
    oasp_runtime_write_barrier(current_runtime, handle->value.as.array, &source->value);
    return 0;
}

static int host_object_get(const oasp_value_handle *value, const char *member, size_t member_length, oasp_value_handle **result, oasp_ext_error *error) {
    const oasp_value_handle_impl *handle = (const oasp_value_handle_impl *)value;
    oasp_value output = oasp_value_empty();
    if (!current_runtime || !handle || handle->released || !member || handle->value.kind != OASP_VALUE_OBJECT ||
        !oasp_component_get(current_runtime, handle->value.as.object, (oasp_slice){member, member_length}, 0, &output)) {
        if (error) *error = (oasp_ext_error){sizeof(*error), 5, "object property access failed", "OpenASP.Extension"};
        return -1;
    }
    *result = oasp_external_value_create(output);
    return *result ? 0 : -1;
}

static int host_object_call(const oasp_value_handle *value, const char *member, size_t member_length, const oasp_value_handle *const *args, size_t argc,
                            oasp_value_handle **result, oasp_ext_error *error) {
    const oasp_value_handle_impl *handle = (const oasp_value_handle_impl *)value;
    oasp_value *values = argc ? oasp_runtime_alloc_values(current_runtime, argc) : NULL;
    oasp_value output = oasp_value_empty();
    if (!current_runtime || !handle || handle->released || !member || handle->value.kind != OASP_VALUE_OBJECT ||
        (argc && !values))
        return -1;
    for (size_t i = 0; i < argc; i++) {
        const oasp_value_handle_impl *argument = (const oasp_value_handle_impl *)args[i];
        if (!argument || argument->released)
            return -1;
        values[i] = argument->value;
    }
    if (!oasp_component_call(current_runtime, handle->value.as.object, (oasp_slice){member, member_length},
                             values, argc, 0, &output)) {
        if (error) *error = (oasp_ext_error){sizeof(*error), 5, "object method call failed", "OpenASP.Extension"};
        return -1;
    }
    *result = oasp_external_value_create(output);
    return *result ? 0 : -1;
}

static int host_retain(const oasp_value_handle *value) {
    oasp_value_handle_impl *handle = (oasp_value_handle_impl *)value;
    if (!handle || handle->released || handle->references == SIZE_MAX)
        return -1;
    handle->references++;
    return 0;
}

static void host_release(const oasp_value_handle *value) {
    oasp_value_handle_impl *handle = (oasp_value_handle_impl *)value;
    if (handle && !handle->released && handle->references && --handle->references == 0)
        handle->released = true;
}

int oasp_external_host_account_external(ptrdiff_t delta) {
    return current_runtime && current_external_owner &&
                   oasp_runtime_account_external(current_runtime, current_external_owner, delta) ? 0 : -1;
}

void oasp_external_host_value_api(oasp_host_api *api) {
    api->value_kind = host_value_kind;
    api->value_get_bool = host_value_get_bool;
    api->value_get_i64 = host_value_get_i64;
    api->value_get_f64 = host_value_get_f64;
    api->value_get_bytes = host_value_get_bytes;
    api->value_new_empty = host_new_empty;
    api->value_new_null = host_new_null;
    api->value_new_bool = host_new_bool;
    api->value_new_i64 = host_new_i64;
    api->value_new_f64 = host_new_f64;
    api->value_new_string = host_new_string;
    api->value_new_array = host_new_array;
    api->value_array_get = host_array_get;
    api->value_array_set = host_array_set;
    api->value_object_get = host_object_get;
    api->value_object_call = host_object_call;
    api->value_retain = host_retain;
    api->value_release = host_release;
}
