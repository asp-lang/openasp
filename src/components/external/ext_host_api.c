// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ext_internal.h"

#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t host_granted_capabilities(oasp_ext_call_context *context) {
    return context && context->module && context->module->config ? context->module->config->capabilities : 0;
}

static int host_require_capability(oasp_ext_call_context *context, uint64_t capability, oasp_ext_error *error) {
    if (capability && (host_granted_capabilities(context) & capability) == capability)
        return OASP_EXT_OK;
    if (error) {
        *error = (oasp_ext_error){
            .struct_size = sizeof(*error),
            .code = 70,
            .category = OASP_EXT_ERROR_PERMISSION,
            .message = "extension capability is not granted",
            .message_length = 35,
            .source = "OpenASP.Extension",
            .source_length = 17
        };
    }
    return OASP_EXT_ERROR;
}

static int host_account_external(oasp_ext_call_context *context, ptrdiff_t delta) {
    return context && context->runtime && context->resource_owner &&
           oasp_runtime_account_external(context->runtime, context->resource_owner, delta) ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int account_counter(atomic_size_t *counter, size_t limit, ptrdiff_t delta) {
    size_t current = atomic_load_explicit(counter, memory_order_relaxed);
    for (;;) {
        size_t next;
        if (delta < 0) {
            size_t amount = (size_t)(-(delta + 1)) + 1;
            if (amount > current)
                return OASP_EXT_ERROR;
            next = current - amount;
        } else {
            size_t amount = (size_t)delta;
            if (amount > limit || current > limit - amount)
                return OASP_EXT_ERROR;
            next = current + amount;
        }
        if (atomic_compare_exchange_weak_explicit(
                counter, &current, next, memory_order_relaxed, memory_order_relaxed))
            return OASP_EXT_OK;
    }
}

static int host_account_resource(oasp_ext_call_context *context, oasp_ext_resource_scope scope, const char *resource_id, size_t resource_id_length, ptrdiff_t delta,
                                 oasp_ext_error *error) {
    if (!context || !context->module || !context->module->config ||
        ((scope == OASP_EXT_RESOURCE_MODEL || scope == OASP_EXT_RESOURCE_DEVICE) &&
         (!resource_id || !resource_id_length))) {
        if (error)
            *error = (oasp_ext_error){.struct_size = sizeof(*error), .code = 5,
                                      .category = OASP_EXT_ERROR_ARGUMENT,
                                      .message = "invalid extension resource accounting request",
                                      .message_length = 45};
        return OASP_EXT_ERROR;
    }
    if (scope == OASP_EXT_RESOURCE_REQUEST)
        return host_account_external(context, delta);
    atomic_size_t *counter = scope == OASP_EXT_RESOURCE_WORKER
                                 ? &context->module->worker_external_bytes
                                 : scope == OASP_EXT_RESOURCE_MODEL
                                       ? &context->module->model_external_bytes
                                       : scope == OASP_EXT_RESOURCE_DEVICE
                                             ? &context->module->device_external_bytes : NULL;
    size_t limit = scope == OASP_EXT_RESOURCE_WORKER
                       ? context->module->config->max_worker_external_bytes
                       : scope == OASP_EXT_RESOURCE_MODEL
                             ? context->module->config->max_model_external_bytes
                             : scope == OASP_EXT_RESOURCE_DEVICE
                                   ? context->module->config->max_device_external_bytes : 0;
    if (counter && limit && account_counter(counter, limit, delta) == OASP_EXT_OK) {
        if (delta > 0)
            oasp_runtime_note_external_pressure(context->runtime, (size_t)delta);
        return OASP_EXT_OK;
    }
    if (error)
        *error = (oasp_ext_error){.struct_size = sizeof(*error), .code = 7,
                                  .category = OASP_EXT_ERROR_RESOURCE,
                                  .message = "extension resource budget exceeded",
                                  .message_length = 34};
    return OASP_EXT_ERROR;
}

static void host_log(oasp_ext_call_context *context, int level, const char *message, size_t message_length) {
    (void)context;
    oasp_log_write(level >= 0 && level <= 4 ? (oasp_log_level)level : OASP_LOG_ERROR,
                   "%.*s", (int)(message_length > INT32_MAX ? INT32_MAX : message_length), message ? message : "");
}

static int host_cancelled(oasp_ext_call_context *context) {
    return oasp_external_context_cancelled(context);
}

static int64_t host_deadline_milliseconds(oasp_ext_call_context *context) {
    return oasp_external_context_deadline(context);
}

static void host_yield(oasp_ext_call_context *context, uint32_t maximum_wait_milliseconds) {
    if (host_cancelled(context) || !maximum_wait_milliseconds)
        return;
    struct timespec delay = {
        .tv_sec = maximum_wait_milliseconds / 1000,
        .tv_nsec = (long)(maximum_wait_milliseconds % 1000) * 1000000L
    };
    (void)nanosleep(&delay, NULL);
}

static oasp_ext_future *host_future_create(oasp_ext_call_context *context, const oasp_ext_future_vtable *vtable, void *state) {
    (void)context;
    if (!vtable || vtable->struct_size < sizeof(*vtable) || !vtable->poll || !vtable->destroy)
        return NULL;
    oasp_ext_future *future = calloc(1, sizeof(*future));
    if (future) {
        future->vtable = vtable;
        future->state = state;
    }
    return future;
}

static oasp_ext_stream *host_stream_create(oasp_ext_call_context *context, const oasp_ext_stream_vtable *vtable, void *state) {
    (void)context;
    if (!vtable || vtable->struct_size < sizeof(*vtable) || !vtable->next || !vtable->destroy)
        return NULL;
    oasp_ext_stream *stream = calloc(1, sizeof(*stream));
    if (stream) {
        stream->vtable = vtable;
        stream->state = state;
    }
    return stream;
}

static const oasp_ext_async_api async_api = {
    .struct_size = sizeof(async_api),
    .version = OASP_EXT_API_ASYNC_VERSION,
    .future_create = host_future_create,
    .stream_create = host_stream_create,
    .cancelled = host_cancelled,
    .deadline_milliseconds = host_deadline_milliseconds,
    .yield = host_yield
};

static const oasp_ext_runtime_api runtime_api = {
    .struct_size = sizeof(runtime_api),
    .version = OASP_EXT_API_RUNTIME_VERSION,
    .granted_capabilities = host_granted_capabilities,
    .require_capability = host_require_capability,
    .account_external = host_account_external,
    .log = host_log,
    .account_resource = host_account_resource
};

static const void *host_query_api(const char *name, uint32_t minimum_version, uint32_t *actual_version) {
    const void *result = NULL;
    uint32_t version = 0;
    if (name && !strcmp(name, OASP_EXT_API_VALUE)) {
        result = oasp_external_value_api();
        version = OASP_EXT_API_VALUE_VERSION;
    } else if (name && !strcmp(name, OASP_EXT_API_OBJECT)) {
        result = oasp_external_object_api();
        version = OASP_EXT_API_OBJECT_VERSION;
    } else if (name && !strcmp(name, OASP_EXT_API_ASYNC)) {
        result = &async_api;
        version = OASP_EXT_API_ASYNC_VERSION;
    } else if (name && !strcmp(name, OASP_EXT_API_RUNTIME)) {
        result = &runtime_api;
        version = OASP_EXT_API_RUNTIME_VERSION;
    }
    if (actual_version)
        *actual_version = result ? version : 0;
    return result && minimum_version <= version ? result : NULL;
}

const oasp_ext_host *oasp_external_host_api(void) {
    static const oasp_ext_host api = {
        .struct_size = sizeof(api),
        .abi_major = OASP_EXTENSION_ABI_MAJOR,
        .abi_minor = OASP_EXTENSION_ABI_MINOR,
        .query_api = host_query_api
    };
    return &api;
}

const oasp_ext_async_api *oasp_external_async_api(void) {
    return &async_api;
}

const oasp_ext_runtime_api *oasp_external_runtime_api(void) {
    return &runtime_api;
}
