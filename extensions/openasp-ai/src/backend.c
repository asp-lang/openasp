#define _POSIX_C_SOURCE 200809L

// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <dlfcn.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    oasp_ai_memory_stats stats;
} reference_device;

typedef struct {
    reference_device *device;
} reference_stream;

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} backend_ir_buffer;

static int backend_ir_append(backend_ir_buffer *buffer, const void *data, size_t size) {
    if (buffer->size > SIZE_MAX - size)
        return -1;
    size_t required = buffer->size + size;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2)
                return -1;
            capacity *= 2;
        }
        unsigned char *grown = realloc(buffer->data, capacity);
        if (!grown)
            return -1;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 0;
}

static int backend_ir_u32(backend_ir_buffer *buffer, uint32_t value) {
    unsigned char bytes[] = {(unsigned char)value, (unsigned char)(value >> 8), (unsigned char)(value >> 16), (unsigned char)(value >> 24)};
    return backend_ir_append(buffer, bytes, sizeof(bytes));
}

static int backend_ir_u64(backend_ir_buffer *buffer, uint64_t value) {
    unsigned char bytes[8];
    for (unsigned index = 0; index < 8; index++)
        bytes[index] = (unsigned char)(value >> (index * 8));
    return backend_ir_append(buffer, bytes, sizeof(bytes));
}

int oasp_ai_backend_ir_build(oasp_ai_graph *graph, void **data, size_t *size, oasp_ai_error *error) {
    backend_ir_buffer buffer = {0};
    int failed = backend_ir_append(&buffer, "OAIRABI1", 8) || backend_ir_u32(&buffer, 1) ||
                 backend_ir_u32(&buffer, (uint32_t)graph->value_count) ||
                 backend_ir_u32(&buffer, (uint32_t)graph->operation_count) ||
                 backend_ir_u32(&buffer, (uint32_t)graph->input_count) ||
                 backend_ir_u32(&buffer, (uint32_t)graph->output_count);
    for (size_t index = 0; index < graph->value_count && !failed; index++) {
        oasp_ai_ir_value *value = &graph->values[index];
        uint64_t bytes = value->constant ? oasp_ai_tensor_bytes(value->constant) : 0;
        failed = backend_ir_u32(&buffer, value->id) || backend_ir_u32(&buffer, value->desc.dtype) ||
                 backend_ir_u32(&buffer, value->desc.rank) || backend_ir_u32(&buffer, value->constant != NULL);
        for (uint32_t axis = 0; axis < value->desc.rank && !failed; axis++)
            failed = backend_ir_u64(&buffer, (uint64_t)value->desc.shape[axis]) ||
                     backend_ir_u64(&buffer, (uint64_t)value->desc.strides[axis]);
        if (!failed)
            failed = backend_ir_u64(&buffer, bytes);
        if (!failed && value->constant) {
            oasp_ai_tensor *contiguous = oasp_ai_tensor_contiguous(value->constant, error);
            if (!contiguous)
                failed = 1;
            else {
                failed = backend_ir_append(&buffer, oasp_ai_tensor_const_data(contiguous), (size_t)bytes);
                oasp_ai_tensor_release(contiguous);
            }
        }
    }
    for (size_t index = 0; index < graph->operation_count && !failed; index++) {
        oasp_ai_ir_operation *operation = &graph->operations[index];
        failed = backend_ir_u32(&buffer, operation->op_id) || backend_ir_u32(&buffer, operation->op_version) ||
                 backend_ir_u32(&buffer, operation->flags) || backend_ir_u32(&buffer, operation->output) ||
                 backend_ir_u32(&buffer, (uint32_t)operation->input_count) ||
                 backend_ir_u64(&buffer, operation->attributes_size);
        for (size_t input = 0; input < operation->input_count && !failed; input++)
            failed = backend_ir_u32(&buffer, operation->inputs[input]);
        if (!failed)
            failed = backend_ir_append(&buffer, operation->attributes, operation->attributes_size);
    }
    for (size_t index = 0; index < graph->input_count && !failed; index++)
        failed = backend_ir_u32(&buffer, graph->inputs[index]);
    for (size_t index = 0; index < graph->output_count && !failed; index++)
        failed = backend_ir_u32(&buffer, graph->outputs[index]);
    if (failed) {
        free(buffer.data);
        oasp_ai_set_error(error, 7, "BackendIR", "unable to serialize normalized Core IR", "Reduce graph size or memory pressure.");
        return -1;
    }
    *data = buffer.data;
    *size = buffer.size;
    return 0;
}

