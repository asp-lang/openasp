// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_AI_BACKEND_H
#define OPENASP_AI_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#define OASP_AI_BACKEND_ABI_MAJOR 1u
#define OASP_AI_BACKEND_ABI_MINOR 4u
#define OASP_AI_MAX_RANK 8u

typedef uintptr_t oasp_ai_device;
typedef uintptr_t oasp_ai_backend_storage;
typedef uintptr_t oasp_ai_stream;
typedef uintptr_t oasp_ai_event;
typedef uintptr_t oasp_ai_executable;

typedef enum {
    OASP_AI_DTYPE_BOOL,
    OASP_AI_DTYPE_U8,
    OASP_AI_DTYPE_I8,
    OASP_AI_DTYPE_I16,
    OASP_AI_DTYPE_I32,
    OASP_AI_DTYPE_I64,
    OASP_AI_DTYPE_F16,
    OASP_AI_DTYPE_BF16,
    OASP_AI_DTYPE_F32,
    OASP_AI_DTYPE_F64
} oasp_ai_dtype;

typedef enum {
    OASP_AI_MEMORY_HOST,
    OASP_AI_MEMORY_HOST_PINNED,
    OASP_AI_MEMORY_DEVICE,
    OASP_AI_MEMORY_UNIFIED,
    OASP_AI_MEMORY_SHARED,
    OASP_AI_MEMORY_REMOTE
} oasp_ai_memory_kind;

typedef enum {
    OASP_AI_DYNAMIC_STATIC,
    OASP_AI_DYNAMIC_BOUNDED,
    OASP_AI_DYNAMIC_FULL,
    OASP_AI_DYNAMIC_RECOMPILE
} oasp_ai_dynamic_shape_mode;

typedef struct {
    uint32_t struct_size;
    int32_t code;
    int32_t backend_code;
    const char *operation;
    const char *message;
    const char *suggestion;
} oasp_ai_error;

typedef struct {
    const char *data;
    size_t length;
} oasp_ai_string;

typedef struct {
    uint32_t struct_size;
    uint32_t ordinal;
    oasp_ai_string name;
    uint64_t total_memory;
    uint64_t free_memory;
    uint64_t flags;
} oasp_ai_device_info;

typedef struct {
    uint32_t struct_size;
    uint64_t memory_limit;
    uint32_t compute_streams;
    uint32_t transfer_streams;
    uint32_t flags;
} oasp_ai_device_options;

typedef struct {
    uint32_t struct_size;
    uint64_t bytes;
    uint32_t alignment;
    oasp_ai_memory_kind memory_kind;
    uint32_t flags;
} oasp_ai_alloc_desc;

typedef struct {
    uint32_t struct_size;
    oasp_ai_backend_storage source;
    oasp_ai_backend_storage destination;
    uint64_t source_offset;
    uint64_t destination_offset;
    uint64_t bytes;
    const void *source_host;
    void *destination_host;
    oasp_ai_memory_kind source_kind;
    oasp_ai_memory_kind destination_kind;
} oasp_ai_copy_desc;

typedef struct {
    uint32_t struct_size;
    uint32_t priority;
    uint32_t flags;
} oasp_ai_stream_desc;

enum {
    OASP_AI_TENSOR_VIEW_CONTIGUOUS = 1u << 0
};

typedef struct {
    uint32_t struct_size;
    oasp_ai_backend_storage storage;
    uint64_t byte_offset;
    uint64_t element_count;
    oasp_ai_dtype dtype;
    uint32_t rank;
    int64_t shape[OASP_AI_MAX_RANK];
    int64_t strides[OASP_AI_MAX_RANK];
    uint32_t flags;
} oasp_ai_tensor_view;

typedef struct {
    double argument;
} oasp_ai_unary_attributes;

typedef struct {
    int32_t axis;
    int32_t keep_dimensions;
} oasp_ai_reduce_attributes;

typedef struct {
    int32_t axis;
} oasp_ai_axis_attributes;

typedef struct {
    double epsilon;
    int32_t rms_only;
} oasp_ai_norm_attributes;

