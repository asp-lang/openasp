// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include "ext_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

enum {
    IPC_CREATE = 1,
    IPC_INVOKE,
    IPC_DESTROY,
    IPC_SNAPSHOT,
    IPC_RESTORE,
    IPC_REQUEST_INIT,
    IPC_REQUEST_SHUTDOWN,
    IPC_STOP
};

typedef struct {
    uint32_t operation;
    uint32_t class_id;
    uint64_t instance_id;
    uint64_t request_id;
    uint32_t invoke_operation;
    uint32_t member_id;
    uint32_t argument_count;
    uint32_t has_value;
    uint64_t payload_length;
    uint64_t deadline_milliseconds;
} ipc_request;

typedef struct {
    int32_t status;
    int32_t error_code;
    int32_t error_category;
    uint32_t reserved;
    uint64_t instance_id;
    uint64_t message_length;
    uint64_t source_length;
    uint64_t detail_length;
    uint64_t payload_length;
} ipc_response;

typedef struct {
    int fd;
    pid_t pid;
    uint64_t next_request_id;
    bool failed;
} isolated_proxy;

typedef struct {
    uint64_t id;
    uint64_t request_id;
    oasp_external_class class_definition;
    oasp_external_object object;
    oasp_object *value_object;
    void *request_state;
} isolated_instance;

typedef struct {
    isolated_instance *instances;
    size_t instance_count;
    uint64_t *next_id;
    uint64_t request_id;
    void *request_state;
} isolated_object_registry;

typedef struct {
    uint64_t id;
    oasp_request *request;
    oasp_runtime *runtime;
    void *state;
    bool runtime_initialized;
} isolated_request;

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t offset;
} wire_reader;

static bool write_all(int fd, const void *data, size_t length) {
    const unsigned char *bytes = data;
    while (length) {
#if defined(MSG_NOSIGNAL)
        ssize_t written = send(fd, bytes, length, MSG_NOSIGNAL);
#else
        ssize_t written = send(fd, bytes, length, 0);
#endif
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        bytes += (size_t)written;
        length -= (size_t)written;
    }
    return true;
}

static void suppress_socket_sigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
    (void)fd;
#endif
}

static int64_t monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return 0;
    if (now.tv_sec > INT64_MAX / 1000)
        return INT64_MAX;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool wait_socket(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t remaining = deadline - monotonic_milliseconds();
        if (remaining <= 0)
            return false;
        int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd descriptor = {.fd = fd, .events = events};
        int result = poll(&descriptor, 1, timeout);
        if (result > 0)
            return (descriptor.revents & (events | POLLHUP)) != 0;
        if (!result)
            return false;
        if (errno != EINTR)
            return false;
    }
}

static bool timed_write_all(int fd, const void *data, size_t length, int64_t deadline) {
    const unsigned char *bytes = data;
    while (length) {
        int flags = 0;
#if defined(MSG_NOSIGNAL)
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t written = send(fd, bytes, length, flags);
        if (written > 0) {
            bytes += (size_t)written;
            length -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            wait_socket(fd, POLLOUT, deadline))
            continue;
        return false;
    }
    return true;
}

static bool timed_read_all(int fd, void *data, size_t length, int64_t deadline) {
    unsigned char *bytes = data;
    while (length) {
        ssize_t received = recv(fd, bytes, length, 0);
        if (received > 0) {
            bytes += (size_t)received;
            length -= (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            wait_socket(fd, POLLIN, deadline))
            continue;
        return false;
    }
    return true;
}

static bool read_all(int fd, void *data, size_t length) {
    unsigned char *bytes = data;
    while (length) {
        ssize_t received = read(fd, bytes, length);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0)
            return false;
        bytes += (size_t)received;
        length -= (size_t)received;
    }
    return true;
}

static bool append_u32(oasp_buffer *buffer, uint32_t value) {
    return oasp_buffer_append(buffer, &value, sizeof(value));
}

static bool append_u64(oasp_buffer *buffer, uint64_t value) {
    return oasp_buffer_append(buffer, &value, sizeof(value));
}

static bool read_bytes(wire_reader *reader, void *result, size_t length) {
    if (length > reader->length - reader->offset)
        return false;
    memcpy(result, reader->data + reader->offset, length);
    reader->offset += length;
    return true;
}

static bool read_u32(wire_reader *reader, uint32_t *result) {
    return read_bytes(reader, result, sizeof(*result));
}

static bool read_u64(wire_reader *reader, uint64_t *result) {
    return read_bytes(reader, result, sizeof(*result));
}

static bool append_string(oasp_buffer *buffer, const char *value) {
    size_t length = value ? strlen(value) : 0;
    return append_u64(buffer, length) && oasp_buffer_append(buffer, value, length);
}

static char *read_string(wire_reader *reader, size_t maximum) {
    uint64_t length;
    if (!read_u64(reader, &length) || !length || length > maximum || length > reader->length - reader->offset)
        return NULL;
    char *value = malloc((size_t)length + 1);
    if (!value)
        return NULL;
    memcpy(value, reader->data + reader->offset, (size_t)length);
    value[length] = '\0';
    reader->offset += (size_t)length;
    return value;
}

static bool encode_descriptor(oasp_buffer *buffer, const oasp_extension_descriptor *descriptor) {
    if (!append_string(buffer, descriptor->module_name) ||
        !append_string(buffer, descriptor->module_version) ||
        !append_u64(buffer, descriptor->required_capabilities) ||
        !append_u64(buffer, descriptor->class_count))
        return false;
    for (size_t i = 0; i < descriptor->class_count; i++) {
        const oasp_extension_class *class_definition = &descriptor->classes[i];
        if (!append_string(buffer, class_definition->prog_id) ||
            !append_string(buffer, class_definition->canonical_name) ||
            !append_u32(buffer, class_definition->class_id) ||
            !append_u32(buffer, class_definition->flags) ||
            !append_u64(buffer, class_definition->member_count))
            return false;
        for (size_t j = 0; j < class_definition->member_count; j++) {
            const oasp_extension_member *member = &class_definition->members[j];
            if (!append_string(buffer, member->name) || !append_u32(buffer, member->member_id) ||
                !append_u32(buffer, member->flags))
                return false;
        }
    }
    return true;
}

static int proxy_dummy_create(oasp_ext_call_context *context, void *worker_state, void *request_state, void **instance, oasp_ext_error *error) {
    (void)context;
    (void)worker_state;
    (void)request_state;
    (void)instance;
    (void)error;
    return OASP_EXT_ERROR;
}

static void proxy_dummy_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    (void)instance;
}

static int proxy_dummy_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    (void)context;
    (void)instance;
    (void)invocation;
    (void)result;
    (void)error;
    return OASP_EXT_ERROR;
}

static int proxy_dummy_snapshot(oasp_ext_call_context *context, void *instance, oasp_ext_value **state, oasp_ext_error *error) {
    (void)context;
    (void)instance;
    (void)state;
    (void)error;
    return OASP_EXT_ERROR;
}

static int proxy_dummy_restore(oasp_ext_call_context *context, void *worker_state, void *request_state, const oasp_ext_value *state, void **instance,
                               oasp_ext_error *error) {
    (void)context;
    (void)worker_state;
    (void)request_state;
    (void)state;
    (void)instance;
    (void)error;
    return OASP_EXT_ERROR;
}

static void free_proxy_descriptor(const oasp_extension_descriptor *descriptor) {
    if (!descriptor)
        return;
    oasp_extension_class *classes = (oasp_extension_class *)descriptor->classes;
    for (size_t i = 0; i < descriptor->class_count; i++) {
        oasp_extension_member *members = (oasp_extension_member *)classes[i].members;
        for (size_t j = 0; j < classes[i].member_count; j++)
            free((char *)members[j].name);
        free(members);
        free((char *)classes[i].prog_id);
        free((char *)classes[i].canonical_name);
    }
    free(classes);
    free((char *)descriptor->module_name);
    free((char *)descriptor->module_version);
    free((oasp_extension_descriptor *)descriptor);
}