static int reference_enumerate(void *worker, oasp_ai_device_info *devices, size_t *count, oasp_ai_error *error) {
    (void)worker;
    (void)error;
    if (!count)
        return -1;
    if (!devices || !*count) {
        *count = 1;
        return 0;
    }
    devices[0] = (oasp_ai_device_info){
        .struct_size = sizeof(devices[0]),
        .ordinal = 0,
        .name = {"Reference CPU", 13},
        .total_memory = UINT64_MAX,
        .free_memory = UINT64_MAX,
        .flags = 1,
    };
    *count = 1;
    return 0;
}

static int reference_open(void *worker, uint32_t ordinal, const oasp_ai_device_options *options, oasp_ai_device *device, oasp_ai_error *error) {
    (void)worker;
    (void)options;
    if (ordinal || !device) {
        oasp_ai_set_error(error, 5, "OpenDevice", "reference CPU only provides device zero", "Use cpu:0.");
        return -1;
    }
    reference_device *state = calloc(1, sizeof(*state));
    if (!state)
        return -1;
    state->stats.struct_size = sizeof(state->stats);
    *device = (oasp_ai_device)(uintptr_t)state;
    return 0;
}

static void reference_close(oasp_ai_device device) {
    free((void *)(uintptr_t)device);
}

static int reference_query(oasp_ai_device device, oasp_ai_capability_query *query, oasp_ai_error *error) {
    (void)device;
    (void)error;
    if (!query)
        return -1;
    query->supported = query->op_id >= OASP_AI_OP_ZEROS && query->op_id <= OASP_AI_OP_SOFTMAX && query->op_version == 1;
    query->minimum_rank = 1;
    query->maximum_rank = OASP_AI_MAX_RANK;
    query->maximum_workspace = 0;
    query->dynamic_shape = OASP_AI_DYNAMIC_FULL;
    query->deterministic = 1;
    query->training = 1;
    return 0;
}

static int reference_alloc(oasp_ai_device handle, const oasp_ai_alloc_desc *desc, oasp_ai_backend_storage *storage, oasp_ai_error *error) {
    reference_device *device = (reference_device *)(uintptr_t)handle;
    if (!device || !desc || !storage || desc->bytes > SIZE_MAX) {
        oasp_ai_set_error(error, 7, "BackendAlloc", "invalid CPU allocation", "Check the allocation size and device.");
        return -1;
    }
    void *data = calloc(1, (size_t)desc->bytes);
    if (!data)
        return -1;
    *storage = (oasp_ai_backend_storage)(uintptr_t)data;
    device->stats.live_bytes += desc->bytes;
    device->stats.allocation_count++;
    if (device->stats.live_bytes > device->stats.peak_bytes)
        device->stats.peak_bytes = device->stats.live_bytes;
    return 0;
}

static void reference_free(oasp_ai_device device, oasp_ai_backend_storage storage) {
    (void)device;
    free((void *)(uintptr_t)storage);
}

