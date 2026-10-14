// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/components/component_minimal.c`): Native component boundary. It maps
 * ASP-compatible object operations onto C libraries while preserving runtime error semantics,
 * ownership, and garbage-collector tracing. Result and error variables are initialized before
 * crossing the boundary.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "component.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct oasp_dictionary_entry {
    struct oasp_dictionary_entry *next;
    oasp_value key;
    oasp_value value;
} oasp_dictionary_entry;

typedef struct {
    oasp_dictionary_entry *first;
    oasp_dictionary_entry *last;
    size_t count;
    int compare_mode;
} oasp_dictionary;

struct oasp_component_context {
    unsigned char reserved;
};

static oasp_value oasp_component_object_value(oasp_object *object) {
    oasp_value value = oasp_value_empty();
    value.kind = OASP_VALUE_OBJECT;
    value.as.object = object;
    return value;
}

static oasp_object *oasp_component_object(oasp_runtime *runtime, const char *name, oasp_dictionary *dictionary) {
    oasp_object *object = oasp_runtime_alloc_object(runtime);
    if (!object)
        return NULL;
    memset(object, 0, sizeof(*object));
    object->component_kind = OASP_COMPONENT_DICTIONARY;
    object->component_name = name;
    object->native = dictionary;
    return object;
}

static void oasp_component_missing(oasp_runtime *runtime, const oasp_object *object, oasp_slice member, size_t line) {
    static const char prefix[] = "component member not found: ";
    char *message = oasp_runtime_alloc(runtime, sizeof(prefix) + member.length, _Alignof(char));
    if (message) {
        memcpy(message, prefix, sizeof(prefix) - 1);
        memcpy(message + sizeof(prefix) - 1, member.data, member.length);
        message[sizeof(prefix) - 1 + member.length] = '\0';
    }
    oasp_runtime_raise(runtime, 438, message ? message : prefix, object->component_name, line);
}

static bool oasp_dictionary_text_equal(oasp_slice left, oasp_slice right, bool ignore_case) {
    if (left.length != right.length)
        return false;
    if (!ignore_case)
        return !left.length || !memcmp(left.data, right.data, left.length);
    for (size_t i = 0; i < left.length; i++)
        if (tolower((unsigned char)left.data[i]) != tolower((unsigned char)right.data[i]))
            return false;
    return true;
}

static bool oasp_dictionary_key_equal(const oasp_dictionary *dictionary, const oasp_value *left, const oasp_value *right) {
    if (left->kind != OASP_VALUE_STRING || right->kind != OASP_VALUE_STRING) {
        int64_t left_integer;
        int64_t right_integer;
        if (oasp_value_to_int(left, &left_integer) && oasp_value_to_int(right, &right_integer))
            return left_integer == right_integer;
    }
    char left_buffer[64];
    char right_buffer[64];
    oasp_slice left_text;
    oasp_slice right_text;
    if (!oasp_value_text(left, left_buffer, sizeof(left_buffer), &left_text) ||
        !oasp_value_text(right, right_buffer, sizeof(right_buffer), &right_text))
        return false;
    return oasp_dictionary_text_equal(left_text, right_text, dictionary->compare_mode != 0);
}

static oasp_dictionary_entry *oasp_dictionary_find(oasp_dictionary *dictionary, const oasp_value *key) {
    for (oasp_dictionary_entry *entry = dictionary->first; entry; entry = entry->next)
        if (oasp_dictionary_key_equal(dictionary, &entry->key, key))
            return entry;
    return NULL;
}

static bool oasp_dictionary_store(oasp_runtime *runtime, oasp_dictionary *dictionary, const oasp_value *key, const oasp_value *value, bool reject_existing,
                                  size_t line) {
    oasp_dictionary_entry *entry = oasp_dictionary_find(dictionary, key);
    if (entry) {
        if (reject_existing) {
            oasp_runtime_raise(runtime, 457, "This key is already associated with an element", "Scripting.Dictionary", line);
            return false;
        }
        entry->value = oasp_runtime_value_copy(runtime, value, false);
        oasp_runtime_write_barrier(runtime, entry, &entry->value);
        return true;
    }
    entry = oasp_runtime_alloc(runtime, sizeof(*entry), _Alignof(oasp_dictionary_entry));
    if (!entry)
        return false;
    memset(entry, 0, sizeof(*entry));
    entry->key = oasp_runtime_value_copy(runtime, key, false);
    entry->value = oasp_runtime_value_copy(runtime, value, false);
    oasp_runtime_write_barrier(runtime, entry, &entry->key);
    oasp_runtime_write_barrier(runtime, entry, &entry->value);
    if (dictionary->last) {
        dictionary->last->next = entry;
        if (runtime->allocator)
            oasp_allocator_write_barrier(runtime->allocator, dictionary->last, entry);
    } else {
        dictionary->first = entry;
    }
    dictionary->last = entry;
    if (runtime->allocator)
        oasp_allocator_write_barrier(runtime->allocator, dictionary, entry);
    dictionary->count++;
    return true;
}

