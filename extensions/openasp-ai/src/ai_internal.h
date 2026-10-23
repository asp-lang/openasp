// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_AI_INTERNAL_H
#define OPENASP_AI_INTERNAL_H

#include <openasp/ai.h>

#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>

enum {
    OASP_AI_TENSOR_CONTIGUOUS = 1u << 0,
    OASP_AI_TENSOR_REQUIRES_GRAD = 1u << 1,
    OASP_AI_TENSOR_PARAMETER = 1u << 2
};

typedef struct oasp_ai_storage {
    atomic_uint references;
    oasp_ai_context *context;
    void *data;
    oasp_ai_backend_storage backend_storage;
    uint64_t bytes;
    uint32_t memory_kind;
    int32_t backend_index;
    uint64_t version;
} oasp_ai_storage;

typedef struct oasp_ai_autograd_node {
    atomic_uint references;
    oasp_ai_operator operation;
    oasp_ai_tensor **inputs;
    uint64_t *versions;
    size_t input_count;
    double argument;
    int32_t axis;
    int64_t parameters[4];
} oasp_ai_autograd_node;

struct oasp_ai_tensor {
    atomic_uint references;
    oasp_ai_context *context;
    oasp_ai_storage *storage;
    oasp_ai_tensor_desc desc;
    uint64_t offset;
    uint64_t element_count;
    uint64_t version;
    oasp_ai_autograd_node *grad_fn;
    oasp_ai_tensor *grad;
    uint32_t quantized_bits;
    int32_t quantized_axis;
    double quantized_scale;
    int64_t quantized_zero_point;
    double *quantized_channel_scales;
    size_t quantized_channel_count;
    char quantized_calibration[32];
};

typedef struct {
    char *name;
    oasp_ai_tensor *tensor;
    int persistent;
} oasp_ai_named_tensor;

typedef struct {
    char *name;
    oasp_ai_module *module;
} oasp_ai_named_module;

typedef struct {
    char *name;
    oasp_ai_tensor *tensor;
    oasp_ai_state_kind kind;
} oasp_ai_state_entry;

struct oasp_ai_state_dict {
    atomic_uint references;
    oasp_ai_state_entry *entries;
    size_t count;
};

struct oasp_ai_module {
    atomic_uint references;
    oasp_ai_context *context;
    char *name;
    oasp_ai_named_tensor *parameters;
    size_t parameter_count;
    size_t parameter_capacity;
    oasp_ai_named_tensor *buffers;
    size_t buffer_count;
    size_t buffer_capacity;
    oasp_ai_named_module *children;
    size_t child_count;
    size_t child_capacity;
    int training;
    oasp_ai_module_kind kind;
    double argument;
    uint64_t feature_size;
};

struct oasp_ai_optimizer {
    atomic_uint references;
    oasp_ai_module *module;
    oasp_ai_optimizer_kind kind;
    double learning_rate;
    double weight_decay;
    double momentum;
    double beta1;
    double beta2;
    double epsilon;
    uint64_t step;
    oasp_ai_tensor **first_moment;
    oasp_ai_tensor **second_moment;
    size_t parameter_count;
};

struct oasp_ai_scheduler {
    oasp_ai_optimizer *optimizer;
    oasp_ai_scheduler_kind kind;
    uint64_t total_steps;
    uint64_t warmup_or_step_size;
    uint64_t step;
    double initial_learning_rate;
    double factor_or_minimum;
};

struct oasp_ai_loss_scaler {
    double scale;
    double growth_factor;
    double backoff_factor;
    uint32_t growth_interval;
    uint32_t finite_steps;
};

typedef struct {
    uint32_t id;
    char *name;
    oasp_ai_tensor_desc desc;
    oasp_ai_tensor *constant;
    uint32_t producer;
} oasp_ai_ir_value;

typedef struct {
    uint32_t op_id;
    uint16_t op_version;
    uint16_t flags;
    uint32_t *inputs;
    size_t input_count;
    uint32_t output;
    void *attributes;
    size_t attributes_size;
} oasp_ai_ir_operation;