static int reference_copy(oasp_ai_stream stream, const oasp_ai_copy_desc *desc, oasp_ai_event *event, oasp_ai_error *error) {
    (void)error;
    if (!stream || !desc)
        return -1;
    const unsigned char *source = desc->source_host ? desc->source_host : (const unsigned char *)(uintptr_t)desc->source;
    unsigned char *destination = desc->destination_host ? desc->destination_host : (unsigned char *)(uintptr_t)desc->destination;
    if (!source || !destination)
        return -1;
    memcpy(destination + desc->destination_offset, source + desc->source_offset, (size_t)desc->bytes);
    if (event)
        *event = 1;
    return 0;
}

static int reference_create_stream(oasp_ai_device device, const oasp_ai_stream_desc *desc, oasp_ai_stream *stream, oasp_ai_error *error) {
    (void)desc;
    (void)error;
    reference_stream *state = calloc(1, sizeof(*state));
    if (!state)
        return -1;
    state->device = (reference_device *)(uintptr_t)device;
    *stream = (oasp_ai_stream)(uintptr_t)state;
    return 0;
}

static void reference_destroy_stream(oasp_ai_stream stream) {
    free((void *)(uintptr_t)stream);
}

static int reference_record(oasp_ai_stream stream, oasp_ai_event *event, oasp_ai_error *error) {
    (void)error;
    if (!stream || !event)
        return -1;
    *event = 1;
    return 0;
}

static int reference_wait(oasp_ai_stream stream, oasp_ai_event event, oasp_ai_error *error) {
    (void)error;
    return stream && event ? 0 : -1;
}

static int reference_synchronize(oasp_ai_stream stream, oasp_ai_error *error) {
    (void)error;
    return stream ? 0 : -1;
}

static int reference_execute_op(oasp_ai_stream stream, const oasp_ai_op_desc *op, oasp_ai_event *event, oasp_ai_error *error) {
    (void)stream;
    (void)op;
    (void)event;
    oasp_ai_set_error(error, 5, "ExecuteOp", "raw backend execution requires normalized tensor descriptors",
                      "Use OpenASP.AI execution plans, which provide validated descriptors.");
    return -1;
}

static int reference_compile_graph(oasp_ai_device device, const oasp_ai_graph_desc *graph, oasp_ai_executable *executable, oasp_ai_error *error) {
    (void)device;
    (void)error;
    if (!graph || !executable)
        return -1;
    *executable = (oasp_ai_executable)(uintptr_t)graph->core_ir;
    return 0;
}

static int reference_execute_graph(oasp_ai_stream stream, oasp_ai_executable executable, const oasp_ai_graph_bindings *bindings, oasp_ai_event *event, oasp_ai_error *error) {
    (void)stream;
    (void)executable;
    (void)bindings;
    (void)event;
    oasp_ai_set_error(error, 5, "ExecuteGraph", "reference graph executable is owned by the AI Core", "Execute the compiled OpenASP.AI Plan.");
    return -1;
}

static void reference_destroy_executable(oasp_ai_executable executable) {
    (void)executable;
}

static int reference_memory_stats(oasp_ai_device handle, oasp_ai_memory_stats *stats, oasp_ai_error *error) {
    (void)error;
    reference_device *device = (reference_device *)(uintptr_t)handle;
    if (!device || !stats)
        return -1;
    *stats = device->stats;
    return 0;
}

static int reference_trim(oasp_ai_device device, uint64_t target_bytes, oasp_ai_error *error) {
    (void)device;
    (void)target_bytes;
    (void)error;
    return 0;
}

static const oasp_ai_backend_api reference_api = {
    .struct_size = sizeof(reference_api),
    .enumerate_devices = reference_enumerate,
    .open_device = reference_open,
    .close_device = reference_close,
    .query_capabilities = reference_query,
    .alloc = reference_alloc,
    .free = reference_free,
    .copy = reference_copy,
    .create_stream = reference_create_stream,
    .destroy_stream = reference_destroy_stream,
    .record_event = reference_record,
    .wait_event = reference_wait,
    .synchronize = reference_synchronize,
    .execute_op = reference_execute_op,
    .compile_graph = reference_compile_graph,
    .execute_graph = reference_execute_graph,
    .destroy_executable = reference_destroy_executable,
    .memory_stats = reference_memory_stats,
    .trim_memory = reference_trim,
};

