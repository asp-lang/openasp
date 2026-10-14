// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ext_internal.h"

#include <string.h>

bool oasp_external_ascii_equal(oasp_slice left, const char *right) {
    size_t length = strlen(right);
    if (left.length != length)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char a = (unsigned char)left.data[i], b = (unsigned char)right[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

void oasp_external_error(oasp_runtime *runtime, const oasp_ext_error *error, const char *fallback, size_t line) {
    const char *message = error && error->message ? error->message : fallback;
    const char *source = error && error->source ? error->source : "OpenASP.Extension";
    int code = error && error->code > 0 ? error->code : 5;
    oasp_runtime_raise(runtime, code, message, source, line);
}

static oasp_external_object *external_object(const oasp_object *object) {
    return object && object->component_kind == OASP_COMPONENT_EXTERNAL ? object->native : NULL;
}

static void external_object_destroy(void *context) {
    oasp_external_object *object = context;
    if (object && object->instance && object->class_definition->class_definition->destroy)
        object->class_definition->class_definition->destroy(object->instance);
    if (object)
        object->instance = NULL;
}

bool oasp_external_component_create(oasp_runtime *runtime, oasp_component_context *context, oasp_slice prog_id, size_t line, oasp_value *result) {
    oasp_external_registry *registry = oasp_component_context_external_registry(context);
    if (!registry)
        return false;
    for (size_t i = 0; i < registry->class_count; i++) {
        oasp_external_class *class_definition = &registry->classes[i];
        if (!oasp_external_ascii_equal(prog_id, class_definition->class_definition->prog_id))
            continue;
        oasp_external_object *external = oasp_runtime_alloc(runtime, sizeof(*external), _Alignof(oasp_external_object));
        oasp_object *object = oasp_runtime_alloc_object(runtime);
        if (!external || !object)
            return true;
        memset(external, 0, sizeof(*external));
        oasp_ext_error error = {sizeof(error), 0, NULL, NULL};
        oasp_external_call_enter(runtime, external);
        int status = class_definition->class_definition->create(class_definition->module->worker_state, &external->instance, &error);
        oasp_external_call_leave();
        if (status || !external->instance) {
            oasp_external_error(runtime, &error, "external component create failed", line);
            return true;
        }
        external->class_definition = class_definition;
        memset(object, 0, sizeof(*object));
        object->component_kind = OASP_COMPONENT_EXTERNAL;
        object->component_name = class_definition->class_definition->canonical_name;
        object->native = external;
        object->component_vtable = oasp_external_component_vtable();
        if (!oasp_runtime_defer(runtime, external, external_object_destroy, external))
            return true;
        *result = (oasp_value){.kind = OASP_VALUE_OBJECT, .as.object = object};
        return true;
    }
    return false;
}

static const oasp_extension_member *find_member(const oasp_external_object *object, oasp_slice member) {
    const oasp_extension_class *class_definition = object->class_definition->class_definition;
    for (size_t i = 0; i < class_definition->member_count; i++)
        if (oasp_external_ascii_equal(member, class_definition->members[i].name))
            return &class_definition->members[i];
    return NULL;
}

static bool external_invoke(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, oasp_value *arguments,
                            size_t count, size_t line, int operation, oasp_value *result) {
    oasp_external_object *external = external_object(object);
    const oasp_extension_member *entry = external ? find_member(external, member) : NULL;
    if (!entry)
        return false;
    oasp_ext_error error = {sizeof(error), 0, NULL, NULL};
    oasp_value_handle_impl input = {value ? *value : oasp_value_empty(), 1, false};
    oasp_value_handle_impl *handles = count ? oasp_runtime_alloc(runtime, count * sizeof(*handles), _Alignof(oasp_value_handle_impl)) : NULL;
    const oasp_value_handle **views = count ? oasp_runtime_alloc(runtime, count * sizeof(*views), _Alignof(oasp_value_handle *)) : NULL;
    if (count && (!handles || !views))
        return true;
    for (size_t i = 0; i < count; i++) {
        handles[i] = (oasp_value_handle_impl){arguments[i], 1, false};
        views[i] = (oasp_value_handle *)&handles[i];
    }
    oasp_value_handle *output = NULL;
    oasp_external_call_enter(runtime, external);
    int status = operation == 0 ? external->class_definition->class_definition->get(external->instance, entry->member_id, &output, &error)
               : operation == 1 ? external->class_definition->class_definition->set(external->instance, entry->member_id, (oasp_value_handle *)&input, &error)
               : external->class_definition->class_definition->call(external->instance, entry->member_id, views, count, &output, &error);
    oasp_external_call_leave();
    if (status) {
        oasp_external_error(runtime, &error, "external component call failed", line);
        return true;
    }
    if (result)
        *result = output ? ((oasp_value_handle_impl *)output)->value : oasp_value_empty();
    return true;
}

bool oasp_external_component_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result) {
    return external_invoke(runtime, object, member, NULL, NULL, 0, line, 0, result);
}

bool oasp_external_component_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    return external_invoke(runtime, object, member, value, NULL, 0, line, 1, NULL);
}

bool oasp_external_component_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t count,
                                  size_t line, oasp_value *result) {
    return external_invoke(runtime, object, member, NULL, arguments, count, line, 2, result);
}

bool oasp_external_component_default(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t count, size_t line,
                                     oasp_value *result) {
    return oasp_external_component_call(runtime, object, (oasp_slice){"", 0}, arguments, count, line, result);
}

bool oasp_external_component_enumerate(oasp_runtime *runtime, oasp_object *object, oasp_value *result) {
    (void)runtime;
    (void)object;
    (void)result;
    return false;
}

bool oasp_external_component_persist_copy(oasp_runtime *runtime, const oasp_object *source, oasp_object **result) {
    (void)runtime;
    (void)source;
    (void)result;
    return false;
}

static void external_trace_value(void *context, const oasp_value_handle *value) {
    oasp_allocator *collector = context;
    const oasp_value_handle_impl *handle = (const oasp_value_handle_impl *)value;
    if (!handle)
        return;
    if (handle->value.kind == OASP_VALUE_STRING)
        oasp_allocator_trace(collector, handle->value.as.string.data);
    else if (handle->value.kind == OASP_VALUE_DATE)
        oasp_allocator_trace(collector, handle->value.as.date.data);
    else if (handle->value.kind == OASP_VALUE_ARRAY)
        oasp_allocator_trace(collector, handle->value.as.array);
    else if (handle->value.kind == OASP_VALUE_OBJECT)
        oasp_allocator_trace(collector, handle->value.as.object);
}

void oasp_external_component_gc_trace(oasp_allocator *allocator, const oasp_object *object) {
    oasp_external_object *external = external_object(object);
    if (!external || !external->instance || !external->class_definition->class_definition->trace)
        return;
    external->class_definition->class_definition->trace(external->instance, external_trace_value, allocator);
}

static bool external_vtable_call(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, oasp_value *arguments,
                                 size_t argument_count, size_t line, oasp_value *result) {
    (void)member_id;
    return oasp_external_component_call(runtime, object, member, arguments, argument_count, line, result);
}

static const oasp_component_vtable external_vtable = {
    oasp_external_component_get, NULL, oasp_external_component_set, external_vtable_call,
    oasp_external_component_default, oasp_external_component_enumerate, oasp_external_component_persist_copy,
    oasp_external_component_gc_trace
};

const oasp_component_vtable *oasp_external_component_vtable(void) {
    return &external_vtable;
}