typedef enum {
    OASP_AI_BACKEND_OP_FILL = 0x10000,
    OASP_AI_BACKEND_OP_SUM_TO_SHAPE,
    OASP_AI_BACKEND_OP_UNARY_BACKWARD,
    OASP_AI_BACKEND_OP_BINARY_BACKWARD,
    OASP_AI_BACKEND_OP_REDUCE_BACKWARD,
    OASP_AI_BACKEND_OP_SOFTMAX_BACKWARD,
    OASP_AI_BACKEND_OP_CROSS_ENTROPY_BACKWARD,
    OASP_AI_BACKEND_OP_NORM_BACKWARD,
    OASP_AI_BACKEND_OP_ATTENTION_BACKWARD,
    OASP_AI_BACKEND_OP_OPTIMIZER_STEP,
    OASP_AI_BACKEND_OP_UNSCALE_AND_CHECK,
    OASP_AI_BACKEND_OP_NLL_BACKWARD,
    OASP_AI_BACKEND_OP_CTC_LOSS_BACKWARD
} oasp_ai_backend_operator;

typedef struct {
    double value;
} oasp_ai_fill_attributes;

typedef struct {
    uint32_t forward_op;
    double argument;
} oasp_ai_backward_attributes;

typedef struct {
    uint32_t forward_op;
    int32_t axis;
    int32_t keep_dimensions;
} oasp_ai_reduce_backward_attributes;

typedef struct {
    uint32_t optimizer;
    uint64_t step;
    double learning_rate;
    double weight_decay;
    double momentum;
    double beta1;
    double beta2;
    double epsilon;
} oasp_ai_optimizer_attributes;

typedef struct {
    double inverse_scale;
} oasp_ai_loss_scale_attributes;

typedef struct {
    uint32_t reduction;
} oasp_ai_loss_attributes;

typedef struct {
    uint32_t reduction;
    int64_t ignore_index;
    double label_smoothing;
    int32_t has_weight;
} oasp_ai_cross_entropy_attributes;

typedef struct {
    uint32_t reduction;
    int64_t blank;
    int32_t zero_infinity;
    int32_t targets_are_padded;
} oasp_ai_ctc_attributes;

typedef struct {
    int32_t causal;
} oasp_ai_attention_attributes;

typedef struct {
    uint32_t struct_size;
    uint32_t op_id;
    uint16_t op_version;
    uint16_t flags;
    const oasp_ai_tensor_view *inputs;
    size_t input_count;
    oasp_ai_tensor_view *outputs;
    size_t output_count;
    const void *attributes;
    size_t attributes_size;
} oasp_ai_op_desc;

typedef struct {
    uint32_t struct_size;
    const void *core_ir;
    size_t core_ir_size;
    const void *options;
    size_t options_size;
} oasp_ai_graph_desc;

typedef struct {
    uint32_t struct_size;
    const oasp_ai_backend_storage *inputs;
    size_t input_count;
    oasp_ai_backend_storage *outputs;
    size_t output_count;
} oasp_ai_graph_bindings;

typedef struct {
    uint32_t struct_size;
    uint32_t op_id;
    uint16_t op_version;
    uint16_t flags;
    oasp_ai_dtype input_dtype;
    oasp_ai_dtype output_dtype;
    uint32_t minimum_rank;
    uint32_t maximum_rank;
    uint64_t maximum_workspace;
    oasp_ai_dynamic_shape_mode dynamic_shape;
    int supported;
    int deterministic;
    int training;
} oasp_ai_capability_query;

typedef struct {
    uint32_t struct_size;
    uint64_t live_bytes;
    uint64_t reserved_bytes;
    uint64_t peak_bytes;
    uint64_t workspace_bytes;
    uint64_t allocation_count;
} oasp_ai_memory_stats;

typedef enum {
    OASP_AI_COLLECTIVE_SUM,
    OASP_AI_COLLECTIVE_MEAN,
    OASP_AI_COLLECTIVE_MIN,
    OASP_AI_COLLECTIVE_MAX
} oasp_ai_collective_reduction;