static int reference_module_init(const oasp_ai_host_api *host, void **module) {
    (void)host;
    *module = NULL;
    return 0;
}

static int reference_worker_init(void *module, void **worker, oasp_ai_error *error) {
    (void)module;
    (void)error;
    *worker = NULL;
    return 0;
}

static const oasp_ai_backend_descriptor reference_descriptor = {
    .struct_size = sizeof(reference_descriptor),
    .abi_major_required = OASP_AI_BACKEND_ABI_MAJOR,
    .abi_minor_required = OASP_AI_BACKEND_ABI_MINOR,
    .backend_name = "cpu",
    .backend_version = "1.0.0",
    .flags = 0,
    .module_init = reference_module_init,
    .worker_init = reference_worker_init,
    .api = &reference_api,
};

OASP_AI_BACKEND_EXPORT const oasp_ai_backend_descriptor *openasp_ai_backend(void) {
    return &reference_descriptor;
}

int oasp_ai_context_register_backend(oasp_ai_context *context, const oasp_ai_backend_descriptor *descriptor, oasp_ai_error *error) {
    if (!context || !descriptor || descriptor->struct_size < sizeof(*descriptor) || descriptor->abi_major_required != OASP_AI_BACKEND_ABI_MAJOR ||
        descriptor->abi_minor_required > OASP_AI_BACKEND_ABI_MINOR || !descriptor->backend_name || !descriptor->api ||
        descriptor->api->struct_size < sizeof(*descriptor->api)) {
        oasp_ai_set_error(error, 5, "RegisterBackend", "backend ABI descriptor is incompatible", "Rebuild the backend against OpenASP AI Backend ABI 1.4.");
        return -1;
    }
    for (size_t index = 0; index < context->backend_count; index++)
        if (!strcmp(context->backends[index].descriptor->backend_name, descriptor->backend_name)) {
            oasp_ai_set_error(error, 5, "RegisterBackend", "backend name is already registered", "Remove the duplicate backend configuration.");
            return -1;
        }
    oasp_ai_host_api host = {.struct_size = sizeof(host), .version = 1};
    void *module = NULL;
    void *worker = NULL;
    if (descriptor->module_init && descriptor->module_init(&host, &module))
        return -1;
    if (descriptor->worker_init && descriptor->worker_init(module, &worker, error)) {
        if (descriptor->module_shutdown)
            descriptor->module_shutdown(module);
        return -1;
    }
    size_t device_count = 0;
    if (!descriptor->api->enumerate_devices ||
        descriptor->api->enumerate_devices(worker, NULL, &device_count, error) || !device_count) {
        if (descriptor->worker_shutdown)
            descriptor->worker_shutdown(worker);
        if (descriptor->module_shutdown)
            descriptor->module_shutdown(module);
        oasp_ai_set_error(error, 5, "RegisterBackend", "backend did not expose any devices", "Check the device driver and backend installation.");
        return -1;
    }
    oasp_ai_registered_backend *grown =
        oasp_ai_grow_array(context->backends, sizeof(*context->backends), &context->backend_capacity, context->backend_count + device_count);
    if (!grown) {
        if (descriptor->worker_shutdown)
            descriptor->worker_shutdown(worker);
        if (descriptor->module_shutdown)
            descriptor->module_shutdown(module);
        return -1;
    }
    context->backends = grown;
    oasp_ai_device_options device_options = {
        .struct_size = sizeof(device_options),
        .memory_limit = context->options.maximum_model_bytes,
        .compute_streams = 1,
        .transfer_streams = 1,
    };
    oasp_ai_stream_desc stream_desc = {.struct_size = sizeof(stream_desc)};
    size_t opened = 0;
    for (; opened < device_count; opened++) {
        oasp_ai_registered_backend *backend = &context->backends[context->backend_count + opened];
        memset(backend, 0, sizeof(*backend));
        backend->descriptor = descriptor;
        backend->module = module;
        backend->worker = worker;
        backend->ordinal = (uint32_t)opened;
        backend->owns_runtime = opened == 0;
        if (!descriptor->api->open_device ||
            descriptor->api->open_device(worker, backend->ordinal, &device_options, &backend->device, error) ||
            !descriptor->api->create_stream ||
            descriptor->api->create_stream(backend->device, &stream_desc, &backend->compute_stream, error) ||
            descriptor->api->create_stream(backend->device, &stream_desc, &backend->transfer_stream, error))
            break;
    }
    if (opened != device_count) {
        for (size_t index = 0; index <= opened && index < device_count; index++) {
            oasp_ai_registered_backend *backend = &context->backends[context->backend_count + index];
            if (backend->transfer_stream && descriptor->api->destroy_stream)
                descriptor->api->destroy_stream(backend->transfer_stream);
            if (backend->compute_stream && descriptor->api->destroy_stream)
                descriptor->api->destroy_stream(backend->compute_stream);
            if (backend->device && descriptor->api->close_device)
                descriptor->api->close_device(backend->device);
            memset(backend, 0, sizeof(*backend));
        }
        if (descriptor->worker_shutdown)
            descriptor->worker_shutdown(worker);
        if (descriptor->module_shutdown)
            descriptor->module_shutdown(module);
        return -1;
    }
    context->backend_count += device_count;
    return 0;
}

