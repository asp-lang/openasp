// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "component.h"

/*
 * Built-in implementations remain in component.c while its historical public
 * entry points are now private adapters. This file owns the ABI-facing,
 * uniform vtable dispatch boundary.
 */
bool oasp_builtin_component_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result);
bool oasp_builtin_component_get_id(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, size_t line,
                                   oasp_value *result);
bool oasp_builtin_component_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_component_call_id(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, oasp_value *arguments,
                                    size_t argument_count, size_t line, oasp_value *result);
bool oasp_builtin_component_default(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t argument_count, size_t line,
                                    oasp_value *result);
bool oasp_builtin_component_enumerate(oasp_runtime *runtime, oasp_object *object, oasp_value *result);
bool oasp_builtin_component_persist_copy(oasp_runtime *runtime, const oasp_object *source, oasp_object **result);
void oasp_builtin_component_gc_trace(oasp_allocator *allocator, const oasp_object *object);

static const oasp_component_vtable builtin_vtable = {
    oasp_builtin_component_get, oasp_builtin_component_get_id, oasp_builtin_component_set,
    oasp_builtin_component_call_id, oasp_builtin_component_default, oasp_builtin_component_enumerate,
    oasp_builtin_component_persist_copy, oasp_builtin_component_gc_trace
};

const oasp_component_vtable *oasp_builtin_component_vtable(void) {
    return &builtin_vtable;
}

static const oasp_component_vtable *component_vtable(const oasp_object *object) {
    return object && object->component_kind != OASP_COMPONENT_NONE
               ? (object->component_vtable ? object->component_vtable : &builtin_vtable) : NULL;
}

bool oasp_component_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result) {
    const oasp_component_vtable *vtable = component_vtable(object);
    return vtable && vtable->get ? vtable->get(runtime, object, member, line, result) : false;
}

bool oasp_component_get_id(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, size_t line,
                           oasp_value *result) {
    const oasp_component_vtable *vtable = component_vtable(object);
    if (!vtable)
        return false;
    return vtable->get_id ? vtable->get_id(runtime, object, member_id, member, line, result)
                          : oasp_component_get(runtime, object, member, line, result);
}

bool oasp_component_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    const oasp_component_vtable *vtable = component_vtable(object);
    return vtable && vtable->set ? vtable->set(runtime, object, member, value, line) : false;
}

bool oasp_component_call_id(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, oasp_value *arguments,
                            size_t argument_count, size_t line, oasp_value *result) {
    const oasp_component_vtable *vtable = component_vtable(object);
    return vtable && vtable->call ? vtable->call(runtime, object, member_id, member, arguments, argument_count, line, result) : false;
}

bool oasp_component_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                         size_t line, oasp_value *result) {
    return oasp_component_call_id(runtime, object, 0, member, arguments, argument_count, line, result);
}

bool oasp_component_default(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t argument_count, size_t line,
                            oasp_value *result) {
    const oasp_component_vtable *vtable = component_vtable(object);
    return vtable && vtable->default_member ? vtable->default_member(runtime, object, arguments, argument_count, line, result) : false;
}

bool oasp_component_enumerate(oasp_runtime *runtime, oasp_object *object, oasp_value *result) {
    const oasp_component_vtable *vtable = component_vtable(object);
    return vtable && vtable->enumerate ? vtable->enumerate(runtime, object, result) : false;
}

bool oasp_component_persist_copy(oasp_runtime *runtime, const oasp_object *source, oasp_object **result) {
    const oasp_component_vtable *vtable = component_vtable(source);
    return vtable && vtable->persist_copy ? vtable->persist_copy(runtime, source, result) : false;
}

void oasp_component_gc_trace(oasp_allocator *allocator, const oasp_object *object) {
    const oasp_component_vtable *vtable = component_vtable(object);
    if (vtable && vtable->gc_trace)
        vtable->gc_trace(allocator, object);
}