static oasp_extension_descriptor *decode_descriptor(const void *data, size_t length) {
    wire_reader reader = {data, length, 0};
    oasp_extension_descriptor *descriptor = calloc(1, sizeof(*descriptor));
    uint64_t class_count;
    if (!descriptor)
        return NULL;
    descriptor->struct_size = sizeof(*descriptor);
    descriptor->abi_major_required = OASP_EXTENSION_ABI_MAJOR;
    descriptor->abi_minor_required = OASP_EXTENSION_ABI_MINOR;
    descriptor->module_name = read_string(&reader, 127);
    descriptor->module_version = read_string(&reader, 63);
    if (!descriptor->module_name || !descriptor->module_version ||
        !read_u64(&reader, &descriptor->required_capabilities) ||
        !read_u64(&reader, &class_count) || !class_count || class_count > 4096) {
        free_proxy_descriptor(descriptor);
        return NULL;
    }
    descriptor->class_count = (size_t)class_count;
    oasp_extension_class *classes = calloc(descriptor->class_count, sizeof(*classes));
    descriptor->classes = classes;
    if (!classes) {
        free_proxy_descriptor(descriptor);
        return NULL;
    }
    for (size_t i = 0; i < descriptor->class_count; i++) {
        uint64_t member_count;
        classes[i].struct_size = sizeof(classes[i]);
        classes[i].prog_id = read_string(&reader, 255);
        classes[i].canonical_name = read_string(&reader, 255);
        if (!classes[i].prog_id || !classes[i].canonical_name ||
            !read_u32(&reader, &classes[i].class_id) || !read_u32(&reader, &classes[i].flags) ||
            !read_u64(&reader, &member_count) || member_count > 65535) {
            free_proxy_descriptor(descriptor);
            return NULL;
        }
        classes[i].member_count = (size_t)member_count;
        oasp_extension_member *members = calloc(classes[i].member_count, sizeof(*members));
        classes[i].members = members;
        if (classes[i].member_count && !members) {
            free_proxy_descriptor(descriptor);
            return NULL;
        }
        for (size_t j = 0; j < classes[i].member_count; j++) {
            members[j].struct_size = sizeof(members[j]);
            members[j].name = read_string(&reader, 255);
            if (!members[j].name || !read_u32(&reader, &members[j].member_id) ||
                !read_u32(&reader, &members[j].flags)) {
                free_proxy_descriptor(descriptor);
                return NULL;
            }
        }
        classes[i].create = proxy_dummy_create;
        classes[i].destroy = proxy_dummy_destroy;
        classes[i].invoke = proxy_dummy_invoke;
        if (classes[i].flags & OASP_EXT_CLASS_PERSISTABLE) {
            classes[i].snapshot = proxy_dummy_snapshot;
            classes[i].restore = proxy_dummy_restore;
        }
    }
    if (reader.offset != reader.length) {
        free_proxy_descriptor(descriptor);
        return NULL;
    }
    return descriptor;
}

enum {
    WIRE_OBJECT_DICTIONARY,
    WIRE_OBJECT_EXTENSION
};

static isolated_instance *find_instance(isolated_instance *instances, size_t count, uint64_t id);

static bool wire_encode_value(oasp_buffer *buffer, const oasp_value *value, size_t depth, const oasp_external_module *module) {
    if (!value || depth > 32 || !append_u32(buffer, (uint32_t)value->kind))
        return false;
    switch (value->kind) {
    case OASP_VALUE_EMPTY:
    case OASP_VALUE_NULL:
    case OASP_VALUE_NOTHING:
        return true;
    case OASP_VALUE_BOOL:
        return append_u32(buffer, value->as.boolean ? 1u : 0u);
    case OASP_VALUE_INT:
        return append_u64(buffer, (uint64_t)value->as.integer);
    case OASP_VALUE_FLOAT: {
        uint64_t bits;
        memcpy(&bits, &value->as.floating, sizeof(bits));
        return append_u64(buffer, bits);
    }
    case OASP_VALUE_STRING:
    case OASP_VALUE_DATE: {
        oasp_slice slice = value->kind == OASP_VALUE_STRING ? value->as.string : value->as.date;
        return append_u64(buffer, slice.length) && oasp_buffer_append(buffer, slice.data, slice.length);
    }
    case OASP_VALUE_ARRAY:
        if (!value->as.array || !append_u64(buffer, value->as.array->rank) ||
            !append_u64(buffer, value->as.array->length) ||
            !oasp_buffer_append(buffer, value->as.array->bounds,
                                value->as.array->rank * sizeof(*value->as.array->bounds)))
            return false;
        for (size_t i = 0; i < value->as.array->length; i++)
            if (!wire_encode_value(buffer, &value->as.array->items[i], depth + 1, module))
                return false;
        return true;
    case OASP_VALUE_OBJECT: {
        uint32_t class_id;
        uint64_t remote_id;
        if (oasp_external_object_remote_reference(value, module, &class_id, &remote_id))
            return append_u32(buffer, WIRE_OBJECT_EXTENSION) &&
                   append_u32(buffer, class_id) && append_u64(buffer, remote_id);
        int compare_mode = 0;
        size_t count = 0;
        if (!oasp_component_dictionary_info(value->as.object, &compare_mode, &count) ||
            !append_u32(buffer, WIRE_OBJECT_DICTIONARY) ||
            !append_u32(buffer, (uint32_t)compare_mode) || !append_u64(buffer, count))
            return false;
        for (size_t i = 0; i < count; i++) {
            const oasp_value *key = NULL;
            const oasp_value *item = NULL;
            if (!oasp_component_dictionary_item(value->as.object, i, &key, &item) ||
                !wire_encode_value(buffer, key, depth + 1, module) ||
                !wire_encode_value(buffer, item, depth + 1, module))
                return false;
        }
        return true;
    }
    }
    return false;
}

static bool wire_encode_invocation_value(oasp_buffer *buffer, const oasp_value *value, oasp_ext_call_context *context) {
    size_t checkpoint = buffer->length;
    if (wire_encode_value(buffer, value, 0, context->module))
        return true;
    buffer->length = checkpoint;
    if (!value || value->kind != OASP_VALUE_OBJECT || !value->as.object ||
        value->as.object->component_kind != OASP_COMPONENT_NONE)
        return false;
    oasp_value native = oasp_runtime_get_member(
        context->runtime, value, (oasp_slice){"NativeModule", 12}, context->line);
    return !context->runtime->error_pending &&
           wire_encode_value(buffer, &native, 0, context->module);
}

