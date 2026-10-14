// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"

#include <stdio.h>
#include <string.h>

static bool dictionary_text_equal(oasp_slice left, oasp_slice right, bool ignore_case) {
    if (left.length != right.length)
        return false;
    for (size_t i = 0; i < left.length; i++) {
        unsigned char a = (unsigned char)left.data[i];
        unsigned char b = (unsigned char)right.data[i];
        if (ignore_case) {
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b + ('a' - 'A'));
        }
        if (a != b)
            return false;
    }
    return true;
}

static bool dictionary_key_equal(const oasp_dictionary *dictionary, const oasp_value *left, const oasp_value *right) {
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
    return oasp_value_text(left, left_buffer, sizeof(left_buffer), &left_text) &&
           oasp_value_text(right, right_buffer, sizeof(right_buffer), &right_text) &&
           dictionary_text_equal(left_text, right_text, dictionary->compare_mode != 0);
}

static oasp_dictionary_entry *dictionary_find(oasp_dictionary *dictionary, const oasp_value *key) {
    for (oasp_dictionary_entry *entry = dictionary->first; entry; entry = entry->next)
        if (dictionary_key_equal(dictionary, &entry->key, key))
            return entry;
    return NULL;
}

static bool dictionary_store(oasp_runtime *runtime, oasp_dictionary *dictionary, const oasp_value *key, const oasp_value *value, bool reject_existing,
                             size_t line) {
    oasp_dictionary_entry *entry = dictionary_find(dictionary, key);
    if (entry) {
        if (reject_existing) {
            oasp_runtime_raise(runtime, 457, "key already exists", "Scripting.Dictionary", line);
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
    if (oasp_process_options.trace_dictionary) {
        char *key_text = oasp_value_format(runtime->arena, key);
        char *value_text = oasp_value_format(runtime->arena, value);
        fprintf(stderr, "OpenASP dictionary add %s (%zu bytes)\n",
                key_text ? key_text : "", value_text ? strlen(value_text) : 0);
    }
    return true;
}

static oasp_value dictionary_array(oasp_runtime *runtime, const oasp_dictionary *dictionary, bool keys) {
    oasp_array *array = oasp_runtime_alloc_array(runtime);
    if (!array)
        return oasp_value_empty();
    memset(array, 0, sizeof(*array));
    array->rank = 1;
    array->length = dictionary->count;
    array->bounds = oasp_runtime_alloc(runtime, sizeof(*array->bounds), _Alignof(int64_t));
    array->items = dictionary->count ? oasp_runtime_alloc_values(runtime, dictionary->count) : NULL;
    if (!array->bounds || (dictionary->count && !array->items))
        return oasp_value_empty();
    array->bounds[0] = dictionary->count ? (int64_t)dictionary->count - 1 : -1;
    size_t index = 0;
    for (const oasp_dictionary_entry *entry = dictionary->first; entry; entry = entry->next)
        array->items[index++] = oasp_runtime_value_copy(runtime, keys ? &entry->key : &entry->value, false);
    return (oasp_value){.kind = OASP_VALUE_ARRAY, .as.array = array};
}

oasp_dictionary *oasp_builtin_dictionary_create(oasp_runtime *runtime, int compare_mode) {
    oasp_dictionary *dictionary = oasp_runtime_alloc(runtime, sizeof(*dictionary), _Alignof(oasp_dictionary));
    if (dictionary) {
        memset(dictionary, 0, sizeof(*dictionary));
        dictionary->compare_mode = compare_mode;
    }
    return dictionary;
}

bool oasp_builtin_dictionary_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *result) {
    oasp_dictionary *dictionary = object->native;
    if (oasp_ascii_equal(member, "count"))
        *result = oasp_value_int((int64_t)dictionary->count);
    else if (oasp_ascii_equal(member, "keys") || oasp_ascii_equal(member, "items"))
        *result = dictionary_array(runtime, dictionary, oasp_ascii_equal(member, "keys"));
    else if (oasp_ascii_equal(member, "comparemode"))
        *result = oasp_value_int(dictionary->compare_mode);
    else
        return false;
    return true;
}

bool oasp_builtin_dictionary_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (!oasp_ascii_equal(member, "comparemode"))
        return false;
    int64_t mode = 0;
    if (!oasp_value_to_int(value, &mode) || (mode != 0 && mode != 1))
        oasp_runtime_raise(runtime, 5, "invalid CompareMode", object->component_name, line);
    else
        ((oasp_dictionary *)object->native)->compare_mode = (int)mode;
    return true;
}