int oasp_ai_backend_find(oasp_ai_context *context, const char *device, oasp_ai_error *error) {
    if (!context || !device || !*device)
        return -2;
    if (!strcmp(device, "cpu") || !strcmp(device, "cpu:0"))
        return -1;
    const char *separator = strrchr(device, ':');
    if (!separator || separator == device || !separator[1]) {
        oasp_ai_set_error(error, 5, "Device", "device name must include an ordinal", "Use a device name such as cuda:0.");
        return -2;
    }
    char *end = NULL;
    unsigned long ordinal = strtoul(separator + 1, &end, 10);
    size_t name_length = (size_t)(separator - device);
    if (!end || *end || ordinal > UINT32_MAX) {
        oasp_ai_set_error(error, 5, "Device", "device ordinal is invalid", "Use a non-negative device ordinal.");
        return -2;
    }
    for (size_t index = 0; index < context->backend_count; index++) {
        oasp_ai_registered_backend *backend = &context->backends[index];
        if (strlen(backend->descriptor->backend_name) == name_length &&
            !strncmp(backend->descriptor->backend_name, device, name_length) && backend->ordinal == (uint32_t)ordinal)
            return (int)index;
    }
    oasp_ai_set_error(error, 5, "Device", "requested device is not available", "Load the backend and select an enumerated device.");
    return -2;
}

int oasp_ai_tensor_same_device(oasp_ai_tensor *const *tensors, size_t count, int32_t *backend_index, oasp_ai_error *error) {
    if (!tensors || !count || !backend_index)
        return -1;
    int32_t selected = oasp_ai_tensor_backend_index(tensors[0]);
    for (size_t index = 0; index < count; index++)
        if (!tensors[index] || oasp_ai_tensor_backend_index(tensors[index]) != selected) {
            oasp_ai_set_error(error, 5, "Device", "operator inputs are on different devices", "Move all operands to the same device.");
            return -1;
        }
    *backend_index = selected;
    return 0;
}