static bool wire_decode_value(wire_reader *reader, oasp_ext_call_context *context, isolated_object_registry *registry, oasp_value *result, size_t depth) {
    uint32_t kind = 0;
    oasp_runtime *runtime = context ? context->runtime : NULL;
    if (!runtime || !result || depth > 32 || !read_u32(reader, &kind) || kind > OASP_VALUE_DATE)
        return false;
    *result = oasp_value_empty();
    result->kind = (oasp_value_kind)kind;
    switch (result->kind) {
    case OASP_VALUE_EMPTY:
    case OASP_VALUE_NULL:
    case OASP_VALUE_NOTHING:
        return true;
    case OASP_VALUE_BOOL: {
        uint32_t value;
        if (!read_u32(reader, &value) || value > 1)
            return false;
        result->as.boolean = value != 0;
        return true;
    }
    case OASP_VALUE_INT: {
        uint64_t value;
        if (!read_u64(reader, &value))
            return false;
        result->as.integer = (int64_t)value;
        return true;
    }
    case OASP_VALUE_FLOAT: {
        uint64_t bits;
        if (!read_u64(reader, &bits))
            return false;
        memcpy(&result->as.floating, &bits, sizeof(bits));
        return true;
    }
    case OASP_VALUE_STRING:
    case OASP_VALUE_DATE: {
        uint64_t length;
        if (!read_u64(reader, &length) || length > SIZE_MAX || length > reader->length - reader->offset)
            return false;
        char *copy = oasp_runtime_copy(runtime, (const char *)reader->data + reader->offset, (size_t)length);
        if (!copy)
            return false;
        reader->offset += (size_t)length;
        if (result->kind == OASP_VALUE_STRING)
            result->as.string = (oasp_slice){copy, (size_t)length};
        else
            result->as.date = (oasp_slice){copy, (size_t)length};
        return true;
    }
    case OASP_VALUE_ARRAY: {
        uint64_t rank;
        uint64_t length;
        if (!read_u64(reader, &rank) || !read_u64(reader, &length) || !rank || rank > 32 ||
            length > SIZE_MAX || rank > SIZE_MAX / sizeof(int64_t))
            return false;
        oasp_array *array = oasp_runtime_alloc_array(runtime);
        int64_t *bounds = oasp_runtime_alloc(runtime, (size_t)rank * sizeof(*bounds), _Alignof(int64_t));
        oasp_value *items = length ? oasp_runtime_alloc_values(runtime, (size_t)length) : NULL;
        if (!array || !bounds || (length && !items) ||
            !read_bytes(reader, bounds, (size_t)rank * sizeof(*bounds)))
            return false;
        *array = (oasp_array){.rank = (size_t)rank, .length = (size_t)length, .bounds = bounds, .items = items};
        result->as.array = array;
        for (size_t i = 0; i < (size_t)length; i++)
            if (!wire_decode_value(reader, context, registry, &items[i], depth + 1))
                return false;
        return true;
    }
    case OASP_VALUE_OBJECT: {
        uint32_t object_kind;
        if (!read_u32(reader, &object_kind))
            return false;
        if (object_kind == WIRE_OBJECT_EXTENSION) {
            uint32_t class_id;
            uint64_t remote_id;
            if (!read_u32(reader, &class_id) || !read_u64(reader, &remote_id) || !remote_id)
                return false;
            if (registry) {
                isolated_instance *instance =
                    find_instance(registry->instances, registry->instance_count, remote_id);
                if (!instance || !instance->value_object ||
                    instance->class_definition.class_definition->class_id != class_id)
                    return false;
                result->as.object = instance->value_object;
                return true;
            }
            oasp_ext_value *handle = NULL;
            oasp_ext_error error = {.struct_size = sizeof(error)};
            if (oasp_external_object_wrap_owned(
                    context, class_id, (void *)(uintptr_t)remote_id, &handle, &error) != OASP_EXT_OK)
                return false;
            bool unwrapped = oasp_external_value_unwrap(context, handle, result);
            oasp_external_value_api()->release(context, handle);
            return unwrapped;
        }
        if (object_kind != WIRE_OBJECT_DICTIONARY)
            return false;
        uint32_t compare_mode;
        uint64_t count;
        if (!read_u32(reader, &compare_mode) || !read_u64(reader, &count) || count > SIZE_MAX)
            return false;
        oasp_value *keys = count ? oasp_runtime_alloc_values(runtime, (size_t)count) : NULL;
        oasp_value *values = count ? oasp_runtime_alloc_values(runtime, (size_t)count) : NULL;
        if (count && (!keys || !values))
            return false;
        for (size_t i = 0; i < (size_t)count; i++)
            if (!wire_decode_value(reader, context, registry, &keys[i], depth + 1) ||
                !wire_decode_value(reader, context, registry, &values[i], depth + 1))
                return false;
        result->as.object = oasp_component_dictionary_restore(runtime, (int)compare_mode, keys, values, (size_t)count);
        return result->as.object != NULL;
    }
    }
    return false;
}

static const oasp_extension_class *find_class(const oasp_extension_descriptor *descriptor, uint32_t class_id) {
    for (size_t i = 0; i < descriptor->class_count; i++)
        if (descriptor->classes[i].class_id == class_id)
            return &descriptor->classes[i];
    return NULL;
}

static isolated_instance *find_instance(isolated_instance *instances, size_t count, uint64_t id) {
    for (size_t i = 0; i < count; i++)
        if (instances[i].id == id)
            return &instances[i];
    return NULL;
}

static isolated_request *find_request(isolated_request *requests, size_t count, uint64_t id) {
    for (size_t i = 0; i < count; i++)
        if (requests[i].id == id)
            return &requests[i];
    return NULL;
}

static int child_wrap_owned(oasp_ext_call_context *context, uint32_t class_id, void *native_instance, oasp_ext_value **result, oasp_ext_error *error) {
    isolated_object_registry *registry = context ? context->wrap_owned_data : NULL;
    const oasp_extension_class *class_definition =
        context ? oasp_external_module_class_find(context->module, class_id) : NULL;
    isolated_instance *slot = NULL;
    if (registry)
        for (size_t i = 0; i < registry->instance_count; i++)
            if (!registry->instances[i].id) {
                slot = &registry->instances[i];
                break;
            }
    if (!context || !native_instance || !result || !registry || !class_definition || !slot) {
        if (error)
            *error = (oasp_ext_error){.struct_size = sizeof(*error), .code = 7,
                                      .category = OASP_EXT_ERROR_RESOURCE,
                                      .message = "unable to register isolated extension object",
                                      .message_length = 44};
        return OASP_EXT_ERROR;
    }
    slot->id = (*registry->next_id)++;
    slot->request_id = registry->request_id;
    slot->request_state = registry->request_state;
    slot->class_definition = (oasp_external_class){
        .module = context->module,
        .class_definition = class_definition
    };
    char preparation_error[128];
    if (!oasp_external_class_prepare(&slot->class_definition, preparation_error, sizeof(preparation_error)))
        goto failed;
    slot->object = (oasp_external_object){
        .class_definition = &slot->class_definition,
        .instance = native_instance,
        .request_state = registry->request_state,
        .remote_id = slot->id
    };
    slot->object.handles.runtime = context->runtime;
    slot->object.handles.owner = &slot->object;
    slot->value_object = oasp_runtime_alloc_object(context->runtime);
    if (!slot->value_object)
        goto failed;
    *slot->value_object = (oasp_object){
        .component_kind = OASP_COMPONENT_EXTERNAL,
        .component_name = class_definition->canonical_name,
        .native = &slot->object,
        .component_vtable = oasp_external_component_vtable()
    };
    *result = oasp_external_value_create(
        context, (oasp_value){.kind = OASP_VALUE_OBJECT, .as.object = slot->value_object},
        OASP_EXT_VALUE_OBJECT);
    if (*result)
        return OASP_EXT_OK;

failed:
    oasp_external_class_dispose(&slot->class_definition);
    memset(slot, 0, sizeof(*slot));
    if (error)
        *error = (oasp_ext_error){.struct_size = sizeof(*error), .code = 7,
                                  .category = OASP_EXT_ERROR_RESOURCE,
                                  .message = "unable to register isolated extension object",
                                  .message_length = 44};
    return OASP_EXT_ERROR;
}

static void child_context_objects(oasp_ext_call_context *context, isolated_object_registry *registry) {
    context->wrap_owned = child_wrap_owned;
    context->wrap_owned_data = registry;
}