static oasp_value oasp_dictionary_array(oasp_runtime *runtime, const oasp_dictionary *dictionary, bool keys) {
    oasp_array *array = oasp_runtime_alloc_array(runtime);
    if (!array)
        return oasp_value_empty();
    memset(array, 0, sizeof(*array));
    array->rank = 1;
    array->length = dictionary->count;
    array->bounds = oasp_runtime_alloc(runtime, sizeof(*array->bounds), _Alignof(int64_t));
    if (!array->bounds)
        return oasp_value_empty();
    array->bounds[0] = dictionary->count ? (int64_t)dictionary->count - 1 : -1;
    if (dictionary->count) {
        array->items = oasp_runtime_alloc_values(runtime, dictionary->count);
        if (!array->items)
            return oasp_value_empty();
    }
    size_t index = 0;
    for (const oasp_dictionary_entry *entry = dictionary->first; entry; entry = entry->next)
        array->items[index++] = oasp_runtime_value_copy(runtime, keys ? &entry->key : &entry->value, false);
    oasp_value result = oasp_value_empty();
    result.kind = OASP_VALUE_ARRAY;
    result.as.array = array;
    return result;
}

oasp_component_context *oasp_component_context_create(void) { return calloc(1, sizeof(oasp_component_context)); }

void oasp_component_context_destroy(oasp_component_context *context) { free(context); }

void oasp_component_context_release_memory(oasp_component_context *context) { (void)context; }

bool oasp_component_context_load_external(oasp_component_context *context, const oasp_external_module_config *modules, size_t module_count, bool require_sha256, char *error,
                                          size_t error_size) {
    (void)context;
    (void)modules;
    (void)module_count;
    (void)require_sha256;
    if (error && error_size)
        snprintf(error, error_size, "external components are unavailable in PROFILE=minimal");
    return false;
}

bool oasp_component_context_external_worker_init(oasp_component_context *context, char *error, size_t error_size) {
    (void)context;
    (void)error;
    (void)error_size;
    return true;
}

void oasp_component_context_external_worker_shutdown(oasp_component_context *context) { (void)context; }

void *oasp_component_context_external_registry(const oasp_component_context *context) {
    (void)context;
    return NULL;
}

oasp_value oasp_component_create(oasp_runtime *runtime, oasp_slice prog_id, size_t line) {
    bool json_object = oasp_ascii_equal(prog_id, "openasp.json.object");
    if (!json_object && !oasp_ascii_equal(prog_id, "scripting.dictionary")) {
        oasp_runtime_raise(runtime, 429,
                           "component is not included in the minimal build; rebuild with PROFILE=full",
                           "Server", line);
        return oasp_value_empty();
    }
    oasp_dictionary *dictionary = oasp_runtime_alloc(runtime, sizeof(*dictionary), _Alignof(oasp_dictionary));
    if (!dictionary)
        return oasp_value_empty();
    memset(dictionary, 0, sizeof(*dictionary));
    dictionary->compare_mode = json_object ? 0 : 1;
    oasp_object *object = oasp_component_object(runtime, json_object ? "OpenASP.JSON.Object" : "Scripting.Dictionary", dictionary);
    return object ? oasp_component_object_value(object) : oasp_value_empty();
}

bool oasp_component_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result) {
    if (!object || object->component_kind != OASP_COMPONENT_DICTIONARY)
        return false;
    oasp_dictionary *dictionary = object->native;
    if (oasp_ascii_equal(member, "count"))
        *result = oasp_value_int((int64_t)dictionary->count);
    else if (oasp_ascii_equal(member, "keys") || oasp_ascii_equal(member, "items"))
        *result = oasp_dictionary_array(runtime, dictionary, oasp_ascii_equal(member, "keys"));
    else if (oasp_ascii_equal(member, "comparemode"))
        *result = oasp_value_int(dictionary->compare_mode);
    else if (oasp_ascii_equal(member, "removeall"))
        return oasp_component_call(runtime, object, member, NULL, 0, line, result);
    else
        oasp_component_missing(runtime, object, member, line);
    return true;
}

bool oasp_component_get_id(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, size_t line,
                           oasp_value *result) {
    (void)member_id;
    return oasp_component_get(runtime, object, member, line, result);
}