int oasp_ai_backend_execute(oasp_ai_context *context, int32_t backend_index, uint32_t operation,
                            oasp_ai_tensor *const *inputs, size_t input_count, oasp_ai_tensor *const *outputs,
                            size_t output_count, const void *attributes, size_t attributes_size, oasp_ai_error *error) {
    if (!context || backend_index < 0 || (size_t)backend_index >= context->backend_count)
        return -1;
    oasp_ai_registered_backend *backend = &context->backends[backend_index];
    if (!backend->descriptor->api->execute_op) {
        oasp_ai_set_error(error, 5, "ExecuteOp", "backend does not support eager execution", "Compile a graph or install an eager-capable backend.");
        return -1;
    }
    oasp_ai_tensor_view *input_views = calloc(input_count, sizeof(*input_views));
    oasp_ai_tensor_view *output_views = calloc(output_count, sizeof(*output_views));
    if ((input_count && !input_views) || (output_count && !output_views)) {
        free(input_views);
        free(output_views);
        return -1;
    }
    int status = 0;
    for (size_t index = 0; index < input_count && !status; index++)
        status = oasp_ai_tensor_view_build(inputs[index], &input_views[index]);
    for (size_t index = 0; index < output_count && !status; index++)
        status = oasp_ai_tensor_view_build(outputs[index], &output_views[index]);
    oasp_ai_op_desc descriptor = {
        .struct_size = sizeof(descriptor),
        .op_id = operation,
        .op_version = 1,
        .inputs = input_views,
        .input_count = input_count,
        .outputs = output_views,
        .output_count = output_count,
        .attributes = attributes,
        .attributes_size = attributes_size,
    };
    if (!status)
        status = backend->descriptor->api->execute_op(backend->compute_stream, &descriptor, NULL, error);
    free(input_views);
    free(output_views);
    return status;
}

size_t oasp_ai_context_device_count(const oasp_ai_context *context) {
    return context ? context->backend_count : 0;
}

int oasp_ai_context_device_info(const oasp_ai_context *context, size_t index, oasp_ai_device_info *info, oasp_ai_error *error) {
    if (!context || index >= context->backend_count || !info)
        return -1;
    oasp_ai_registered_backend *backend = &context->backends[index];
    size_t count = (size_t)backend->ordinal + 1;
    oasp_ai_device_info *devices = calloc(count, sizeof(*devices));
    int status = !devices || backend->descriptor->api->enumerate_devices(backend->worker, devices, &count, error);
    if (!status && backend->ordinal >= count)
        status = -1;
    if (!status)
        *info = devices[backend->ordinal];
    free(devices);
    return status;
}

int oasp_ai_tensor_all_reduce(oasp_ai_tensor *const *tensors, size_t count, oasp_ai_collective_reduction reduction, oasp_ai_error *error) {
    if (!tensors || !count || reduction > OASP_AI_COLLECTIVE_MAX) {
        oasp_ai_set_error(error, 5, "AllReduce", "invalid collective arguments", "Pass one contiguous tensor per participating device.");
        return -1;
    }
    if (count == 1)
        return tensors[0] ? 0 : -1;
    oasp_ai_context *context = tensors[0] ? tensors[0]->context : NULL;
    int32_t first_index = oasp_ai_tensor_backend_index(tensors[0]);
    if (!context || first_index < 0)
        return -1;
    oasp_ai_registered_backend *first = &context->backends[first_index];
    if (!first->descriptor->api->all_reduce) {
        oasp_ai_set_error(error, 38, "AllReduce", "backend was built without collective support",
                          "Install the collective runtime and rebuild the backend with collective support enabled.");
        return -1;
    }
    oasp_ai_stream *streams = calloc(count, sizeof(*streams));
    oasp_ai_tensor_view *views = calloc(count, sizeof(*views));
    int status = !streams || !views;
    for (size_t index = 0; index < count && !status; index++) {
        int32_t backend_index = oasp_ai_tensor_backend_index(tensors[index]);
        if (backend_index < 0 || tensors[index]->context != context ||
            context->backends[backend_index].descriptor != first->descriptor ||
            tensors[index]->desc.dtype != tensors[0]->desc.dtype ||
            tensors[index]->element_count != tensors[0]->element_count ||
            !(tensors[index]->desc.flags & OASP_AI_TENSOR_CONTIGUOUS)) {
            oasp_ai_set_error(error, 5, "AllReduce", "collective tensors are incompatible",
                              "Use equal contiguous tensors on distinct devices of the same backend.");
            status = -1;
            break;
        }
        for (size_t previous = 0; previous < index; previous++)
            if (oasp_ai_tensor_backend_index(tensors[previous]) == backend_index) {
                oasp_ai_set_error(error, 5, "AllReduce", "a device appears more than once",
                                  "Pass exactly one tensor for each participating device.");
                status = -1;
                break;
            }
        streams[index] = context->backends[backend_index].compute_stream;
        if (!status)
            status = oasp_ai_tensor_view_build(tensors[index], &views[index]);
    }
    if (!status)
        status = first->descriptor->api->all_reduce(streams, views, count, reduction, error);
    free(streams);
    free(views);
    return status;
}