static oasp_ext_value_kind isolated_public_kind(const oasp_value *value) {
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

static bool child_send(int fd, int status, uint64_t instance_id, const oasp_ext_error *error, const oasp_buffer *payload) {
    size_t message_length = error && error->message ? error->message_length : 0;
    size_t source_length = error && error->source ? error->source_length : 0;
    size_t detail_length = error && error->detail ? error->detail_length : 0;
    if (error && error->message && !message_length)
        message_length = strlen(error->message);
    if (error && error->source && !source_length)
        source_length = strlen(error->source);
    if (error && error->detail && !detail_length)
        detail_length = strlen(error->detail);
    ipc_response response = {
        .status = status,
        .error_code = error ? error->code : 0,
        .error_category = error ? error->category : 0,
        .instance_id = instance_id,
        .message_length = message_length,
        .source_length = source_length,
        .detail_length = detail_length,
        .payload_length = payload ? payload->length : 0
    };
    return write_all(fd, &response, sizeof(response)) &&
           (!message_length || write_all(fd, error->message, message_length)) &&
           (!source_length || write_all(fd, error->source, source_length)) &&
           (!detail_length || write_all(fd, error->detail, detail_length)) &&
           (!payload || !payload->length || write_all(fd, payload->data, payload->length));
}

static void child_destroy_instance(oasp_ext_call_context *call, isolated_instance *instance, const oasp_extension_descriptor *descriptor) {
    const oasp_extension_class *class_definition = instance->class_definition.class_definition;
    if (instance->object.instance && class_definition && class_definition->destroy)
        class_definition->destroy(call, instance->object.instance);
    (void)descriptor;
    instance->object.instance = NULL;
    instance->object.destroyed = true;
    oasp_external_value_table_dispose(&instance->object.handles);
    oasp_external_class_dispose(&instance->class_definition);
    memset(instance, 0, sizeof(*instance));
}

static int isolated_host_loop(int fd, oasp_external_module *module) {
    suppress_socket_sigpipe(fd);
    module->library = dlopen(module->config->library, RTLD_NOW | RTLD_LOCAL);
    oasp_extension_entry entry = NULL;
    if (module->library) {
        void *symbol = dlsym(module->library, "openasp_extension");
        _Static_assert(sizeof(entry) == sizeof(symbol), "dlsym function pointer size mismatch");
        memcpy(&entry, &symbol, sizeof(entry));
    }
    module->descriptor = entry ? entry() : NULL;
    void *module_state = NULL;
    void *worker_state = NULL;
    oasp_ext_error error = {.struct_size = sizeof(error)};
    char validation_error[512] = {0};
    if (!oasp_external_descriptor_validate(module->config, module->descriptor,
                                           validation_error, sizeof(validation_error))) {
        error = (oasp_ext_error){.struct_size = sizeof(error), .code = 5,
                                 .category = OASP_EXT_ERROR_RUNTIME,
                                 .message = validation_error,
                                 .message_length = strlen(validation_error)};
        child_send(fd, OASP_EXT_ERROR, 0, &error, NULL);
        return 70;
    }
    if (module->descriptor->module_init &&
        module->descriptor->module_init(oasp_external_host_api(), module->config->capabilities,
                                        &module_state, &error) != OASP_EXT_OK) {
        child_send(fd, OASP_EXT_ERROR, 0, &error, NULL);
        return 70;
    }
    if (module->descriptor->worker_init &&
        module->descriptor->worker_init(module_state, &worker_state, &error) != OASP_EXT_OK) {
        child_send(fd, OASP_EXT_ERROR, 0, &error, NULL);
        if (module->descriptor->module_shutdown)
            module->descriptor->module_shutdown(module_state);
        return 70;
    }
    oasp_buffer metadata = {0};
    if (!oasp_buffer_init(&metadata, 1024) || !encode_descriptor(&metadata, module->descriptor) ||
        metadata.length > module->config->max_message_bytes ||
        !child_send(fd, OASP_EXT_OK, 0, NULL, &metadata)) {
        oasp_buffer_dispose(&metadata);
        if (module->descriptor->worker_shutdown)
            module->descriptor->worker_shutdown(worker_state);
        if (module->descriptor->module_shutdown)
            module->descriptor->module_shutdown(module_state);
        return 70;
    }
    oasp_buffer_dispose(&metadata);
    isolated_instance *instances = calloc(module->config->max_instances, sizeof(*instances));
    isolated_request *requests = calloc(module->config->max_instances, sizeof(*requests));
    size_t instance_count = module->config->max_instances;
    size_t request_count = module->config->max_instances;
    uint64_t next_id = 1;
    uint64_t next_request_id = 1;
    if (!instances || !requests) {
        free(instances);
        free(requests);
        if (module->descriptor->worker_shutdown)
            module->descriptor->worker_shutdown(worker_state);
        if (module->descriptor->module_shutdown)
            module->descriptor->module_shutdown(module_state);
        return 70;
    }
    for (;;) {
        ipc_request request;
        if (!read_all(fd, &request, sizeof(request)) ||
            request.payload_length > module->config->max_message_bytes)
            break;
        unsigned char *payload = request.payload_length ? malloc((size_t)request.payload_length) : NULL;
        if (request.payload_length && (!payload || !read_all(fd, payload, (size_t)request.payload_length))) {
            free(payload);
            break;
        }
        if (request.operation == IPC_STOP) {
            free(payload);
            break;
        }
        oasp_ext_call_context call;
        oasp_ext_error operation_error = {.struct_size = sizeof(operation_error)};
        int status = OASP_EXT_ERROR;
        uint64_t response_id = 0;
        oasp_buffer output = {0};
        (void)oasp_buffer_init(&output, 256);
        if (request.operation == IPC_REQUEST_INIT) {
            isolated_request *slot = NULL;
            for (size_t i = 0; i < request_count; i++)
                if (!requests[i].id) {
                    slot = &requests[i];
                    break;
                }
            if (slot) {
                slot->id = next_request_id++;
                slot->request = oasp_request_create("/", "", "", "EXTENSION");
                slot->runtime = calloc(1, sizeof(*slot->runtime));
                oasp_request_limits limits = {.memory_limit = module->config->max_external_bytes};
                status = slot->request && slot->runtime &&
                                 !oasp_request_set_limits(slot->request, &limits)
                             ? OASP_EXT_OK : OASP_EXT_ERROR;
                if (status == OASP_EXT_OK) {
                    oasp_runtime_init(slot->runtime, slot->request);
                    slot->runtime_initialized = true;
                    oasp_external_context_init(&call, slot->runtime, module, NULL, NULL, 0);
                    call.resource_owner = slot;
                    isolated_object_registry object_registry = {
                        instances, instance_count, &next_id, slot->id, slot->state
                    };
                    child_context_objects(&call, &object_registry);
                    if (request.deadline_milliseconds != UINT64_MAX)
                        oasp_external_context_set_timeout(&call, request.deadline_milliseconds);
                    status = module->descriptor->request_init
                                 ? module->descriptor->request_init(
                                       &call, worker_state, &slot->state, &operation_error)
                                 : OASP_EXT_OK;
                }
                if (status == OASP_EXT_OK) {
                    for (size_t i = 0; i < instance_count; i++)
                        if (instances[i].id && instances[i].request_id == slot->id) {
                            instances[i].request_state = slot->state;
                            instances[i].object.request_state = slot->state;
                        }
                    response_id = slot->id;
                } else {
                    if (slot->runtime_initialized)
                        for (size_t i = 0; i < instance_count; i++)
                            if (instances[i].id && instances[i].request_id == slot->id) {
                                oasp_external_context_init(
                                    &call, slot->runtime, module, &instances[i].object,
                                    &instances[i].object.handles, 0);
                                child_destroy_instance(&call, &instances[i], module->descriptor);
                            }
                    if (slot->runtime_initialized)
                        oasp_runtime_dispose(slot->runtime);
                    free(slot->runtime);
                    oasp_request_destroy(slot->request);
                    slot->request = NULL;
                    slot->runtime = NULL;
                    slot->state = NULL;
                    slot->id = 0;
                }
            }
        } else if (request.operation == IPC_REQUEST_SHUTDOWN) {
            isolated_request *active_request =
                find_request(requests, request_count, request.request_id);
            if (active_request) {
                for (size_t i = 0; i < instance_count; i++) {
                    if (!instances[i].id || instances[i].request_id != active_request->id)
                        continue;
                    oasp_external_context_init(
                        &call, active_request->runtime, module,
                        &instances[i].object, &instances[i].object.handles, 0);
                    child_destroy_instance(&call, &instances[i], module->descriptor);
                }
                oasp_external_context_init(&call, active_request->runtime, module, NULL, NULL, 0);
                call.resource_owner = active_request;
                if (module->descriptor->request_shutdown)
                    module->descriptor->request_shutdown(&call, active_request->state);
                oasp_runtime_dispose(active_request->runtime);
                free(active_request->runtime);
                oasp_request_destroy(active_request->request);
                memset(active_request, 0, sizeof(*active_request));
                status = OASP_EXT_OK;
            }
        } else if (request.operation == IPC_CREATE) {
            const oasp_extension_class *class_definition = find_class(module->descriptor, request.class_id);
            isolated_request *active_request =
                find_request(requests, request_count, request.request_id);
            isolated_instance *slot = NULL;
            for (size_t i = 0; i < instance_count; i++)
                if (!instances[i].id) {
                    slot = &instances[i];
                    break;
                }
            if (class_definition && active_request && slot) {
                slot->id = next_id++;
                slot->request_id = active_request->id;
                slot->class_definition = (oasp_external_class){
                    .module = module,
                    .class_definition = class_definition
                };
                char preparation_error[128];
                if (!oasp_external_class_prepare(
                        &slot->class_definition, preparation_error, sizeof(preparation_error))) {
                    memset(slot, 0, sizeof(*slot));
                } else {
                    slot->object.class_definition = &slot->class_definition;
                    slot->object.request_state = active_request->state;
                    slot->object.remote_id = slot->id;
                    slot->object.handles.runtime = active_request->runtime;
                    slot->object.handles.owner = &slot->object;
                    slot->value_object = oasp_runtime_alloc_object(active_request->runtime);
                    if (slot->value_object)
                        *slot->value_object = (oasp_object){
                            .component_kind = OASP_COMPONENT_EXTERNAL,
                            .component_name = class_definition->canonical_name,
                            .native = &slot->object,
                            .component_vtable = oasp_external_component_vtable()
                        };
                    oasp_external_context_init(&call, active_request->runtime, module, &slot->object,
                                               &slot->object.handles, 0);
                    isolated_object_registry object_registry = {
                        instances, instance_count, &next_id, active_request->id, active_request->state
                    };
                    child_context_objects(&call, &object_registry);
                    if (request.deadline_milliseconds != UINT64_MAX)
                        oasp_external_context_set_timeout(&call, request.deadline_milliseconds);
                    slot->request_state = active_request->state;
                    status = slot->value_object
                                 ? class_definition->create(&call, worker_state, active_request->state,
                                                            &slot->object.instance, &operation_error)
                                 : OASP_EXT_ERROR;
                    if (status == OASP_EXT_OK && slot->object.instance)
                        response_id = slot->id;
                    else
                        child_destroy_instance(&call, slot, module->descriptor);
                }
            }
        } else {
            isolated_instance *instance = find_instance(instances, instance_count, request.instance_id);
            isolated_request *active_request = instance
                                                   ? find_request(
                                                         requests, request_count, instance->request_id)
                                                   : NULL;
            if (instance && active_request) {
                instance->object.handles.runtime = active_request->runtime;
                oasp_external_context_init(&call, active_request->runtime, module,
                                           &instance->object, &instance->object.handles, 0);
                isolated_object_registry object_registry = {
                    instances, instance_count, &next_id, active_request->id, active_request->state
                };
                child_context_objects(&call, &object_registry);
                if (request.deadline_milliseconds != UINT64_MAX)
                    oasp_external_context_set_timeout(&call, request.deadline_milliseconds);
                if (request.operation == IPC_DESTROY) {
                    child_destroy_instance(&call, instance, module->descriptor);
                    status = OASP_EXT_OK;
                } else if (request.operation == IPC_SNAPSHOT) {
                    const oasp_extension_class *class_definition =
                        instance->class_definition.class_definition;
                    oasp_ext_value *state = NULL;
                    if (class_definition->snapshot)
                        status = class_definition->snapshot(
                            &call, instance->object.instance, &state, &operation_error);
                    if (status == OASP_EXT_OK && state) {
                        oasp_value value = oasp_value_empty();
                        if (!oasp_external_value_unwrap(&call, state, &value) ||
                            !wire_encode_value(&output, &value, 0, module))
                            status = OASP_EXT_ERROR;
                        oasp_external_value_api()->release(&call, state);
                    }
                } else if (request.operation == IPC_RESTORE) {
                    wire_reader reader = {payload, (size_t)request.payload_length, 0};
                    oasp_value value = oasp_value_empty();
                    oasp_ext_value *state = NULL;
                    const oasp_extension_class *class_definition =
                        instance->class_definition.class_definition;
                    if (wire_decode_value(&reader, &call, &object_registry, &value, 0) &&
                        reader.offset == reader.length &&
                        (state = oasp_external_value_create(&call, value, isolated_public_kind(&value))) &&
                        class_definition->restore) {
                        if (instance->object.instance && class_definition->destroy)
                            class_definition->destroy(&call, instance->object.instance);
                        instance->object.instance = NULL;
                        status = class_definition->restore(
                            &call, worker_state, instance->request_state, state,
                            &instance->object.instance, &operation_error);
                        oasp_external_value_api()->release(&call, state);
                    }
                } else if (request.operation == IPC_INVOKE) {
                    wire_reader reader = {payload, (size_t)request.payload_length, 0};
                    oasp_value assigned = oasp_value_empty();
                    oasp_value *arguments = request.argument_count
                                                ? oasp_runtime_alloc_values(
                                                      active_request->runtime,
                                                      request.argument_count) : NULL;
                    oasp_ext_value *assigned_handle = NULL;
                    const oasp_ext_value **views = request.argument_count
                                                      ? calloc(request.argument_count, sizeof(*views)) : NULL;
                    bool decoded = (!request.has_value ||
                                    (wire_decode_value(&reader, &call, &object_registry, &assigned, 0) &&
                                     (assigned_handle = oasp_external_value_create(
                                          &call, assigned, isolated_public_kind(&assigned))))) &&
                                   (!request.argument_count || (arguments && views));
                    for (size_t i = 0; decoded && i < request.argument_count; i++) {
                        decoded = wire_decode_value(
                            &reader, &call, &object_registry, &arguments[i], 0);
                        if (decoded)
                            views[i] = oasp_external_value_create(
                                &call, arguments[i], isolated_public_kind(&arguments[i]));
                        decoded = decoded && views[i];
                    }
                    if (decoded && reader.offset == reader.length) {
                        oasp_ext_invocation invocation = {
                            .struct_size = sizeof(invocation),
                            .operation = (oasp_ext_operation)request.invoke_operation,
                            .member_id = request.member_id,
                            .value = assigned_handle,
                            .arguments = views,
                            .argument_count = request.argument_count
                        };
                        oasp_ext_result result = {.struct_size = sizeof(result)};
                        status = instance->class_definition.class_definition->invoke(
                            &call, instance->object.instance, &invocation, &result, &operation_error);
                        oasp_value value = oasp_value_empty();
                        if (status == OASP_EXT_OK)
                            status = oasp_external_await_result(&call, &result, &value, &operation_error);
                        if (status == OASP_EXT_OK && !wire_encode_value(&output, &value, 0, module))
                            status = OASP_EXT_ERROR;
                    }
                    if (assigned_handle)
                        oasp_external_value_api()->release(&call, assigned_handle);
                    for (size_t i = 0; i < request.argument_count; i++)
                        if (views && views[i])
                            oasp_external_value_api()->release(&call, views[i]);
                    free(views);
                }
            }
        }
        free(payload);
        bool sent = child_send(fd, status, response_id, &operation_error, &output);
        oasp_buffer_dispose(&output);
        if (!sent)
            break;
    }
    for (size_t i = 0; i < instance_count; i++) {
        if (!instances[i].id)
            continue;
        isolated_request *active_request =
            find_request(requests, request_count, instances[i].request_id);
        if (!active_request)
            continue;
        oasp_ext_call_context call;
        oasp_external_context_init(
            &call, active_request->runtime, module,
            &instances[i].object, &instances[i].object.handles, 0);
        child_destroy_instance(&call, &instances[i], module->descriptor);
    }
    for (size_t i = 0; i < request_count; i++) {
        if (!requests[i].id)
            continue;
        oasp_ext_call_context call;
        oasp_external_context_init(&call, requests[i].runtime, module, NULL, NULL, 0);
        call.resource_owner = &requests[i];
        if (module->descriptor->request_shutdown)
            module->descriptor->request_shutdown(&call, requests[i].state);
        oasp_runtime_dispose(requests[i].runtime);
        free(requests[i].runtime);
        oasp_request_destroy(requests[i].request);
    }
    free(instances);
    free(requests);
    if (module->descriptor->worker_shutdown)
        module->descriptor->worker_shutdown(worker_state);
    if (module->descriptor->module_shutdown)
        module->descriptor->module_shutdown(module_state);
    return 0;
}

static bool parse_u64_argument(const char *text, uint64_t maximum, uint64_t *result) {
    if (!text || !*text || !result || *text == '-')
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || !end || *end || value > maximum)
        return false;
    *result = (uint64_t)value;
    return true;
}

