// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include <openasp/ai.h>

#include <stdlib.h>
#include <string.h>

static int enumerate(void *worker, oasp_ai_device_info *devices, size_t *count, oasp_ai_error *error) {
    (void)worker;
    (void)error;
    if (!count)
        return -1;
    if (devices && *count) {
        devices[0] = (oasp_ai_device_info){.struct_size = sizeof(*devices), .name = {"Mock NPU", 8}};
        *count = 1;
    } else
        *count = 1;
    return 0;
}

static int open_device(void *worker, uint32_t ordinal, const oasp_ai_device_options *options, oasp_ai_device *device, oasp_ai_error *error) {
    (void)worker;
    (void)options;
    (void)error;
    if (ordinal)
        return -1;
    *device = (oasp_ai_device)(uintptr_t)malloc(1);
    return *device ? 0 : -1;
}

static void close_device(oasp_ai_device device) {
    free((void *)(uintptr_t)device);
}

static int capabilities(oasp_ai_device device, oasp_ai_capability_query *query, oasp_ai_error *error) {
    (void)device;
    (void)error;
    query->supported = query->op_id == OASP_AI_OP_ADD && query->input_dtype == OASP_AI_DTYPE_F32;
    query->deterministic = 1;
    query->dynamic_shape = OASP_AI_DYNAMIC_STATIC;
    return 0;
}

static int allocate(oasp_ai_device device, const oasp_ai_alloc_desc *desc, oasp_ai_backend_storage *storage, oasp_ai_error *error) {
    (void)device;
    (void)error;
    void *data = calloc(1, (size_t)desc->bytes);
    *storage = (oasp_ai_backend_storage)(uintptr_t)data;
    return data ? 0 : -1;
}

static void release_storage(oasp_ai_device device, oasp_ai_backend_storage storage) {
    (void)device;
    free((void *)(uintptr_t)storage);
}

static int copy_data(oasp_ai_stream stream, const oasp_ai_copy_desc *desc, oasp_ai_event *event, oasp_ai_error *error) {
    (void)stream;
    (void)error;
    const unsigned char *source = desc->source_host ? desc->source_host : (const unsigned char *)(uintptr_t)desc->source;
    unsigned char *destination = desc->destination_host ? desc->destination_host : (unsigned char *)(uintptr_t)desc->destination;
    if (!source || !destination)
        return -1;
    memcpy(destination + desc->destination_offset, source + desc->source_offset, (size_t)desc->bytes);
    if (event)
        *event = 1;
    return 0;
}

static int create_stream(oasp_ai_device device, const oasp_ai_stream_desc *desc, oasp_ai_stream *stream, oasp_ai_error *error) {
    (void)desc;
    (void)error;
    *stream = device;
    return 0;
}

static void destroy_stream(oasp_ai_stream stream) {
    (void)stream;
}

static int record_event(oasp_ai_stream stream, oasp_ai_event *event, oasp_ai_error *error) {
    (void)error;
    if (!stream || !event)
        return -1;
    *event = 1;
    return 0;
}

static int wait_event(oasp_ai_stream stream, oasp_ai_event event, oasp_ai_error *error) {
    (void)error;
    return stream && event ? 0 : -1;
}

static int synchronize(oasp_ai_stream stream, oasp_ai_error *error) {
    (void)error;
    return stream ? 0 : -1;
}

static int execute_op(oasp_ai_stream stream, const oasp_ai_op_desc *op, oasp_ai_event *event, oasp_ai_error *error) {
    (void)error;
    if (!stream || !op || op->op_id != OASP_AI_OP_ADD || op->input_count != 2 || op->output_count != 1 ||
        op->inputs[0].dtype != OASP_AI_DTYPE_F32 || op->inputs[1].dtype != OASP_AI_DTYPE_F32 ||
        op->outputs[0].dtype != OASP_AI_DTYPE_F32)
        return -1;
    const float *left = (const float *)(uintptr_t)op->inputs[0].storage;
    const float *right = (const float *)(uintptr_t)op->inputs[1].storage;
    float *output = (float *)(uintptr_t)op->outputs[0].storage;
    for (uint64_t index = 0; index < op->outputs[0].element_count; index++)
        output[index] = left[index] + right[index];
    if (event)
        *event = 1;
    return 0;
}

static int compile_graph(oasp_ai_device device, const oasp_ai_graph_desc *graph, oasp_ai_executable *executable, oasp_ai_error *error) {
    (void)device;
    (void)error;
    if (!graph || graph->core_ir_size < 8 || memcmp(graph->core_ir, "OAIRABI1", 8))
        return -1;
    *executable = (oasp_ai_executable)(uintptr_t)malloc(1);
    return *executable ? 0 : -1;
}

static int execute_graph(oasp_ai_stream stream, oasp_ai_executable executable, const oasp_ai_graph_bindings *bindings, oasp_ai_event *event, oasp_ai_error *error) {
    (void)stream;
    (void)error;
    if (!executable || bindings->input_count != 2 || bindings->output_count != 1)
        return -1;
    const float *left = (const float *)(uintptr_t)bindings->inputs[0];
    const float *right = (const float *)(uintptr_t)bindings->inputs[1];
    float *output = (float *)(uintptr_t)bindings->outputs[0];
    for (size_t index = 0; index < 4; index++)
        output[index] = left[index] + right[index];
    if (event)
        *event = 1;
    return 0;
}

static void destroy_executable(oasp_ai_executable executable) {
    free((void *)(uintptr_t)executable);
}

static const oasp_ai_backend_api api = {
    .struct_size = sizeof(api),
    .enumerate_devices = enumerate,
    .open_device = open_device,
    .close_device = close_device,
    .query_capabilities = capabilities,
    .alloc = allocate,
    .free = release_storage,
    .copy = copy_data,
    .create_stream = create_stream,
    .destroy_stream = destroy_stream,
    .record_event = record_event,
    .wait_event = wait_event,
    .synchronize = synchronize,
    .execute_op = execute_op,
    .compile_graph = compile_graph,
    .execute_graph = execute_graph,
    .destroy_executable = destroy_executable,
};

static const oasp_ai_backend_descriptor descriptor = {
    .struct_size = sizeof(descriptor),
    .abi_major_required = OASP_AI_BACKEND_ABI_MAJOR,
    .abi_minor_required = OASP_AI_BACKEND_ABI_MINOR,
    .backend_name = "mock",
    .backend_version = "1.0.0",
    .api = &api,
};

const oasp_ai_backend_descriptor *oasp_ai_test_mock_backend(void) {
    return &descriptor;
}