bool oasp_component_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (!object || object->component_kind != OASP_COMPONENT_DICTIONARY)
        return false;
    if (!oasp_ascii_equal(member, "comparemode")) {
        oasp_component_missing(runtime, object, member, line);
        return true;
    }
    int64_t mode = 0;
    if (!oasp_value_to_int(value, &mode) || (mode != 0 && mode != 1))
        oasp_runtime_raise(runtime, 5, "invalid CompareMode", object->component_name, line);
    else
        ((oasp_dictionary *)object->native)->compare_mode = (int)mode;
    return true;
}

bool oasp_component_call_id(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, oasp_value *arguments,
                            size_t argument_count, size_t line, oasp_value *result) {
    (void)member_id;
    if (!object || object->component_kind != OASP_COMPONENT_DICTIONARY)
        return false;
    oasp_dictionary *dictionary = object->native;
    if (oasp_ascii_equal(member, "add")) {
        if (argument_count < 2)
            oasp_runtime_raise(runtime, 450, "wrong number of arguments", object->component_name, line);
        else
            (void)oasp_dictionary_store(runtime, dictionary, &arguments[0], &arguments[1], true, line);
        *result = oasp_value_empty();
    } else if (oasp_ascii_equal(member, "exists")) {
        *result = oasp_value_bool(argument_count && oasp_dictionary_find(dictionary, &arguments[0]));
    } else if (oasp_ascii_equal(member, "item")) {
        oasp_dictionary_entry *entry = argument_count ? oasp_dictionary_find(dictionary, &arguments[0]) : NULL;
        *result = entry ? entry->value : oasp_value_empty();
    } else if (oasp_ascii_equal(member, "remove")) {
        oasp_dictionary_entry *previous = NULL;
        oasp_dictionary_entry *entry = argument_count ? dictionary->first : NULL;
        while (entry && !oasp_dictionary_key_equal(dictionary, &entry->key, &arguments[0])) {
            previous = entry;
            entry = entry->next;
        }
        if (entry) {
            if (previous)
                previous->next = entry->next;
            else
                dictionary->first = entry->next;
            if (dictionary->last == entry)
                dictionary->last = previous;
            dictionary->count--;
        }
        *result = oasp_value_empty();
    } else if (oasp_ascii_equal(member, "removeall")) {
        dictionary->first = NULL;
        dictionary->last = NULL;
        dictionary->count = 0;
        *result = oasp_value_empty();
    } else {
        oasp_component_missing(runtime, object, member, line);
    }
    return true;
}

bool oasp_component_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                         size_t line, oasp_value *result) {
    return oasp_component_call_id(runtime, object, 0, member, arguments, argument_count, line, result);
}

bool oasp_component_default(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t argument_count, size_t line,
                            oasp_value *result) {
    if (!object || object->component_kind != OASP_COMPONENT_DICTIONARY)
    if (!object || object->component_kind != OASP_COMPONENT_DICTIONARY)
        return false;
    return oasp_component_call(runtime, object, (oasp_slice){"item", 4}, arguments, argument_count, line, result);
}

bool oasp_component_recordset_field(oasp_runtime *runtime, oasp_object *object, oasp_slice column, size_t line, oasp_value *result) {
    (void)runtime;
    (void)object;
    (void)column;
    (void)line;
    (void)result;
    return false;
}

bool oasp_component_recordset_field_view(oasp_runtime *runtime, oasp_object *object, oasp_slice column, int *ordinal, const void **schema,
                                         bool *copy_text, size_t line, const oasp_value **result) {
    (void)runtime;
    (void)object;
    (void)column;
    (void)ordinal;
    (void)schema;
    (void)copy_text;
    (void)line;
    (void)result;
    return false;
}

bool oasp_component_recordset_move_next(oasp_runtime *runtime, oasp_object *object, size_t line) {
    (void)runtime;
    (void)object;
    (void)line;
    return false;
}

bool oasp_component_recordset_eof(oasp_runtime *runtime, oasp_object *object, size_t line, bool *result) {
    (void)runtime;
    (void)object;
    (void)line;
    (void)result;
    return false;
}

bool oasp_component_assign(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                           const oasp_value *value, size_t line) {
    if (!object || object->component_kind != OASP_COMPONENT_DICTIONARY)
        return false;
    if (member.length && !oasp_ascii_equal(member, "item")) {
        oasp_component_missing(runtime, object, member, line);
        return true;
    }
    if (!argument_count)
        oasp_runtime_raise(runtime, 450, "wrong number of arguments", object->component_name, line);
    else
        (void)oasp_dictionary_store(runtime, object->native, &arguments[0], value, false, line);
    return true;
}

bool oasp_component_enumerate(oasp_runtime *runtime, oasp_object *object, oasp_value *result) {
    (void)runtime;
    (void)object;
    (void)result;
    return false;
}