int oasp_external_isolated_host_main(int argc, char **argv) {
    uint64_t fd_value;
    uint64_t capabilities;
    uint64_t max_instances;
    uint64_t max_message_bytes;
    uint64_t max_external_bytes;
    uint64_t max_worker_external_bytes;
    uint64_t max_model_external_bytes;
    uint64_t max_device_external_bytes;
    uint64_t call_timeout_ms;
    if (argc != 12 ||
        !parse_u64_argument(argv[1], INT_MAX, &fd_value) ||
        !parse_u64_argument(argv[4], UINT64_MAX, &capabilities) ||
        !parse_u64_argument(argv[5], SIZE_MAX, &max_instances) || !max_instances ||
        !parse_u64_argument(argv[6], SIZE_MAX, &max_message_bytes) || !max_message_bytes ||
        !parse_u64_argument(argv[7], SIZE_MAX, &max_external_bytes) || !max_external_bytes ||
        !parse_u64_argument(argv[8], SIZE_MAX, &max_worker_external_bytes) || !max_worker_external_bytes ||
        !parse_u64_argument(argv[9], SIZE_MAX, &max_model_external_bytes) || !max_model_external_bytes ||
        !parse_u64_argument(argv[10], SIZE_MAX, &max_device_external_bytes) || !max_device_external_bytes ||
        !parse_u64_argument(argv[11], ULONG_MAX, &call_timeout_ms) || !call_timeout_ms)
        return 64;
    oasp_external_module_config config = {
        .id = argv[3],
        .library = argv[2],
        .capabilities = capabilities,
        .mode = OASP_EXTERNAL_MODE_ISOLATED,
        .max_instances = (size_t)max_instances,
        .max_message_bytes = (size_t)max_message_bytes,
        .max_external_bytes = (size_t)max_external_bytes,
        .max_worker_external_bytes = (size_t)max_worker_external_bytes,
        .max_model_external_bytes = (size_t)max_model_external_bytes,
        .max_device_external_bytes = (size_t)max_device_external_bytes,
        .call_timeout_ms = (unsigned long)call_timeout_ms
    };
    oasp_external_module module = {.config = &config, .isolated = true};
    int status = isolated_host_loop((int)fd_value, &module);
    if (module.library)
        dlclose(module.library);
    return status;
}