typedef struct oasp_ai_host_api {
    uint32_t struct_size;
    uint32_t version;
    void (*log)(void *context, int level, const char *message, size_t length);
    int (*account_memory)(void *context, int64_t delta, oasp_ai_error *error);
    uint64_t (*monotonic_nanoseconds)(void);
    int (*cancelled)(void *context);
    int64_t (*deadline_milliseconds)(void *context);
    void (*metric)(void *context, const char *name, size_t name_length, double value);
    void *context;
} oasp_ai_host_api;

typedef struct oasp_ai_backend_api {
    uint32_t struct_size;
    int (*enumerate_devices)(void *worker, oasp_ai_device_info *devices, size_t *count, oasp_ai_error *error);
    int (*open_device)(void *worker, uint32_t ordinal, const oasp_ai_device_options *options, oasp_ai_device *device, oasp_ai_error *error);
    void (*close_device)(oasp_ai_device device);
    int (*query_capabilities)(oasp_ai_device device, oasp_ai_capability_query *query, oasp_ai_error *error);
    int (*alloc)(oasp_ai_device device, const oasp_ai_alloc_desc *desc, oasp_ai_backend_storage *storage, oasp_ai_error *error);
    void (*free)(oasp_ai_device device, oasp_ai_backend_storage storage);
    int (*copy)(oasp_ai_stream stream, const oasp_ai_copy_desc *desc, oasp_ai_event *event, oasp_ai_error *error);
    int (*create_stream)(oasp_ai_device device, const oasp_ai_stream_desc *desc, oasp_ai_stream *stream, oasp_ai_error *error);
    void (*destroy_stream)(oasp_ai_stream stream);
    int (*record_event)(oasp_ai_stream stream, oasp_ai_event *event, oasp_ai_error *error);
    int (*wait_event)(oasp_ai_stream stream, oasp_ai_event event, oasp_ai_error *error);
    int (*synchronize)(oasp_ai_stream stream, oasp_ai_error *error);
    int (*execute_op)(oasp_ai_stream stream, const oasp_ai_op_desc *op, oasp_ai_event *event, oasp_ai_error *error);
    int (*compile_graph)(oasp_ai_device device, const oasp_ai_graph_desc *graph, oasp_ai_executable *executable, oasp_ai_error *error);
    int (*execute_graph)(oasp_ai_stream stream, oasp_ai_executable executable, const oasp_ai_graph_bindings *bindings, oasp_ai_event *event, oasp_ai_error *error);
    void (*destroy_executable)(oasp_ai_executable executable);
    int (*memory_stats)(oasp_ai_device device, oasp_ai_memory_stats *stats, oasp_ai_error *error);
    int (*trim_memory)(oasp_ai_device device, uint64_t target_bytes, oasp_ai_error *error);
    int (*all_reduce)(const oasp_ai_stream *streams, oasp_ai_tensor_view *tensors, size_t count,
                      oasp_ai_collective_reduction reduction, oasp_ai_error *error);
} oasp_ai_backend_api;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_major_required;
    uint32_t abi_minor_required;
    const char *backend_name;
    const char *backend_version;
    uint64_t flags;
    int (*module_init)(const oasp_ai_host_api *host, void **module);
    void (*module_shutdown)(void *module);
    int (*worker_init)(void *module, void **worker, oasp_ai_error *error);
    void (*worker_shutdown)(void *worker);
    const oasp_ai_backend_api *api;
} oasp_ai_backend_descriptor;

#if defined(_WIN32)
#define OASP_AI_BACKEND_EXPORT __declspec(dllexport)
#else
#define OASP_AI_BACKEND_EXPORT __attribute__((visibility("default")))
#endif

typedef const oasp_ai_backend_descriptor *(*oasp_ai_backend_entry)(void);
#ifdef __cplusplus
extern "C" {
#endif
OASP_AI_BACKEND_EXPORT const oasp_ai_backend_descriptor *openasp_ai_backend(void);
#ifdef __cplusplus
}
#endif

#endif