bool oasp_builtin_dictionary_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                  size_t line, oasp_value *result) {
    oasp_dictionary *dictionary = object->native;
    if (oasp_ascii_equal(member, "add")) {
        if (argument_count < 2)
            oasp_runtime_raise(runtime, 450, "wrong number of arguments", object->component_name, line);
        else
            (void)dictionary_store(runtime, dictionary, &arguments[0], &arguments[1], true, line);
    } else if (oasp_ascii_equal(member, "exists")) {
        *result = oasp_value_bool(argument_count && dictionary_find(dictionary, &arguments[0]));
        return true;
    } else if (oasp_ascii_equal(member, "item")) {
        oasp_dictionary_entry *entry = argument_count ? dictionary_find(dictionary, &arguments[0]) : NULL;
        *result = entry ? entry->value : oasp_value_empty();
        return true;
    } else if (oasp_ascii_equal(member, "remove")) {
        if (argument_count) {
            oasp_dictionary_entry *previous = NULL;
            oasp_dictionary_entry *entry = dictionary->first;
            while (entry && !dictionary_key_equal(dictionary, &entry->key, &arguments[0])) {
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
        }
    } else if (oasp_ascii_equal(member, "removeall")) {
        dictionary->first = NULL;
        dictionary->last = NULL;
        dictionary->count = 0;
    } else {
        return false;
    }
    *result = oasp_value_empty();
    return true;
}

bool oasp_builtin_dictionary_assign(oasp_runtime *runtime, oasp_object *object, const oasp_value *key, const oasp_value *value, size_t line) {
    return dictionary_store(runtime, object->native, key, value, false, line);
}

bool oasp_builtin_dictionary_enumerate(oasp_runtime *runtime, oasp_object *object, oasp_value *result) {
    *result = dictionary_array(runtime, object->native, true);
    return result->kind == OASP_VALUE_ARRAY;
}

bool oasp_builtin_dictionary_persist_copy(oasp_runtime *runtime, const oasp_object *source, oasp_object **result) {
    const oasp_dictionary *source_dictionary = source->native;
    oasp_dictionary *dictionary = oasp_builtin_dictionary_create(runtime, source_dictionary ? source_dictionary->compare_mode : 1);
    oasp_object *object = dictionary ? oasp_component_object(runtime, OASP_COMPONENT_DICTIONARY, "Scripting.Dictionary", dictionary) : NULL;
    if (!object)
        return false;
    for (const oasp_dictionary_entry *entry = source_dictionary ? source_dictionary->first : NULL; entry; entry = entry->next) {
        oasp_value key = oasp_runtime_persist_copy(runtime, &entry->key);
        oasp_value value = oasp_runtime_persist_copy(runtime, &entry->value);
        if (!dictionary_store(runtime, dictionary, &key, &value, false, 0))
            return false;
    }
    *result = object;
    return true;
}

bool oasp_builtin_dictionary_restore(oasp_runtime *runtime, int compare_mode, const oasp_value *keys, const oasp_value *values, size_t count,
                                     oasp_object **result) {
    oasp_dictionary *dictionary = oasp_builtin_dictionary_create(runtime, compare_mode);
    oasp_object *object = dictionary ? oasp_component_object(runtime, OASP_COMPONENT_DICTIONARY, "Scripting.Dictionary", dictionary) : NULL;
    if (!object)
        return false;
    for (size_t i = 0; i < count; i++)
        if (!dictionary_store(runtime, dictionary, &keys[i], &values[i], false, 0))
            return false;
    *result = object;
    return true;
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

void oasp_builtin_dictionary_gc_trace(oasp_allocator *allocator, const oasp_dictionary *dictionary) {
    for (const oasp_dictionary_entry *entry = dictionary->first; entry; entry = entry->next) {
        oasp_allocator_trace(allocator, entry);
        trace_value(allocator, &entry->key);
        trace_value(allocator, &entry->value);
    }
}