static void proxy_failed(isolated_proxy *proxy) {
    if (!proxy || proxy->failed)
        return;
    proxy->failed = true;
    close(proxy->fd);
    proxy->fd = -1;
    if (proxy->pid > 0) {
        (void)kill(proxy->pid, SIGKILL);
        while (waitpid(proxy->pid, NULL, 0) < 0 && errno == EINTR)
            ;
        proxy->pid = -1;
    }
}

static bool descriptor_equal(const oasp_extension_descriptor *left, const oasp_extension_descriptor *right) {
    if (!left || !right ||
        left->abi_major_required != right->abi_major_required ||
        left->abi_minor_required != right->abi_minor_required ||
        left->required_capabilities != right->required_capabilities ||
        left->class_count != right->class_count ||
        strcmp(left->module_name, right->module_name) ||
        strcmp(left->module_version, right->module_version))
        return false;
    for (size_t i = 0; i < left->class_count; i++) {
        const oasp_extension_class *left_class = &left->classes[i];
        const oasp_extension_class *right_class = &right->classes[i];
        if (left_class->class_id != right_class->class_id ||
            left_class->flags != right_class->flags ||
            left_class->member_count != right_class->member_count ||
            strcmp(left_class->prog_id, right_class->prog_id) ||
            strcmp(left_class->canonical_name, right_class->canonical_name))
            return false;
        for (size_t j = 0; j < left_class->member_count; j++) {
            const oasp_extension_member *left_member = &left_class->members[j];
            const oasp_extension_member *right_member = &right_class->members[j];
            if (left_member->member_id != right_member->member_id ||
                left_member->flags != right_member->flags ||
                strcmp(left_member->name, right_member->name))
                return false;
        }
    }
    return true;
}

static bool proxy_restart(oasp_external_module *module, char *error, size_t error_size) {
    isolated_proxy *failed_proxy = module->isolated_state;
    const oasp_extension_descriptor *registered_descriptor = module->descriptor;
    if (!failed_proxy || !failed_proxy->failed || !registered_descriptor)
        return false;
    free(failed_proxy);
    module->isolated_state = NULL;
    module->descriptor = NULL;
    if (!oasp_external_isolated_proxy_load(module, error, error_size)) {
        isolated_proxy *replacement = module->isolated_state;
        if (replacement) {
            proxy_failed(replacement);
            free(replacement);
            module->isolated_state = NULL;
        }
        free_proxy_descriptor(module->descriptor);
        module->descriptor = registered_descriptor;
        return false;
    }
    const oasp_extension_descriptor *replacement_descriptor = module->descriptor;
    if (!descriptor_equal(registered_descriptor, replacement_descriptor)) {
        snprintf(error, error_size,
                 "isolated extension '%s' changed its descriptor while restarting",
                 module->config->id);
        isolated_proxy *replacement = module->isolated_state;
        proxy_failed(replacement);
        free(replacement);
        module->isolated_state = NULL;
        free_proxy_descriptor(replacement_descriptor);
        module->descriptor = registered_descriptor;
        return false;
    }
    free_proxy_descriptor(replacement_descriptor);
    module->descriptor = registered_descriptor;
    return true;
}

static void proxy_transport_error(oasp_ext_error *error, bool timed_out, const char *fallback) {
    if (!error)
        return;
    if (timed_out) {
        *error = (oasp_ext_error){
            .struct_size = sizeof(*error),
            .code = 258,
            .category = OASP_EXT_ERROR_TIMEOUT,
            .message = "isolated extension call timed out",
            .message_length = 33
        };
    } else {
        *error = (oasp_ext_error){
            .struct_size = sizeof(*error),
            .code = 1722,
            .category = OASP_EXT_ERROR_TRANSPORT,
            .message = fallback,
            .message_length = strlen(fallback)
        };
    }
}

static bool proxy_exchange(oasp_ext_call_context *context, const ipc_request *request, const oasp_buffer *payload, ipc_response *response, oasp_buffer *response_payload,
                           oasp_ext_error *error) {
    oasp_external_module *module = context->module;
    isolated_proxy *proxy = module->isolated_state;
    if (!proxy || proxy->failed) {
        if (request->operation != IPC_REQUEST_INIT) {
            proxy_transport_error(error, false, "isolated extension host terminated");
            return false;
        }
        char restart_error[512];
        if (!proxy_restart(module, restart_error, sizeof(restart_error))) {
            proxy_transport_error(error, false, "isolated extension host restart failed");
            if (error && context->runtime) {
                error->detail = oasp_runtime_copy(
                    context->runtime, restart_error, strlen(restart_error));
                error->detail_length = error->detail ? strlen(restart_error) : 0;
            }
            return false;
        }
        proxy = module->isolated_state;
    }
    int64_t timeout = module->config->call_timeout_ms > INT_MAX
                          ? INT_MAX : (int64_t)module->config->call_timeout_ms;
    int64_t request_remaining = oasp_external_context_deadline(context);
    if (request_remaining >= 0 && request_remaining < timeout)
        timeout = request_remaining;
    int64_t now = monotonic_milliseconds();
    int64_t deadline = timeout > INT64_MAX - now ? INT64_MAX : now + timeout;
    ipc_request transmitted = *request;
    transmitted.deadline_milliseconds = timeout < 0 ? UINT64_MAX : (uint64_t)timeout;
    if (!timed_write_all(proxy->fd, &transmitted, sizeof(transmitted), deadline) ||
        (payload && payload->length &&
         !timed_write_all(proxy->fd, payload->data, payload->length, deadline))) {
        bool timed_out = monotonic_milliseconds() >= deadline;
        proxy_failed(proxy);
        proxy_transport_error(error, timed_out, "isolated extension host terminated");
        return false;
    }
    if (!timed_read_all(proxy->fd, response, sizeof(*response), deadline)) {
        bool timed_out = monotonic_milliseconds() >= deadline;
        proxy_failed(proxy);
        proxy_transport_error(error, timed_out, "isolated extension host terminated");
        return false;
    }
    if (response->reserved ||
        (response->status != OASP_EXT_OK && response->status != OASP_EXT_ERROR &&
         response->status != OASP_EXT_PENDING && response->status != OASP_EXT_END &&
         response->status != OASP_EXT_CANCELLED) ||
        response->message_length > SIZE_MAX - response->source_length ||
        response->message_length + response->source_length > SIZE_MAX - response->detail_length ||
        response->message_length + response->source_length + response->detail_length >
            SIZE_MAX - response->payload_length) {
        proxy_failed(proxy);
        proxy_transport_error(error, false, "isolated extension host returned an invalid response");
        return false;
    }
    size_t total = (size_t)(response->message_length + response->source_length +
                            response->detail_length + response->payload_length);
    if (total > module->config->max_message_bytes || total == SIZE_MAX) {
        proxy_failed(proxy);
        proxy_transport_error(error, false, "isolated extension response exceeds configured limit");
        return false;
    }
    if (error) {
        error->code = response->error_code;
        error->category = response->error_category;
    }
    if (!oasp_buffer_init(response_payload, total + 1) ||
        !oasp_buffer_append(response_payload, "", 0)) {
        proxy_failed(proxy);
        proxy_transport_error(error, false, "out of memory receiving isolated extension response");
        return false;
    }
    if (total) {
        char *storage = malloc(total + 1);
        if (!storage || !timed_read_all(proxy->fd, storage, total, deadline)) {
            free(storage);
            bool timed_out = monotonic_milliseconds() >= deadline;
            proxy_failed(proxy);
            proxy_transport_error(error, timed_out, "isolated extension host terminated");
            return false;
        }
        storage[total] = '\0';
        bool appended = oasp_buffer_append(response_payload, storage, total);
        free(storage);
        if (!appended)
            return false;
        if (error) {
            *error = (oasp_ext_error){
                .struct_size = sizeof(*error),
                .code = response->error_code,
                .category = response->error_category,
                .message = response_payload->data,
                .message_length = (size_t)response->message_length,
                .source = response_payload->data + response->message_length,
                .source_length = (size_t)response->source_length,
                .detail = response_payload->data + response->message_length + response->source_length,
                .detail_length = (size_t)response->detail_length
            };
        }
    }
    return true;
}