struct oasp_ai_graph {
    atomic_uint references;
    oasp_ai_context *context;
    char *name;
    oasp_ai_ir_value *values;
    size_t value_count;
    size_t value_capacity;
    oasp_ai_ir_operation *operations;
    size_t operation_count;
    size_t operation_capacity;
    uint32_t *inputs;
    size_t input_count;
    size_t input_capacity;
    uint32_t *outputs;
    size_t output_count;
};

typedef struct {
    uint32_t op_id;
    uint16_t op_version;
    uint16_t flags;
    uint32_t *input_slots;
    size_t input_count;
    uint32_t output_slot;
    void *attributes;
    size_t attributes_size;
} oasp_ai_exec_step;

struct oasp_ai_plan {
    atomic_uint references;
    oasp_ai_graph *graph;
    oasp_ai_exec_step *steps;
    size_t step_count;
    uint32_t slot_count;
    uint32_t *value_slots;
    uint64_t cache_key;
    char *device;
    int32_t backend_index;
    oasp_ai_executable executable;
    void *backend_ir;
    size_t backend_ir_size;
};

typedef struct {
    uint64_t key;
    uint64_t last_used;
    oasp_ai_plan *plan;
} oasp_ai_plan_cache_entry;

typedef struct {
    char name[48];
    char category[24];
    uint64_t started;
    uint64_t duration;
    uint64_t thread_id;
} oasp_ai_trace_event;

typedef struct {
    const oasp_ai_backend_descriptor *descriptor;
    void *module;
    void *worker;
    void *dynamic_library;
    uint32_t ordinal;
    int owns_runtime;
    oasp_ai_device device;
    oasp_ai_stream compute_stream;
    oasp_ai_stream transfer_stream;
} oasp_ai_registered_backend;

struct oasp_ai_context {
    oasp_ai_context_options options;
    oasp_ai_metrics metrics;
    uint64_t rng_counter;
    oasp_ai_registered_backend *backends;
    size_t backend_count;
    size_t backend_capacity;
    pthread_mutex_t mutex;
    int mutex_initialized;
    oasp_ai_plan_cache_entry *plan_cache;
    size_t plan_cache_count;
    uint64_t plan_cache_clock;
    oasp_ai_trace_event *trace_events;
    size_t trace_event_count;
    int profiler_enabled;
    int profiler_detailed;
};

struct oasp_ai_event_handle {
    atomic_uint references;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    oasp_ai_plan *plan;
    oasp_ai_tensor **inputs;
    size_t input_count;
    oasp_ai_tensor **outputs;
    size_t output_count;
    oasp_ai_error error;
    int joined;
    int completed;
    int status;
};

typedef struct {
    oasp_ai_tensor *keys;
    oasp_ai_tensor *values;
} oasp_ai_kv_layer;

struct oasp_ai_kv_cache {
    atomic_uint references;
    oasp_ai_context *context;
    oasp_ai_kv_layer *layers;
    uint32_t layer_count;
    uint32_t batch;
    uint32_t heads;
    uint32_t head_dimension;
    uint32_t page_tokens;
    oasp_ai_dtype dtype;
    uint64_t length;
};

struct oasp_ai_dataset {
    atomic_uint references;
    oasp_ai_context *context;
    oasp_ai_tensor_desc input_desc;
    oasp_ai_tensor_desc label_desc;
    unsigned char *inputs;
    unsigned char *labels;
    size_t input_sample_bytes;
    size_t label_sample_bytes;
    uint64_t sample_count;
};

struct oasp_ai_dataloader {
    atomic_uint references;
    oasp_ai_dataset *dataset;
    oasp_ai_dataloader_options options;
};

typedef struct oasp_ai_prefetched_batch {
    unsigned char *inputs;
    unsigned char *labels;
    uint64_t start;
    uint64_t count;
    struct oasp_ai_prefetched_batch *next;
} oasp_ai_prefetched_batch;