static int sha256_matches(const char *path, const char *expected) {
    if (!expected || !*expected)
        return 1;
    if (strlen(expected) != 64)
        return 0;
    for (size_t index = 0; index < 64; index++)
        if (!((expected[index] >= '0' && expected[index] <= '9') || (expected[index] >= 'a' && expected[index] <= 'f')))
            return 0;
    FILE *file = fopen(path, "rb");
    EVP_MD_CTX *digest_context = file ? EVP_MD_CTX_new() : NULL;
    int valid = digest_context && EVP_DigestInit_ex(digest_context, EVP_sha256(), NULL) == 1;
    unsigned char buffer[16384];
    size_t count;
    while (valid && (count = fread(buffer, 1, sizeof(buffer), file)) != 0)
        valid = EVP_DigestUpdate(digest_context, buffer, count) == 1;
    valid = valid && !ferror(file);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    valid = valid && EVP_DigestFinal_ex(digest_context, digest, &digest_size) == 1 && digest_size == 32;
    EVP_MD_CTX_free(digest_context);
    if (file)
        fclose(file);
    static const char hexadecimal[] = "0123456789abcdef";
    char actual[65];
    for (size_t index = 0; valid && index < 32; index++) {
        actual[index * 2] = hexadecimal[digest[index] >> 4];
        actual[index * 2 + 1] = hexadecimal[digest[index] & 15];
    }
    actual[64] = '\0';
    return valid && !strcmp(actual, expected);
}

int oasp_ai_context_load_backend(oasp_ai_context *context, const char *absolute_path, const char *expected_sha256, oasp_ai_error *error) {
    struct stat info;
    if (!context || !absolute_path || absolute_path[0] != '/' || lstat(absolute_path, &info) || !S_ISREG(info.st_mode) ||
        (info.st_mode & (S_IWGRP | S_IWOTH))) {
        oasp_ai_set_error(error, 13, "LoadBackend", "backend path is unsafe", "Use an absolute regular file that is not group/world writable.");
        return -1;
    }
    if (!sha256_matches(absolute_path, expected_sha256)) {
        oasp_ai_set_error(error, 13, "LoadBackend", "backend SHA-256 validation failed",
                          "Configure the exact lowercase SHA-256 digest of the trusted backend library.");
        return -1;
    }
    void *library = dlopen(absolute_path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        oasp_ai_set_error(error, 2, "LoadBackend", "unable to load backend dynamic library", "Check architecture, dependencies, and file permissions.");
        return -1;
    }
    oasp_ai_backend_entry entry;
    *(void **)(&entry) = dlsym(library, "openasp_ai_backend");
    size_t first_backend = context->backend_count;
    if (!entry || oasp_ai_context_register_backend(context, entry(), error)) {
        dlclose(library);
        return -1;
    }
    context->backends[first_backend].dynamic_library = library;
    return 0;
}
