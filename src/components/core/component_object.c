// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "component_object.h"

#include <string.h>

oasp_value oasp_component_object_value(oasp_object *object) {
    oasp_value value = oasp_value_empty();
    value.kind = OASP_VALUE_OBJECT;
    value.as.object = object;
    return value;
}

oasp_object *oasp_component_object(oasp_runtime *runtime, oasp_component_kind kind, const char *name, void *native) {
    oasp_object *object = oasp_runtime_alloc_object(runtime);
    if (!object)
        return NULL;
    memset(object, 0, sizeof(*object));
    object->component_kind = kind;
    object->component_name = name;
    object->native = native;
    object->component_vtable = oasp_builtin_component_vtable();
    return object;
}

void oasp_component_missing(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line) {
    const char *prefix = "component member not found: ";
    size_t prefix_length = strlen(prefix);
    char *message = oasp_runtime_alloc(runtime, prefix_length + member.length + 1, _Alignof(char));
    if (message) {
        memcpy(message, prefix, prefix_length);
        memcpy(message + prefix_length, member.data, member.length);
        message[prefix_length + member.length] = '\0';
    }
    oasp_runtime_raise(runtime, 438, message ? message : prefix, object->component_name, line);
}