struct oasp_ai_data_iterator {
    oasp_ai_dataloader *loader;
    uint64_t *order;
    uint64_t cursor;
    uint64_t produced_cursor;
    uint64_t epoch;
    pthread_t *workers;
    uint32_t worker_count;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_cond_t space;
    oasp_ai_prefetched_batch *head;
    oasp_ai_prefetched_batch *tail;
    uint32_t queued;
    uint32_t live_workers;
    int mutex_initialized;
    int ready_initialized;
    int space_initialized;
    int stopping;
    int finished;
    int failed;
};

struct oasp_ai_batch {
    atomic_uint references;
    oasp_ai_tensor *inputs;
    oasp_ai_tensor *labels;
    uint64_t size;
};

struct oasp_ai_tokenizer {
    char **tokens;
    size_t *lengths;
    size_t count;
    int64_t unknown_token;
};

void oasp_ai_set_error(oasp_ai_error *error, int code, const char *operation, const char *message, const char *suggestion);
int oasp_ai_checked_elements(const int64_t *shape, uint32_t rank, uint64_t *elements, oasp_ai_error *error);
char *oasp_ai_string_duplicate(const char *value);
void *oasp_ai_grow_array(void *items, size_t element_size, size_t *capacity, size_t required);
uint64_t oasp_ai_now_nanoseconds(void);
double oasp_ai_tensor_read(const oasp_ai_tensor *tensor, uint64_t logical_index);
void oasp_ai_tensor_write(oasp_ai_tensor *tensor, uint64_t logical_index, double value);
oasp_ai_tensor *oasp_ai_tensor_like(oasp_ai_tensor *tensor, int zero, oasp_ai_error *error);
int oasp_ai_accumulate_gradient(oasp_ai_tensor *tensor, oasp_ai_tensor *gradient, oasp_ai_error *error);
int oasp_ai_ctc_loss_backward(oasp_ai_tensor *output, oasp_ai_tensor *upstream, oasp_ai_error *error);
int oasp_ai_attach_grad(oasp_ai_tensor *output, oasp_ai_operator operation, oasp_ai_tensor *const *inputs, size_t input_count, double argument,
                        int32_t axis, oasp_ai_error *error);
oasp_ai_ir_value *oasp_ai_graph_add_value_internal(oasp_ai_graph *graph, const char *name, const oasp_ai_tensor_desc *desc, oasp_ai_error *error);
int oasp_ai_write_atomic(const char *path, int (*writer)(FILE *file, void *state, oasp_ai_error *error), void *state, oasp_ai_error *error);
uint32_t oasp_ai_crc32(const void *data, size_t size);
int oasp_ai_sha256(const void *data, size_t size, unsigned char digest[32]);
uint64_t oasp_ai_module_schema_hash(const oasp_ai_module *module);
void oasp_ai_trace_record(oasp_ai_context *context, const char *name, const char *category, uint64_t started, uint64_t duration);
int oasp_ai_backend_ir_build(oasp_ai_graph *graph, void **data, size_t *size, oasp_ai_error *error);
int oasp_ai_backend_find(oasp_ai_context *context, const char *device, oasp_ai_error *error);
int oasp_ai_tensor_backend_index(const oasp_ai_tensor *tensor);
int oasp_ai_tensor_same_device(oasp_ai_tensor *const *tensors, size_t count, int32_t *backend_index, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_create_on_backend(oasp_ai_context *context, const oasp_ai_tensor_desc *desc, int32_t backend_index, oasp_ai_error *error);
int oasp_ai_tensor_view_build(const oasp_ai_tensor *tensor, oasp_ai_tensor_view *view);
int oasp_ai_backend_execute(oasp_ai_context *context, int32_t backend_index, uint32_t operation, oasp_ai_tensor *const *inputs, size_t input_count,
                            oasp_ai_tensor *const *outputs, size_t output_count, const void *attributes, size_t attributes_size, oasp_ai_error *error);

#endif
