// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ext_internal.h"

#include <stdlib.h>
#include <string.h>

bool oasp_external_ascii_equal(oasp_slice left, const char *right) {
    size_t length = right ? strlen(right) : 0;
    if (left.length != length)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char a = (unsigned char)left.data[i];
        unsigned char b = (unsigned char)right[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

uint64_t oasp_external_ascii_hash(const char *data, size_t length) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)data[i];
        if (byte >= 'A' && byte <= 'Z')
            byte = (unsigned char)(byte + ('a' - 'A'));
        hash = (hash ^ byte) * UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
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

static void object_api_error(oasp_ext_error *error, int code, oasp_ext_error_category category, const char *message) {
    if (!error)
        return;
    *error = (oasp_ext_error){
        .struct_size = sizeof(*error),
        .code = code,
        .category = category,
        .message = message,
        .message_length = strlen(message),
        .source = "OpenASP.Extension",
        .source_length = 17
    };
}

static void external_request_destroy(void *context) {
    oasp_external_request_context *request_context = context;
    if (!request_context || !request_context->module)
        return;
    oasp_ext_call_context call;
    oasp_external_context_init(&call, request_context->runtime, request_context->module, NULL, NULL, 0);
    call.resource_owner = request_context;
    if (request_context->module->isolated)
        oasp_external_isolated_request_shutdown(&call, request_context->state);
    else if (request_context->module->descriptor->request_shutdown)
        request_context->module->descriptor->request_shutdown(&call, request_context->state);
    request_context->module = NULL;
    request_context->state = NULL;
}

bool oasp_external_request_state(oasp_runtime *runtime, oasp_external_module *module, size_t line, void **state, oasp_ext_error *error) {
    if (!runtime || !module || !state)
        return false;
    for (oasp_external_request_context *entry = runtime->external_request_contexts; entry; entry = entry->next) {
        if (entry->module == module) {
            *state = entry->state;
            return true;
        }
    }
    oasp_external_request_context *entry = oasp_runtime_alloc(
        runtime, sizeof(*entry), _Alignof(oasp_external_request_context));
    if (!entry)
        return false;
    memset(entry, 0, sizeof(*entry));
    entry->module = module;
    entry->runtime = runtime;
    oasp_ext_call_context call;
    oasp_external_context_init(&call, runtime, module, NULL, NULL, line);
    call.resource_owner = entry;
    int status = module->isolated
                     ? oasp_external_isolated_request_init(&call, &entry->state, error)
                     : module->descriptor->request_init
                           ? module->descriptor->request_init(
                                 &call, module->worker_state, &entry->state, error)
                           : OASP_EXT_OK;
    if (status != OASP_EXT_OK) {
        entry->module = NULL;
        return false;
    }
    entry->next = runtime->external_request_contexts;
    runtime->external_request_contexts = entry;
    if (!oasp_runtime_defer(runtime, entry, external_request_destroy, entry)) {
        external_request_destroy(entry);
        return false;
    }
    *state = entry->state;
    return true;
}

static oasp_ext_value_kind value_kind(const oasp_value *value) {
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

static void external_object_destroy(void *context) {
    oasp_external_object *object = context;
    if (!object || object->destroyed)
        return;
    object->destroyed = true;
    oasp_ext_call_context call;
    oasp_external_context_init(&call, object->handles.runtime, object->class_definition->module, object,
                               &object->handles, 0);
    if (object->instance && object->remote_proxy)
        oasp_external_isolated_destroy(&call, object->instance);
    else if (object->instance && object->class_definition->class_definition->destroy)
        object->class_definition->class_definition->destroy(&call, object->instance);
    object->instance = NULL;
    object->request_state = NULL;
    oasp_external_value_table_dispose(&object->handles);
}

int oasp_external_object_wrap_owned(oasp_ext_call_context *context, uint32_t class_id, void *instance, oasp_ext_value **result, oasp_ext_error *error) {
    if (!context || !context->runtime || !context->module || !instance || !result) {
        object_api_error(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid owned extension object");
        return OASP_EXT_ERROR;
    }
    if (context->wrap_owned)
        return context->wrap_owned(context, class_id, instance, result, error);
    oasp_external_registry *registry = context->runtime->component_context
                                          ? oasp_component_context_external_registry(context->runtime->component_context)
                                          : NULL;
    oasp_external_class *class_definition =
        oasp_external_class_find_id(registry, context->module, class_id);
    if (!class_definition) {
        object_api_error(error, 438, OASP_EXT_ERROR_ARGUMENT, "unknown extension class id");
        return OASP_EXT_ERROR;
    }
    oasp_external_object *external =
        oasp_runtime_alloc(context->runtime, sizeof(*external), _Alignof(oasp_external_object));
    oasp_object *object = oasp_runtime_alloc_object(context->runtime);
    if (!external || !object) {
        object_api_error(error, 7, OASP_EXT_ERROR_RESOURCE, "out of memory creating extension object");
        return OASP_EXT_ERROR;
    }
    memset(external, 0, sizeof(*external));
    external->class_definition = class_definition;
    external->instance = instance;
    external->remote_proxy = context->module->isolated;
    external->remote_id = external->remote_proxy ? (uint64_t)(uintptr_t)instance : 0;
    external->request_state = context->object ? context->object->request_state : NULL;
    external->handles.runtime = context->runtime;
    external->handles.owner = external;
    *object = (oasp_object){
        .component_kind = OASP_COMPONENT_EXTERNAL,
        .component_name = class_definition->class_definition->canonical_name,
        .native = external,
        .component_vtable = oasp_external_component_vtable()
    };
    if (!oasp_runtime_defer(context->runtime, external, external_object_destroy, external)) {
        external->instance = NULL;
        object_api_error(error, 7, OASP_EXT_ERROR_RESOURCE, "unable to register extension object lifetime");
        return OASP_EXT_ERROR;
    }
    *result = oasp_external_value_create(
        context, (oasp_value){.kind = OASP_VALUE_OBJECT, .as.object = object},
        OASP_EXT_VALUE_OBJECT);
    if (!*result) {
        external_object_destroy(external);
        object_api_error(error, 7, OASP_EXT_ERROR_RESOURCE, "out of memory creating extension object handle");
        return OASP_EXT_ERROR;
    }
    return OASP_EXT_OK;
}

bool oasp_external_object_remote_reference(const oasp_value *value, const oasp_external_module *module, uint32_t *class_id, uint64_t *remote_id) {
    oasp_external_object *external =
        value && value->kind == OASP_VALUE_OBJECT ? external_object(value->as.object) : NULL;
    if (!external || external->destroyed || external->class_definition->module != module ||
        !external->remote_id)
        return false;
    if (class_id)
        *class_id = external->class_definition->class_definition->class_id;
    if (remote_id)
        *remote_id = external->remote_id;
    return true;
}

int oasp_external_object_get_native(oasp_ext_call_context *context, const oasp_ext_value *value, uint32_t class_id, void **instance, oasp_ext_error *error) {
    oasp_value unwrapped = oasp_value_empty();
    if (!context || !instance || !oasp_external_value_unwrap(context, value, &unwrapped) ||
        unwrapped.kind != OASP_VALUE_OBJECT) {
        object_api_error(error, 13, OASP_EXT_ERROR_ARGUMENT, "value is not an extension object");
        return OASP_EXT_ERROR;
    }
    oasp_external_object *external = external_object(unwrapped.as.object);
    if (!external || external->destroyed ||
        external->class_definition->module != context->module ||
        external->class_definition->class_definition->class_id != class_id) {
        object_api_error(error, 13, OASP_EXT_ERROR_ARGUMENT, "extension object has incompatible type or owner");
        return OASP_EXT_ERROR;
    }
    *instance = external->instance;
    return *instance ? OASP_EXT_OK : OASP_EXT_ERROR;
}

bool oasp_external_component_create(oasp_runtime *runtime, oasp_component_context *component_context, oasp_slice prog_id, size_t line, oasp_value *result) {
    oasp_external_registry *registry = oasp_component_context_external_registry(component_context);
    oasp_external_class *class_definition = registry ? oasp_external_class_find(registry, prog_id) : NULL;
    if (!class_definition)
        return false;
    oasp_external_object *external = oasp_runtime_alloc(runtime, sizeof(*external), _Alignof(oasp_external_object));
    oasp_object *object = oasp_runtime_alloc_object(runtime);
    if (!external || !object)
        return true;
    memset(external, 0, sizeof(*external));
    external->class_definition = class_definition;
    external->handles.runtime = runtime;
    external->handles.owner = external;
    oasp_ext_call_context call;
    oasp_external_context_init(&call, runtime, class_definition->module, external, &external->handles, line);
    oasp_ext_error error = {.struct_size = sizeof(error)};
    if (!oasp_external_request_state(
            runtime, class_definition->module, line, &external->request_state, &error)) {
        oasp_external_error(runtime, &error, "external request initialization failed", line);
        return true;
    }
    int status = class_definition->module->isolated
                     ? oasp_external_isolated_create(&call, class_definition->class_definition,
                                                     external->request_state, &external->instance, &error)
                     : class_definition->class_definition->create(
                           &call, class_definition->module->worker_state, external->request_state,
                           &external->instance, &error);
    if (status != OASP_EXT_OK || !external->instance) {
        oasp_external_error(runtime, &error, "external component create failed", line);
        external_object_destroy(external);
        return true;
    }
    external->remote_proxy = class_definition->module->isolated;
    external->remote_id = external->remote_proxy ? (uint64_t)(uintptr_t)external->instance : 0;
    memset(object, 0, sizeof(*object));
    object->component_kind = OASP_COMPONENT_EXTERNAL;
    object->component_name = class_definition->class_definition->canonical_name;
    object->native = external;
    object->component_vtable = oasp_external_component_vtable();
    if (!oasp_runtime_defer(runtime, external, external_object_destroy, external)) {
        external_object_destroy(external);
        return true;
    }
    *result = (oasp_value){.kind = OASP_VALUE_OBJECT, .as.object = object};
    return true;
}

static void release_value(oasp_ext_call_context *context, oasp_ext_value *value) {
    if (value)
        oasp_external_value_api()->release(context, value);
}

static bool stream_collect(oasp_ext_call_context *context, oasp_ext_stream *stream, oasp_value *result, oasp_ext_error *error) {
    size_t count = 0;
    size_t capacity = 0;
    oasp_value *items = NULL;
    int status = OASP_EXT_PENDING;
    while (status == OASP_EXT_PENDING || status == OASP_EXT_OK) {
        if (oasp_external_context_cancelled(context)) {
            if (stream->vtable->cancel)
                stream->vtable->cancel(stream->state);
            status = OASP_EXT_CANCELLED;
            break;
        }
        oasp_ext_value *item = NULL;
        status = stream->vtable->next(stream->state, context, &item, error);
        if (status == OASP_EXT_PENDING) {
            oasp_external_async_api()->yield(context, 1);
            continue;
        }
        if (status == OASP_EXT_OK) {
            if (!item)
                break;
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 8;
                oasp_value *grown = realloc(items, next * sizeof(*grown));
                if (!grown) {
                    status = OASP_EXT_ERROR;
                    release_value(context, item);
                    break;
                }
                items = grown;
                capacity = next;
            }
            if (!oasp_external_value_unwrap(context, item, &items[count++]))
                status = OASP_EXT_ERROR;
            release_value(context, item);
        }
    }
    stream->completed = true;
    stream->vtable->destroy(stream->state);
    free(stream);
    if (status != OASP_EXT_END) {
        free(items);
        return false;
    }
    oasp_array *array = oasp_runtime_alloc_array(context->runtime);
    int64_t *bounds = oasp_runtime_alloc(context->runtime, sizeof(*bounds), _Alignof(int64_t));
    oasp_value *managed = count ? oasp_runtime_alloc_values(context->runtime, count) : NULL;
    if (!array || !bounds || (count && !managed)) {
        free(items);
        return false;
    }
    if (count)
        memcpy(managed, items, count * sizeof(*managed));
    free(items);
    *bounds = count ? (int64_t)count - 1 : -1;
    *array = (oasp_array){.rank = 1, .length = count, .bounds = bounds, .items = managed};
    *result = (oasp_value){.kind = OASP_VALUE_ARRAY, .as.array = array};
    return true;
}

int oasp_external_await_result(oasp_ext_call_context *context, oasp_ext_result *extension_result, oasp_value *result, oasp_ext_error *error) {
    if (extension_result->value) {
        bool ok = oasp_external_value_unwrap(context, extension_result->value, result);
        release_value(context, extension_result->value);
        extension_result->value = NULL;
        return ok ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (extension_result->stream)
        return stream_collect(context, extension_result->stream, result, error) ? OASP_EXT_OK : OASP_EXT_ERROR;
    if (!extension_result->future) {
        *result = oasp_value_empty();
        return OASP_EXT_OK;
    }
    oasp_ext_future *future = extension_result->future;
    for (;;) {
        if (oasp_external_context_cancelled(context)) {
            if (future->vtable->cancel)
                future->vtable->cancel(future->state);
            future->vtable->destroy(future->state);
            free(future);
            return OASP_EXT_CANCELLED;
        }
        oasp_ext_value *value = NULL;
        oasp_ext_async_state state = future->vtable->poll(future->state, context, &value, error);
        if (state == OASP_EXT_ASYNC_PENDING) {
            oasp_external_async_api()->yield(context, 1);
            continue;
        }
        future->vtable->destroy(future->state);
        free(future);
        if (state != OASP_EXT_ASYNC_READY)
            return state == OASP_EXT_ASYNC_CANCELLED ? OASP_EXT_CANCELLED : OASP_EXT_ERROR;
        bool ok = value && oasp_external_value_unwrap(context, value, result);
        release_value(context, value);
        return ok ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
}

static bool external_invoke(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *assigned, oasp_value *arguments,
                            size_t count, size_t line, oasp_ext_operation operation, oasp_value *result) {
    oasp_external_object *external = external_object(object);
    const oasp_extension_member *entry = external && member.length
                                              ? oasp_external_member_find(external->class_definition, member) : NULL;
    if (!external || (member.length && !entry))
        return false;
    uint32_t required_flag = operation == OASP_EXT_OP_GET ? OASP_EXT_MEMBER_GET
                             : operation == OASP_EXT_OP_SET ? OASP_EXT_MEMBER_SET
                             : operation == OASP_EXT_OP_CALL ? OASP_EXT_MEMBER_CALL : 0;
    if (entry && required_flag && !(entry->flags & required_flag))
        return false;
    oasp_ext_call_context call;
    oasp_external_context_init(&call, runtime, external->class_definition->module, external, &external->handles, line);
    oasp_ext_error error = {.struct_size = sizeof(error)};
    oasp_ext_value *assigned_handle = assigned ? oasp_external_value_create(&call, *assigned, value_kind(assigned)) : NULL;
    oasp_ext_value **argument_handles = count ? calloc(count, sizeof(*argument_handles)) : NULL;
    const oasp_ext_value **argument_views = count ? calloc(count, sizeof(*argument_views)) : NULL;
    if ((assigned && !assigned_handle) || (count && (!argument_handles || !argument_views)))
        goto allocation_failed;
    for (size_t i = 0; i < count; i++) {
        argument_handles[i] = oasp_external_value_create(&call, arguments[i], value_kind(&arguments[i]));
        argument_views[i] = argument_handles[i];
        if (!argument_handles[i])
            goto allocation_failed;
    }
    oasp_ext_invocation invocation = {
        .struct_size = sizeof(invocation),
        .operation = operation,
        .member_id = entry ? entry->member_id : 0,
        .value = assigned_handle,
        .arguments = argument_views,
        .argument_count = count
    };
    oasp_ext_result extension_result = {.struct_size = sizeof(extension_result)};
    oasp_value isolated_result = oasp_value_empty();
    int status = external->class_definition->module->isolated
                     ? oasp_external_isolated_invoke(&call, external->instance, &invocation,
                                                     &isolated_result, &error)
                     : external->class_definition->class_definition->invoke(
                           &call, external->instance, &invocation, &extension_result, &error);
    for (size_t i = 0; i < count; i++)
        release_value(&call, argument_handles[i]);
    release_value(&call, assigned_handle);
    free(argument_views);
    free(argument_handles);
    oasp_value discarded = oasp_value_empty();
    if (status == OASP_EXT_OK && external->class_definition->module->isolated)
        *(result ? result : &discarded) = isolated_result;
    else if (status == OASP_EXT_OK)
        status = oasp_external_await_result(&call, &extension_result, result ? result : &discarded, &error);
    if (status == OASP_EXT_OK)
        return true;
    if (status == OASP_EXT_CANCELLED && !error.message)
        error = (oasp_ext_error){.struct_size = sizeof(error), .code = 258, .category = OASP_EXT_ERROR_TIMEOUT,
                                 .message = "external component operation cancelled", .message_length = 38};
    oasp_external_error(runtime, &error, "external component operation failed", line);
    return true;

allocation_failed:
    for (size_t i = 0; i < count; i++)
        release_value(&call, argument_handles ? argument_handles[i] : NULL);
    release_value(&call, assigned_handle);
    free(argument_views);
    free(argument_handles);
    oasp_runtime_raise(runtime, 7, "out of memory while invoking external component", "OpenASP.Extension", line);
    return true;
}

bool oasp_external_component_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result) {
    return external_invoke(runtime, object, member, NULL, NULL, 0, line, OASP_EXT_OP_GET, result);
}

bool oasp_external_component_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    return external_invoke(runtime, object, member, value, NULL, 0, line, OASP_EXT_OP_SET, NULL);
}

bool oasp_external_component_assign(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t count,
                                    const oasp_value *value, size_t line) {
    return external_invoke(runtime, object, member, value, arguments, count, line,
                           member.length ? OASP_EXT_OP_INDEX_SET : OASP_EXT_OP_DEFAULT_SET, NULL);
}

bool oasp_external_component_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t count,
                                  size_t line, oasp_value *result) {
    return external_invoke(runtime, object, member, NULL, arguments, count, line, OASP_EXT_OP_CALL, result);
}

bool oasp_external_component_default(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t count, size_t line,
                                     oasp_value *result) {
    return external_invoke(runtime, object, (oasp_slice){"", 0}, NULL, arguments, count, line,
                           OASP_EXT_OP_DEFAULT_GET, result);
}

bool oasp_external_component_enumerate(oasp_runtime *runtime, oasp_object *object, oasp_value *result) {
    return external_invoke(runtime, object, (oasp_slice){"", 0}, NULL, NULL, 0, 0, OASP_EXT_OP_ENUMERATE, result);
}

bool oasp_external_component_persist_copy(oasp_runtime *runtime, const oasp_object *source, oasp_object **result) {
    oasp_external_object *external = external_object(source);
    if (!external || !(external->class_definition->class_definition->flags & OASP_EXT_CLASS_PERSISTABLE) ||
        !external->class_definition->class_definition->snapshot || !external->class_definition->class_definition->restore)
        return false;
    oasp_ext_call_context call;
    oasp_external_context_init(&call, runtime, external->class_definition->module, external, &external->handles, 0);
    oasp_ext_error error = {.struct_size = sizeof(error)};
    const oasp_extension_class *class_definition = external->class_definition->class_definition;
    if (external->class_definition->module->isolated) {
        oasp_value state = oasp_value_empty();
        if (oasp_external_isolated_snapshot(&call, external->instance, &state, &error) != OASP_EXT_OK)
            return false;
        void *remote_instance = NULL;
        if (oasp_external_isolated_create(
                &call, class_definition, external->request_state, &remote_instance, &error) != OASP_EXT_OK)
            return false;
        oasp_ext_value *wrapped = NULL;
        if (oasp_external_object_wrap_owned(
                &call, class_definition->class_id, remote_instance, &wrapped, &error) != OASP_EXT_OK) {
            oasp_external_isolated_destroy(&call, remote_instance);
            return false;
        }
        oasp_value created = oasp_value_empty();
        bool unwrapped = oasp_external_value_unwrap(&call, wrapped, &created);
        release_value(&call, wrapped);
        if (!unwrapped || created.kind != OASP_VALUE_OBJECT)
            return false;
        oasp_external_object *copy = external_object(created.as.object);
        oasp_ext_call_context copy_call;
        oasp_external_context_init(&copy_call, runtime, copy->class_definition->module, copy, &copy->handles, 0);
        if (oasp_external_isolated_restore(&copy_call, copy->instance, &state, &error) != OASP_EXT_OK) {
            external_object_destroy(copy);
            return false;
        }
        *result = created.as.object;
        return true;
    }
    oasp_ext_value *state = NULL;
    if (class_definition->snapshot(&call, external->instance, &state, &error) != OASP_EXT_OK || !state)
        return false;
    void *native_instance = NULL;
    int status = class_definition->restore(
        &call, external->class_definition->module->worker_state, external->request_state,
        state, &native_instance, &error);
    release_value(&call, state);
    if (status != OASP_EXT_OK || !native_instance)
        return false;
    oasp_ext_value *wrapped = NULL;
    if (oasp_external_object_wrap_owned(
            &call, class_definition->class_id, native_instance, &wrapped, &error) != OASP_EXT_OK) {
        class_definition->destroy(&call, native_instance);
        return false;
    }
    oasp_value created = oasp_value_empty();
    bool unwrapped = oasp_external_value_unwrap(&call, wrapped, &created);
    release_value(&call, wrapped);
    if (!unwrapped || created.kind != OASP_VALUE_OBJECT) {
        return false;
    }
    *result = created.as.object;
    return true;
}

void oasp_external_component_gc_trace(oasp_allocator *allocator, const oasp_object *object) {
    oasp_external_object *external = external_object(object);
    if (external)
        oasp_external_value_table_trace(allocator, &external->handles);
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
