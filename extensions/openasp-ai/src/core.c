#define _POSIX_C_SOURCE 200809L

// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void oasp_ai_set_error(oasp_ai_error *error, int code, const char *operation, const char *message, const char *suggestion) {
    if (!error)
        return;
    *error = (oasp_ai_error){
        .struct_size = sizeof(*error),
        .code = code,
        .operation = operation,
        .message = message,
        .suggestion = suggestion,
    };
}

char *oasp_ai_string_duplicate(const char *value) {
    if (!value)
        return NULL;
    size_t length = strlen(value);
    char *copy = malloc(length + 1);
    if (copy)
        memcpy(copy, value, length + 1);
    return copy;
}

void *oasp_ai_grow_array(void *items, size_t element_size, size_t *capacity, size_t required) {
    if (*capacity >= required)
        return items;
    size_t next = *capacity ? *capacity : 8;
    while (next < required) {
        if (next > SIZE_MAX / 2)
            return NULL;
        next *= 2;
    }
    if (element_size && next > SIZE_MAX / element_size)
        return NULL;
    void *grown = realloc(items, next * element_size);
    if (grown)
        *capacity = next;
    return grown;
}

uint64_t oasp_ai_now_nanoseconds(void) {
    struct timespec value;
    if (timespec_get(&value, TIME_UTC) != TIME_UTC)
        return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

size_t oasp_ai_dtype_size(oasp_ai_dtype dtype) {
    static const uint8_t sizes[] = {1, 1, 1, 2, 4, 8, 2, 2, 4, 8};
    return dtype <= OASP_AI_DTYPE_F64 ? sizes[dtype] : 0;
}

const char *oasp_ai_dtype_name(oasp_ai_dtype dtype) {
    static const char *const names[] = {"bool", "uint8", "int8", "int16", "int32", "int64", "float16", "bfloat16", "float32", "float64"};
    return dtype <= OASP_AI_DTYPE_F64 ? names[dtype] : "unknown";
}

int oasp_ai_dtype_parse(const char *name, oasp_ai_dtype *dtype) {
    if (!name || !dtype)
        return -1;
    for (int value = OASP_AI_DTYPE_BOOL; value <= OASP_AI_DTYPE_F64; value++) {
        if (!strcmp(name, oasp_ai_dtype_name((oasp_ai_dtype)value))) {
            *dtype = (oasp_ai_dtype)value;
            return 0;
        }
    }
    return -1;
}

int oasp_ai_checked_elements(const int64_t *shape, uint32_t rank, uint64_t *elements, oasp_ai_error *error) {
    if (!shape || !elements || !rank || rank > OASP_AI_MAX_RANK) {
        oasp_ai_set_error(error, 5, "Shape", "tensor rank must be between 1 and 8", "Pass a non-empty shape with at most eight dimensions.");
        return -1;
    }
    uint64_t count = 1;
    for (uint32_t index = 0; index < rank; index++) {
        if (shape[index] <= 0 || (uint64_t)shape[index] > UINT64_MAX / count) {
            oasp_ai_set_error(error, 6, "Shape", "tensor shape is invalid or overflows", "Use positive dimensions whose product fits in 64 bits.");
            return -1;
        }
        count *= (uint64_t)shape[index];
    }
    *elements = count;
    return 0;
}

oasp_ai_context *oasp_ai_context_create(const oasp_ai_context_options *options, oasp_ai_error *error) {
    oasp_ai_context *context = calloc(1, sizeof(*context));
    if (!context) {
        oasp_ai_set_error(error, 7, "Context", "unable to allocate AI context", "Reduce process memory pressure.");
        return NULL;
    }
    context->options = options ? *options : (oasp_ai_context_options){0};
    if (!context->options.maximum_tensor_bytes)
        context->options.maximum_tensor_bytes = UINT64_C(1) << 30;
    if (!context->options.maximum_model_bytes)
        context->options.maximum_model_bytes = UINT64_C(2) << 30;
    if (!context->options.maximum_array_elements)
        context->options.maximum_array_elements = UINT64_C(1) << 20;
    if (!context->options.maximum_plan_cache_entries)
        context->options.maximum_plan_cache_entries = 16;
    if (!context->options.maximum_trace_events)
        context->options.maximum_trace_events = 65536;
    if (!context->options.seed)
        context->options.seed = UINT64_C(0x4f415350414931);
    if (pthread_mutex_init(&context->mutex, NULL)) {
        free(context);
        oasp_ai_set_error(error, 7, "Context", "unable to initialize context synchronization", "Reduce process resource pressure.");
        return NULL;
    }
    context->mutex_initialized = 1;
    if (oasp_ai_context_register_backend(context, openasp_ai_backend(), error)) {
        pthread_mutex_destroy(&context->mutex);
        free(context);
        return NULL;
    }
    return context;
}

void oasp_ai_context_destroy(oasp_ai_context *context) {
    if (!context)
        return;
    for (size_t index = 0; index < context->plan_cache_count; index++)
        oasp_ai_plan_release(context->plan_cache[index].plan);
    free(context->plan_cache);
    free(context->trace_events);
    for (size_t index = context->backend_count; index; index--) {
        oasp_ai_registered_backend *backend = &context->backends[index - 1];
        if (backend->descriptor->api->destroy_stream && backend->transfer_stream)
            backend->descriptor->api->destroy_stream(backend->transfer_stream);
        if (backend->descriptor->api->destroy_stream && backend->compute_stream)
            backend->descriptor->api->destroy_stream(backend->compute_stream);
        if (backend->descriptor->api->close_device && backend->device)
            backend->descriptor->api->close_device(backend->device);
        if (backend->owns_runtime && backend->descriptor->worker_shutdown && backend->worker)
            backend->descriptor->worker_shutdown(backend->worker);
        if (backend->owns_runtime && backend->descriptor->module_shutdown && backend->module)
            backend->descriptor->module_shutdown(backend->module);
        if (backend->dynamic_library)
            dlclose(backend->dynamic_library);
    }
    free(context->backends);
    if (context->mutex_initialized)
        pthread_mutex_destroy(&context->mutex);
    free(context);
}

const oasp_ai_metrics *oasp_ai_context_metrics(const oasp_ai_context *context) {
    return context ? &context->metrics : NULL;
}

int oasp_ai_context_synchronize(oasp_ai_context *context, oasp_ai_error *error) {
    if (!context) {
        oasp_ai_set_error(error, 5, "Synchronize", "AI context is required", "Use an object created by OpenASP.AI.");
        return -1;
    }
    for (size_t index = 0; index < context->backend_count; index++) {
        oasp_ai_registered_backend *backend = &context->backends[index];
        if (backend->compute_stream && backend->descriptor->api->synchronize &&
            backend->descriptor->api->synchronize(backend->compute_stream, error))
            return -1;
        if (backend->transfer_stream && backend->descriptor->api->synchronize &&
            backend->descriptor->api->synchronize(backend->transfer_stream, error))
            return -1;
    }
    pthread_mutex_lock(&context->mutex);
    context->metrics.synchronization_count++;
    pthread_mutex_unlock(&context->mutex);
    return 0;
}

uint32_t oasp_ai_crc32(const void *data, size_t size) {
    const unsigned char *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < size; index++) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

int oasp_ai_sha256(const void *data, size_t size, unsigned char digest[32]) {
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned length = 0;
    int status = !context || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1 ||
                 (size && EVP_DigestUpdate(context, data, size) != 1) ||
                 EVP_DigestFinal_ex(context, digest, &length) != 1 || length != 32;
    EVP_MD_CTX_free(context);
    return status ? -1 : 0;
}

int oasp_ai_write_atomic(const char *path, int (*writer)(FILE *file, void *state, oasp_ai_error *error), void *state, oasp_ai_error *error) {
    if (!path || !writer) {
        oasp_ai_set_error(error, 5, "Save", "path and writer are required", "Pass a valid model path.");
        return -1;
    }
    size_t length = strlen(path);
    char *temporary = malloc(length + 32);
    if (!temporary) {
        oasp_ai_set_error(error, 7, "Save", "unable to allocate temporary path", "Reduce process memory pressure.");
        return -1;
    }
    snprintf(temporary, length + 32, "%s.tmp.%ld", path, (long)getpid());
    FILE *file = fopen(temporary, "w+b");
    int status = file ? writer(file, state, error) : -1;
    if (!file)
        oasp_ai_set_error(error, errno, "Save", "unable to create temporary model file", "Check the model directory and write permissions.");
    if (file && !status && (fflush(file) || fsync(fileno(file))))
        status = -1;
    if (file && fclose(file))
        status = -1;
    if (file)
        file = NULL;
    if (!status && rename(temporary, path))
        status = -1;
    if (!status) {
        char *parent = oasp_ai_string_duplicate(path);
        if (!parent)
            status = -1;
        else {
            char *separator = strrchr(parent, '/');
            if (separator)
                *separator = '\0';
            else
                strcpy(parent, ".");
            int directory = open(*parent ? parent : "/", O_RDONLY);
            if (directory < 0 || fsync(directory))
                status = -1;
            if (directory >= 0)
                close(directory);
            free(parent);
        }
    }
    if (file)
        fclose(file);
    if (status)
        remove(temporary);
    free(temporary);
    return status;
}