static char *isolated_host_path(void) {
    const char *override = getenv("OPENASP_COMPONENT_HOST");
    if (override && *override)
        return strdup(override);
    char *executable = NULL;
#if defined(__APPLE__)
    uint32_t size = 0;
    (void)_NSGetExecutablePath(NULL, &size);
    executable = size ? malloc(size) : NULL;
    if (!executable || _NSGetExecutablePath(executable, &size)) {
        free(executable);
        return NULL;
    }
#elif defined(__linux__)
    executable = malloc(PATH_MAX + 1);
    if (!executable)
        return NULL;
    ssize_t length = readlink("/proc/self/exe", executable, PATH_MAX);
    if (length <= 0 || length > PATH_MAX) {
        free(executable);
        return NULL;
    }
    executable[length] = '\0';
#else
    return NULL;
#endif
    char *separator = strrchr(executable, '/');
    const char name[] = "openasp-component-host";
    size_t prefix = separator ? (size_t)(separator + 1 - executable) : 0;
    char *result = malloc(prefix + sizeof(name));
    if (result) {
        if (prefix)
            memcpy(result, executable, prefix);
        memcpy(result + prefix, name, sizeof(name));
    }
    free(executable);
    return result;
}

static void copy_proxy_error(oasp_ext_call_context *context, oasp_ext_error *error) {
    if (!context || !context->runtime || !error)
        return;
    if (error->message && error->message_length)
        error->message = oasp_runtime_copy(context->runtime, error->message, error->message_length);
    if (error->source && error->source_length)
        error->source = oasp_runtime_copy(context->runtime, error->source, error->source_length);
    if (error->detail && error->detail_length)
        error->detail = oasp_runtime_copy(context->runtime, error->detail, error->detail_length);
}

bool oasp_external_isolated_proxy_load(oasp_external_module *module, char *error, size_t error_size) {
    char *host_path = isolated_host_path();
    if (!host_path || access(host_path, X_OK)) {
        snprintf(error, error_size, "unable to execute isolated component host '%s': %s",
                 host_path ? host_path : "openasp-component-host", strerror(host_path ? errno : ENOENT));
        free(host_path);
        return false;
    }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) {
        snprintf(error, error_size, "unable to create isolated extension socket: %s", strerror(errno));
        free(host_path);
        return false;
    }
    suppress_socket_sigpipe(sockets[0]);
    suppress_socket_sigpipe(sockets[1]);
    isolated_proxy *proxy = calloc(1, sizeof(*proxy));
    if (!proxy) {
        close(sockets[0]);
        close(sockets[1]);
        free(host_path);
        return false;
    }
    char fd_text[32];
    char capabilities_text[32];
    char max_instances_text[32];
    char max_message_bytes_text[32];
    char max_external_bytes_text[32];
    char max_worker_external_bytes_text[32];
    char max_model_external_bytes_text[32];
    char max_device_external_bytes_text[32];
    char call_timeout_text[32];
    snprintf(fd_text, sizeof(fd_text), "%d", sockets[1]);
    snprintf(capabilities_text, sizeof(capabilities_text), "%llu",
             (unsigned long long)module->config->capabilities);
    snprintf(max_instances_text, sizeof(max_instances_text), "%zu", module->config->max_instances);
    snprintf(max_message_bytes_text, sizeof(max_message_bytes_text), "%zu", module->config->max_message_bytes);
    snprintf(max_external_bytes_text, sizeof(max_external_bytes_text), "%zu", module->config->max_external_bytes);
    snprintf(max_worker_external_bytes_text, sizeof(max_worker_external_bytes_text), "%zu",
             module->config->max_worker_external_bytes);
    snprintf(max_model_external_bytes_text, sizeof(max_model_external_bytes_text), "%zu",
             module->config->max_model_external_bytes);
    snprintf(max_device_external_bytes_text, sizeof(max_device_external_bytes_text), "%zu",
             module->config->max_device_external_bytes);
    snprintf(call_timeout_text, sizeof(call_timeout_text), "%lu", module->config->call_timeout_ms);
    char *const host_arguments[] = {
        host_path, fd_text, module->config->library, module->config->id, capabilities_text,
        max_instances_text, max_message_bytes_text, max_external_bytes_text,
        max_worker_external_bytes_text, max_model_external_bytes_text,
        max_device_external_bytes_text, call_timeout_text, NULL
    };
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(error, error_size, "unable to start isolated extension host: %s", strerror(errno));
        close(sockets[0]);
        close(sockets[1]);
        free(host_path);
        free(proxy);
        return false;
    }
    if (!pid) {
        close(sockets[0]);
        int descriptor_flags = fcntl(sockets[1], F_GETFD);
        if (descriptor_flags >= 0)
            (void)fcntl(sockets[1], F_SETFD, descriptor_flags & ~FD_CLOEXEC);
        execv(host_path, host_arguments);
        _exit(127);
    }
    free(host_path);
    close(sockets[1]);
    proxy->fd = sockets[0];
    proxy->pid = pid;
    module->isolated_state = proxy;
    module->isolated = true;
    int descriptor_flags = fcntl(proxy->fd, F_GETFD);
    if (descriptor_flags >= 0)
        (void)fcntl(proxy->fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
    int status_flags = fcntl(proxy->fd, F_GETFL);
    if (status_flags >= 0)
        (void)fcntl(proxy->fd, F_SETFL, status_flags | O_NONBLOCK);
    ipc_response response;
    int64_t started = monotonic_milliseconds();
    int64_t timeout = module->config->call_timeout_ms > INT_MAX
                          ? INT_MAX : (int64_t)module->config->call_timeout_ms;
    int64_t deadline = timeout > INT64_MAX - started ? INT64_MAX : started + timeout;
    if (!timed_read_all(proxy->fd, &response, sizeof(response), deadline)) {
        snprintf(error, error_size, "isolated extension '%s' failed to initialize", module->config->id);
        proxy_failed(proxy);
        return false;
    }
    if (response.reserved ||
        (response.status != OASP_EXT_OK && response.status != OASP_EXT_ERROR) ||
        response.message_length > SIZE_MAX - response.source_length ||
        response.message_length + response.source_length > SIZE_MAX - response.detail_length ||
        response.message_length + response.source_length + response.detail_length >
            SIZE_MAX - response.payload_length ||
        response.message_length + response.source_length + response.detail_length +
                response.payload_length >
            module->config->max_message_bytes) {
        snprintf(error, error_size,
                 "isolated extension '%s' returned an invalid initialization response",
                 module->config->id);
        proxy_failed(proxy);
        return false;
    }
    if (response.status != OASP_EXT_OK) {
        size_t diagnostic_length =
            (size_t)(response.message_length + response.source_length + response.detail_length);
        char *diagnostic = diagnostic_length ? malloc(diagnostic_length + 1) : NULL;
        if (diagnostic &&
            timed_read_all(proxy->fd, diagnostic, diagnostic_length, deadline)) {
            diagnostic[diagnostic_length] = '\0';
            snprintf(error, error_size, "isolated extension '%s' failed to initialize: %.*s",
                     module->config->id, (int)(response.message_length > INT_MAX
                                                   ? INT_MAX : response.message_length),
                     diagnostic);
        } else {
            snprintf(error, error_size, "isolated extension '%s' failed to initialize",
                     module->config->id);
        }
        free(diagnostic);
        proxy_failed(proxy);
        return false;
    }
    if (response.message_length || response.source_length || response.detail_length ||
        !response.payload_length) {
        snprintf(error, error_size, "isolated extension '%s' returned invalid initialization metadata",
                 module->config->id);
        proxy_failed(proxy);
        return false;
    }
    unsigned char *metadata = response.payload_length ? malloc((size_t)response.payload_length) : NULL;
    if (!metadata ||
        !timed_read_all(proxy->fd, metadata, (size_t)response.payload_length, deadline)) {
        free(metadata);
        snprintf(error, error_size, "isolated extension '%s' returned invalid descriptor metadata",
                 module->config->id);
        proxy_failed(proxy);
        return false;
    }
    module->descriptor = decode_descriptor(metadata, (size_t)response.payload_length);
    free(metadata);
    if (!module->descriptor) {
        snprintf(error, error_size, "isolated extension '%s' returned invalid descriptor metadata",
                 module->config->id);
        proxy_failed(proxy);
        return false;
    }
    return true;
}