bool oasp_component_persist_copy(oasp_runtime *runtime, const oasp_object *source, oasp_object **result) {
    if (!runtime || !source || !result || source->component_kind != OASP_COMPONENT_DICTIONARY)
        return false;
    const oasp_dictionary *source_dictionary = source->native;
    oasp_dictionary *dictionary = oasp_runtime_alloc(runtime, sizeof(*dictionary), _Alignof(oasp_dictionary));
    if (!dictionary)
        return false;
    memset(dictionary, 0, sizeof(*dictionary));
    dictionary->compare_mode = source_dictionary ? source_dictionary->compare_mode : 1;
    oasp_object *object = oasp_component_object(runtime, source->component_name, dictionary);
    if (!object)
        return false;
    for (const oasp_dictionary_entry *entry = source_dictionary ? source_dictionary->first : NULL; entry; entry = entry->next) {
        oasp_dictionary_entry *copy = oasp_runtime_alloc(runtime, sizeof(*copy), _Alignof(oasp_dictionary_entry));
        if (!copy)
            return false;
        memset(copy, 0, sizeof(*copy));
        copy->key = oasp_runtime_persist_copy(runtime, &entry->key);
        copy->value = oasp_runtime_persist_copy(runtime, &entry->value);
        if (dictionary->last)
            dictionary->last->next = copy;
        else
            dictionary->first = copy;
        dictionary->last = copy;
        dictionary->count++;
    }
    *result = object;
    return true;
}

bool oasp_component_dictionary_info(const oasp_object *object, int *compare_mode, size_t *count) {
    if (!object || object->component_kind != OASP_COMPONENT_DICTIONARY || !object->native)
        return false;
    const oasp_dictionary *dictionary = object->native;
    if (compare_mode)
        *compare_mode = dictionary->compare_mode;
    if (count)
        *count = dictionary->count;
    return true;
}

bool oasp_component_dictionary_item(const oasp_object *object, size_t index, const oasp_value **key, const oasp_value **value) {
    if (!object || object->component_kind != OASP_COMPONENT_DICTIONARY || !object->native)
        return false;
    const oasp_dictionary_entry *entry = ((const oasp_dictionary *)object->native)->first;
    while (entry && index) {
        entry = entry->next;
        index--;
    }
    if (!entry)
        return false;
    if (key)
        *key = &entry->key;
    if (value)
        *value = &entry->value;
    return true;
}

oasp_object *oasp_component_dictionary_restore(oasp_runtime *runtime, int compare_mode, const oasp_value *keys, const oasp_value *values, size_t count) {
    if (!runtime || (count && (!keys || !values)))
        return NULL;
    oasp_dictionary *dictionary = oasp_runtime_alloc(runtime, sizeof(*dictionary), _Alignof(oasp_dictionary));
    if (!dictionary)
        return NULL;
    memset(dictionary, 0, sizeof(*dictionary));
    dictionary->compare_mode = compare_mode;
    oasp_object *object = oasp_component_object(runtime, "Scripting.Dictionary", dictionary);
    if (!object)
        return NULL;
    for (size_t i = 0; i < count; i++)
        if (!oasp_dictionary_store(runtime, dictionary, &keys[i], &values[i], false, 0))
            return NULL;
    return object;
}

static void oasp_component_gc_trace_value(oasp_allocator *allocator, const oasp_value *value) {
    if (value->kind == OASP_VALUE_STRING)
        oasp_allocator_trace(allocator, value->as.string.data);
    else if (value->kind == OASP_VALUE_DATE)
        oasp_allocator_trace(allocator, value->as.date.data);
    else if (value->kind == OASP_VALUE_ARRAY)
        oasp_allocator_trace(allocator, value->as.array);
    else if (value->kind == OASP_VALUE_OBJECT)
        oasp_allocator_trace(allocator, value->as.object);
}

void oasp_component_gc_trace(oasp_allocator *allocator, const oasp_object *object) {
    if (!allocator || !object || object->component_kind != OASP_COMPONENT_DICTIONARY ||
        !object->native)
        return;
    oasp_dictionary *dictionary = object->native;
    oasp_allocator_trace(allocator, dictionary);
    for (oasp_dictionary_entry *entry = dictionary->first; entry; entry = entry->next) {
        oasp_allocator_trace(allocator, entry);
        oasp_component_gc_trace_value(allocator, &entry->key);
        oasp_component_gc_trace_value(allocator, &entry->value);
    }
}

bool oasp_component_xml_snapshot(const oasp_object *object, char **data, size_t *length) {
    (void)object;
    if (data)
        *data = NULL;
    if (length)
        *length = 0;
    return false;
}

bool oasp_component_xml_restore(oasp_runtime *runtime, oasp_slice data, oasp_object **result) {
    (void)runtime;
    (void)data;
    (void)result;
    return false;
}
