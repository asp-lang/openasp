// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "component.h"
#include "builtin_internal.h"
#include "component_object.h"
#include "ext_loader.h"

#include <stdlib.h>
#include <string.h>
bool oasp_builtin_component_default(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t argument_count, size_t line,
                                    oasp_value *result) {
    if (!object)
        return false;
    if (oasp_builtin_ado_default(runtime, object, arguments, argument_count, line, result))
        return true;
    if (object->component_kind == OASP_COMPONENT_DICTIONARY ||
        object->component_kind == OASP_COMPONENT_ADO_PARAMETERS) {
        return oasp_component_call(runtime, object, (oasp_slice){"item", 4},
                                   arguments, argument_count, line, result);
    }
    if (object->component_kind == OASP_COMPONENT_MATCH_COLLECTION ||
        object->component_kind == OASP_COMPONENT_SUBMATCH_COLLECTION ||
        object->component_kind == OASP_COMPONENT_XML_NODE_LIST ||
        object->component_kind == OASP_COMPONENT_ADO_FIELDS) {
        oasp_component_collection *collection = object->native;
        int64_t index = -1;
        if (!argument_count || !oasp_value_to_int(&arguments[0], &index) || index < 0 ||
            (size_t)index >= collection->count) {
            oasp_runtime_raise(runtime, 9, "subscript out of range", object->component_name, line);
            *result = oasp_value_empty();
        } else {
            *result = collection->items[index];
        }
        return true;
    }
    return false;
}

bool oasp_component_recordset_field_view(oasp_runtime *runtime, oasp_object *object, oasp_slice column, int *ordinal, const void **schema,
                                         bool *copy_text, size_t line, const oasp_value **result) {
    return oasp_builtin_ado_recordset_field_view(runtime, object, column, ordinal, schema, copy_text, line, result);
}

bool oasp_component_recordset_field(oasp_runtime *runtime, oasp_object *object, oasp_slice column, size_t line, oasp_value *result) {
    const oasp_value *view = NULL;
    bool copy_text = true;
    if (!oasp_component_recordset_field_view(runtime, object, column, NULL, NULL, &copy_text, line, &view))
        return false;
    *result = view ? oasp_runtime_value_copy(runtime, view, copy_text) : oasp_value_empty();
    return true;
}

bool oasp_component_recordset_move_next(oasp_runtime *runtime, oasp_object *object, size_t line) {
    return oasp_builtin_ado_recordset_move_next(runtime, object, line);
}

bool oasp_component_recordset_eof(oasp_runtime *runtime, oasp_object *object, size_t line, bool *result) {
    return oasp_builtin_ado_recordset_eof(runtime, object, line, result);
}

bool oasp_component_assign(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                           const oasp_value *value, size_t line) {
    if (!object || object->component_kind == OASP_COMPONENT_NONE)
        return false;
    if (oasp_builtin_ado_assign(runtime, object, member, arguments, argument_count, value, line))
        return true;
    if (object->component_kind == OASP_COMPONENT_EXTERNAL) {
        if (argument_count) {
            oasp_runtime_raise(runtime, 438, "external component indexed assignment is unsupported",
                               object->component_name, line);
            return true;
        }
        return oasp_external_component_set(runtime, object, member, value, line);
    }
    if (object->component_kind == OASP_COMPONENT_DICTIONARY &&
        (!member.length || oasp_ascii_equal(member, "item"))) {
        if (!argument_count) {
            oasp_runtime_raise(runtime, 450, "wrong number of arguments", object->component_name, line);
            return true;
        }
        (void)oasp_builtin_dictionary_assign(runtime, object, &arguments[0], value, line);
        return true;
    }
    oasp_component_missing(runtime, object, member, line);
    return true;
}

bool oasp_builtin_component_enumerate(oasp_runtime *runtime, oasp_object *object, oasp_value *result) {
    if (object && object->component_kind == OASP_COMPONENT_EXTERNAL)
        return oasp_external_component_enumerate(runtime, object, result);
    if (!object || (object->component_kind != OASP_COMPONENT_DICTIONARY &&
                    object->component_kind != OASP_COMPONENT_MATCH_COLLECTION &&
                    object->component_kind != OASP_COMPONENT_SUBMATCH_COLLECTION &&
                    object->component_kind != OASP_COMPONENT_XML_NODE_LIST &&
                    object->component_kind != OASP_COMPONENT_FILE_COLLECTION &&
                    object->component_kind != OASP_COMPONENT_FOLDER_COLLECTION &&
                    object->component_kind != OASP_COMPONENT_ADO_FIELDS))
        return false;
    if (object->component_kind == OASP_COMPONENT_DICTIONARY)
        return oasp_builtin_dictionary_enumerate(runtime, object, result);
    oasp_component_collection *collection = object->native;
    oasp_array *array = oasp_runtime_alloc_array(runtime);
    if (!array)
        return false;
    memset(array, 0, sizeof(*array));
    array->rank = 1;
    array->length = collection->count;
    array->bounds = oasp_runtime_alloc(runtime, sizeof(*array->bounds), _Alignof(int64_t));
    if (!array->bounds)
        return false;
    array->bounds[0] = collection->count ? (int64_t)collection->count - 1 : -1;
    if (collection->count) {
        array->items = oasp_runtime_alloc_values(runtime, collection->count);
        if (!array->items)
            return false;
        for (size_t i = 0; i < collection->count; i++)
            array->items[i] = collection->items[i];
    }
    *result = oasp_value_empty();
    result->kind = OASP_VALUE_ARRAY;
    result->as.array = array;
    return true;
}

bool oasp_builtin_component_persist_copy(oasp_runtime *runtime, const oasp_object *source, oasp_object **result) {
    if (!runtime || !source || !result)
        return false;
    if (source->component_kind == OASP_COMPONENT_EXTERNAL)
        return oasp_external_component_persist_copy(runtime, source, result);
    if (source->component_kind == OASP_COMPONENT_XML) {
        char *data = NULL;
        size_t length = 0;
        bool copied = oasp_component_xml_snapshot(source, &data, &length) &&
                      oasp_component_xml_restore(runtime, (oasp_slice){data, length}, result);
        free(data);
        return copied;
    }
    return source->component_kind == OASP_COMPONENT_DICTIONARY &&
           oasp_builtin_dictionary_persist_copy(runtime, source, result);
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
    const oasp_dictionary *dictionary = object->native;
    const oasp_dictionary_entry *entry = dictionary->first;
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
    oasp_object *object = NULL;
    return oasp_builtin_dictionary_restore(runtime, compare_mode, keys, values, count, &object) ? object : NULL;
}