void oasp_external_isolated_proxy_shutdown(oasp_external_module *module) {
    isolated_proxy *proxy = module ? module->isolated_state : NULL;
    if (!proxy)
        return;
    if (!proxy->failed) {
        ipc_request request = {.operation = IPC_STOP};
        (void)write_all(proxy->fd, &request, sizeof(request));
        close(proxy->fd);
        (void)waitpid(proxy->pid, NULL, 0);
    }
    free_proxy_descriptor(module->descriptor);
    module->descriptor = NULL;
    free(proxy);
    module->isolated_state = NULL;
}

int oasp_external_isolated_request_init(oasp_ext_call_context *context, void **request_state, oasp_ext_error *error) {
    ipc_request request = {.operation = IPC_REQUEST_INIT};
    ipc_response response;
    oasp_buffer payload = {0};
    bool ok = proxy_exchange(context, &request, NULL, &response, &payload, error);
    copy_proxy_error(context, error);
    oasp_buffer_dispose(&payload);
    if (!ok) {
        if (error && !error->message)
            *error = (oasp_ext_error){.struct_size = sizeof(*error), .code = 1722,
                                      .category = OASP_EXT_ERROR_TRANSPORT,
                                      .message = "isolated extension host terminated", .message_length = 34};
        return OASP_EXT_ERROR;
    }
    if (response.status == OASP_EXT_OK && request_state)
        *request_state = (void *)(uintptr_t)response.instance_id;
    return response.status;
}

void oasp_external_isolated_request_shutdown(oasp_ext_call_context *context, void *request_state) {
    ipc_request request = {
        .operation = IPC_REQUEST_SHUTDOWN,
        .request_id = (uint64_t)(uintptr_t)request_state
    };
    ipc_response response;
    oasp_buffer payload = {0};
    oasp_ext_error error = {.struct_size = sizeof(error)};
    (void)proxy_exchange(context, &request, NULL, &response, &payload, &error);
    oasp_buffer_dispose(&payload);
}

int oasp_external_isolated_create(oasp_ext_call_context *context, const oasp_extension_class *class_definition, void *request_state, void **instance, oasp_ext_error *error) {
    ipc_request request = {
        .operation = IPC_CREATE,
        .class_id = class_definition->class_id,
        .request_id = (uint64_t)(uintptr_t)request_state
    };
    ipc_response response;
    oasp_buffer payload = {0};
    bool ok = proxy_exchange(context, &request, NULL, &response, &payload, error);
    copy_proxy_error(context, error);
    oasp_buffer_dispose(&payload);
    if (!ok) {
        if (error && !error->message)
            *error = (oasp_ext_error){.struct_size = sizeof(*error), .code = 1722,
                                      .category = OASP_EXT_ERROR_TRANSPORT,
                                      .message = "isolated extension host terminated", .message_length = 34};
        return OASP_EXT_ERROR;
    }
    if (response.status == OASP_EXT_OK)
        *instance = (void *)(uintptr_t)response.instance_id;
    return response.status;
}

void oasp_external_isolated_destroy(oasp_ext_call_context *context, void *instance) {
    ipc_request request = {.operation = IPC_DESTROY, .instance_id = (uint64_t)(uintptr_t)instance};
    ipc_response response;
    oasp_buffer payload = {0};
    oasp_ext_error error = {.struct_size = sizeof(error)};
    (void)proxy_exchange(context, &request, NULL, &response, &payload, &error);
    oasp_buffer_dispose(&payload);
}

int oasp_external_isolated_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_value *result, oasp_ext_error *error) {
    oasp_buffer request_payload = {0};
    if (invocation->argument_count > UINT32_MAX ||
        !oasp_buffer_init(&request_payload, 256))
        return OASP_EXT_ERROR;
    bool encoded = !invocation->value ||
                   wire_encode_invocation_value(&request_payload, &invocation->value->value, context);
    for (size_t i = 0; encoded && i < invocation->argument_count; i++)
        encoded = wire_encode_invocation_value(
            &request_payload, &invocation->arguments[i]->value, context);
    if (!encoded || request_payload.length > context->module->config->max_message_bytes) {
        oasp_buffer_dispose(&request_payload);
        if (error && !error->message)
            *error = (oasp_ext_error){.struct_size = sizeof(*error), .code = 13,
                                      .category = OASP_EXT_ERROR_ARGUMENT,
                                      .message = "isolated extension argument cannot be serialized",
                                      .message_length = 48};
        return OASP_EXT_ERROR;
    }
    ipc_request request = {
        .operation = IPC_INVOKE,
        .instance_id = (uint64_t)(uintptr_t)instance,
        .invoke_operation = invocation->operation,
        .member_id = invocation->member_id,
        .argument_count = (uint32_t)invocation->argument_count,
        .has_value = invocation->value != NULL,
        .payload_length = request_payload.length
    };
    ipc_response response;
    oasp_buffer response_payload = {0};
    bool ok = proxy_exchange(context, &request, &request_payload, &response, &response_payload, error);
    oasp_buffer_dispose(&request_payload);
    if (!ok) {
        copy_proxy_error(context, error);
        oasp_buffer_dispose(&response_payload);
        if (error && !error->message)
            *error = (oasp_ext_error){.struct_size = sizeof(*error), .code = 1722,
                                      .category = OASP_EXT_ERROR_TRANSPORT,
                                      .message = "isolated extension host terminated", .message_length = 34};
        return OASP_EXT_ERROR;
    }
    int status = response.status;
    if (status == OASP_EXT_OK) {
        size_t prefix = (size_t)(response.message_length + response.source_length + response.detail_length);
        wire_reader reader = {(const unsigned char *)response_payload.data + prefix,
                              response_payload.length - prefix, 0};
        if (!wire_decode_value(&reader, context, NULL, result, 0) || reader.offset != reader.length)
            status = OASP_EXT_ERROR;
    }
    copy_proxy_error(context, error);
    oasp_buffer_dispose(&response_payload);
    return status;
}

int oasp_external_isolated_snapshot(oasp_ext_call_context *context, void *instance, oasp_value *state, oasp_ext_error *error) {
    ipc_request request = {.operation = IPC_SNAPSHOT, .instance_id = (uint64_t)(uintptr_t)instance};
    ipc_response response;
    oasp_buffer response_payload = {0};
    if (!proxy_exchange(context, &request, NULL, &response, &response_payload, error)) {
        copy_proxy_error(context, error);
        oasp_buffer_dispose(&response_payload);
        return OASP_EXT_ERROR;
    }
    int status = response.status;
    if (status == OASP_EXT_OK) {
        size_t prefix = (size_t)(response.message_length + response.source_length + response.detail_length);
        wire_reader reader = {(const unsigned char *)response_payload.data + prefix,
                              response_payload.length - prefix, 0};
        if (!wire_decode_value(&reader, context, NULL, state, 0) || reader.offset != reader.length)
            status = OASP_EXT_ERROR;
    }
    copy_proxy_error(context, error);
    oasp_buffer_dispose(&response_payload);
    return status;
}

int oasp_external_isolated_restore(oasp_ext_call_context *context, void *instance, const oasp_value *state, oasp_ext_error *error) {
    oasp_buffer request_payload = {0};
    if (!oasp_buffer_init(&request_payload, 128) ||
        !wire_encode_value(&request_payload, state, 0, context->module) ||
        request_payload.length > context->module->config->max_message_bytes) {
        oasp_buffer_dispose(&request_payload);
        return OASP_EXT_ERROR;
    }
    ipc_request request = {
        .operation = IPC_RESTORE,
        .instance_id = (uint64_t)(uintptr_t)instance,
        .payload_length = request_payload.length
    };
    ipc_response response;
    oasp_buffer response_payload = {0};
    bool ok = proxy_exchange(context, &request, &request_payload, &response, &response_payload, error);
    oasp_buffer_dispose(&request_payload);
    copy_proxy_error(context, error);
    oasp_buffer_dispose(&response_payload);
    return ok ? response.status : OASP_EXT_ERROR;
}
