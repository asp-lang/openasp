// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_AI_H
#define OPENASP_AI_H

#include <openasp/ai_backend.h>

#include <stddef.h>
#include <stdint.h>

#define OASP_AI_VERSION_MAJOR 1u
#define OASP_AI_VERSION_MINOR 0u
#define OASP_AI_OPERATOR_SET_MAJOR 1u
#define OASP_AI_OPERATOR_SET_MINOR 0u

typedef struct oasp_ai_context oasp_ai_context;
typedef struct oasp_ai_tensor oasp_ai_tensor;
typedef struct oasp_ai_module oasp_ai_module;
typedef struct oasp_ai_optimizer oasp_ai_optimizer;
typedef struct oasp_ai_graph oasp_ai_graph;
typedef struct oasp_ai_plan oasp_ai_plan;
typedef struct oasp_ai_kv_cache oasp_ai_kv_cache;
typedef struct oasp_ai_dataset oasp_ai_dataset;
typedef struct oasp_ai_dataloader oasp_ai_dataloader;
typedef struct oasp_ai_data_iterator oasp_ai_data_iterator;
typedef struct oasp_ai_batch oasp_ai_batch;
typedef struct oasp_ai_tokenizer oasp_ai_tokenizer;
typedef struct oasp_ai_scheduler oasp_ai_scheduler;
typedef struct oasp_ai_loss_scaler oasp_ai_loss_scaler;
typedef struct oasp_ai_state_dict oasp_ai_state_dict;
typedef struct oasp_ai_event_handle oasp_ai_event_handle;

typedef enum {
    OASP_AI_OP_ZEROS = 1,
    OASP_AI_OP_ONES,
    OASP_AI_OP_FULL,
    OASP_AI_OP_ARANGE,
    OASP_AI_OP_RANDOM_UNIFORM,
    OASP_AI_OP_RANDOM_NORMAL,
    OASP_AI_OP_ADD,
    OASP_AI_OP_SUB,
    OASP_AI_OP_MUL,
    OASP_AI_OP_DIV,
    OASP_AI_OP_POW,
    OASP_AI_OP_EXP,
    OASP_AI_OP_LOG,
    OASP_AI_OP_SQRT,
    OASP_AI_OP_MAXIMUM,
    OASP_AI_OP_RELU,
    OASP_AI_OP_LEAKY_RELU,
    OASP_AI_OP_SIGMOID,
    OASP_AI_OP_TANH,
    OASP_AI_OP_GELU,
    OASP_AI_OP_SILU,
    OASP_AI_OP_RESHAPE,
    OASP_AI_OP_TRANSPOSE,
    OASP_AI_OP_SLICE,
    OASP_AI_OP_CONCAT,
    OASP_AI_OP_SPLIT,
    OASP_AI_OP_BROADCAST,
    OASP_AI_OP_EXPAND,
    OASP_AI_OP_SUM,
    OASP_AI_OP_MEAN,
    OASP_AI_OP_MAX,
    OASP_AI_OP_MIN,
    OASP_AI_OP_ARGMAX,
    OASP_AI_OP_NORM,
    OASP_AI_OP_MATMUL,
    OASP_AI_OP_BATCHED_MATMUL,
    OASP_AI_OP_LINEAR,
    OASP_AI_OP_CONV2D,
    OASP_AI_OP_POOL2D,
    OASP_AI_OP_BATCH_NORM,
    OASP_AI_OP_LAYER_NORM,
    OASP_AI_OP_RMS_NORM,
    OASP_AI_OP_DROPOUT,
    OASP_AI_OP_ATTENTION,
    OASP_AI_OP_CAUSAL_MASK,
    OASP_AI_OP_ROPE,
    OASP_AI_OP_ALIBI,
    OASP_AI_OP_MSE,
    OASP_AI_OP_CROSS_ENTROPY,
    OASP_AI_OP_NLL,
    OASP_AI_OP_WHERE,
    OASP_AI_OP_MASKED_FILL,
    OASP_AI_OP_GATHER,
    OASP_AI_OP_SCATTER,
    OASP_AI_OP_EMBEDDING,
    OASP_AI_OP_TOPK,
    OASP_AI_OP_MULTINOMIAL,
    OASP_AI_OP_TEMPERATURE,
    OASP_AI_OP_REPETITION_PENALTY,
    OASP_AI_OP_KV_CACHE_READ,
    OASP_AI_OP_KV_CACHE_APPEND,
    OASP_AI_OP_CAST,
    OASP_AI_OP_CONTIGUOUS,
    OASP_AI_OP_SOFTMAX,
    OASP_AI_OP_CTC_LOSS
} oasp_ai_operator;

typedef enum {
    OASP_AI_OPTIMIZER_SGD,
    OASP_AI_OPTIMIZER_ADAM,
    OASP_AI_OPTIMIZER_ADAMW
} oasp_ai_optimizer_kind;

typedef enum {
    OASP_AI_REDUCTION_NONE,
    OASP_AI_REDUCTION_MEAN,
    OASP_AI_REDUCTION_SUM,
    OASP_AI_REDUCTION_BATCH_MEAN
} oasp_ai_reduction;

typedef enum {
    OASP_AI_MODULE_GENERIC,
    OASP_AI_MODULE_SEQUENTIAL,
    OASP_AI_MODULE_LINEAR,
    OASP_AI_MODULE_RELU,
    OASP_AI_MODULE_DROPOUT,
    OASP_AI_MODULE_LAYER_NORM,
    OASP_AI_MODULE_RMS_NORM,
    OASP_AI_MODULE_EMBEDDING
} oasp_ai_module_kind;

typedef struct {
    uint64_t operator_calls;
    uint64_t operator_nanoseconds;
    uint64_t copied_bytes;
    uint64_t fallback_count;
    uint64_t layout_conversion_count;
    uint64_t graph_compile_count;
    uint64_t graph_compile_nanoseconds;
    uint64_t graph_cache_hits;
    uint64_t graph_cache_evictions;
    uint64_t queue_nanoseconds;
    uint64_t synchronization_count;
    uint64_t host_to_device_bytes;
    uint64_t device_to_host_bytes;
    uint64_t device_to_device_bytes;
    uint64_t live_bytes;
    uint64_t reserved_bytes;
    uint64_t peak_bytes;
    uint64_t dataloader_wait_nanoseconds;
    uint64_t forward_nanoseconds;
    uint64_t backward_nanoseconds;
    uint64_t optimizer_nanoseconds;
    uint64_t nan_inf_count;
    double loss_scale;
    double gradient_norm;
} oasp_ai_metrics;

typedef struct {
    oasp_ai_dtype dtype;
    int64_t shape[OASP_AI_MAX_RANK];
    int64_t strides[OASP_AI_MAX_RANK];
    uint32_t rank;
    uint32_t flags;
} oasp_ai_tensor_desc;

typedef struct {
    uint64_t seed;
    uint64_t maximum_tensor_bytes;
    uint64_t maximum_model_bytes;
    uint64_t maximum_array_elements;
    uint32_t maximum_plan_cache_entries;
    uint32_t maximum_trace_events;
    int deterministic;
    int strict_device;
    int training_enabled;
} oasp_ai_context_options;

typedef struct {
    uint64_t batch_size;
    uint64_t seed;
    uint32_t workers;
    uint32_t prefetch_batches;
    int shuffle;
    int drop_last;
    int pinned_memory;
} oasp_ai_dataloader_options;

typedef struct {
    int enabled;
    uint32_t bits;
    int32_t axis;
    double scale;
    int64_t zero_point;
    const double *channel_scales;
    size_t channel_count;
    const char *calibration_method;
} oasp_ai_quantization_info;

typedef enum {
    OASP_AI_SCHEDULER_STEP,
    OASP_AI_SCHEDULER_COSINE,
    OASP_AI_SCHEDULER_LINEAR_WARMUP
} oasp_ai_scheduler_kind;

typedef enum {
    OASP_AI_STATE_PARAMETER,
    OASP_AI_STATE_BUFFER
} oasp_ai_state_kind;

typedef struct {
    char **missing;
    size_t missing_count;
    char **unexpected;
    size_t unexpected_count;
    char **shape_mismatch;
    size_t shape_mismatch_count;
    char **dtype_mismatch;
    size_t dtype_mismatch_count;
} oasp_ai_state_diagnostics;

typedef struct {
    oasp_ai_scheduler *scheduler;
    oasp_ai_loss_scaler *loss_scaler;
    oasp_ai_data_iterator *data_iterator;
    const void *metadata;
    size_t metadata_size;
    uint64_t epoch;
    uint64_t global_step;
} oasp_ai_checkpoint_state;

typedef struct {
    const char *allowed_root;
    const char *expected_sha256;
    const char *signing_private_key_pem;
    const char *trusted_public_key_pem;
    int allow_unsigned;
} oasp_ai_bundle_security;

oasp_ai_context *oasp_ai_context_create(const oasp_ai_context_options *options, oasp_ai_error *error);
void oasp_ai_context_destroy(oasp_ai_context *context);
int oasp_ai_context_register_backend(oasp_ai_context *context, const oasp_ai_backend_descriptor *descriptor, oasp_ai_error *error);
int oasp_ai_context_load_backend(oasp_ai_context *context, const char *absolute_path, const char *expected_sha256, oasp_ai_error *error);
size_t oasp_ai_context_device_count(const oasp_ai_context *context);
int oasp_ai_context_device_info(const oasp_ai_context *context, size_t index, oasp_ai_device_info *info, oasp_ai_error *error);
const oasp_ai_metrics *oasp_ai_context_metrics(const oasp_ai_context *context);
int oasp_ai_context_synchronize(oasp_ai_context *context, oasp_ai_error *error);
int oasp_ai_context_profiler_start(oasp_ai_context *context, int detailed, oasp_ai_error *error);
int oasp_ai_context_profiler_stop(oasp_ai_context *context, oasp_ai_error *error);
int oasp_ai_context_profiler_export_chrome_trace(oasp_ai_context *context, const char *path, oasp_ai_error *error);

size_t oasp_ai_dtype_size(oasp_ai_dtype dtype);
const char *oasp_ai_dtype_name(oasp_ai_dtype dtype);
int oasp_ai_dtype_parse(const char *name, oasp_ai_dtype *dtype);

oasp_ai_tensor *oasp_ai_tensor_create(oasp_ai_context *context, const oasp_ai_tensor_desc *desc, const void *data, size_t data_size, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_full(oasp_ai_context *context, const int64_t *shape, uint32_t rank, oasp_ai_dtype dtype, double value,
                                    oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_random(oasp_ai_context *context, const int64_t *shape, uint32_t rank, oasp_ai_dtype dtype, double first,
                                      double second, int normal, oasp_ai_error *error);
void oasp_ai_tensor_retain(oasp_ai_tensor *tensor);
void oasp_ai_tensor_release(oasp_ai_tensor *tensor);
const oasp_ai_tensor_desc *oasp_ai_tensor_descriptor(const oasp_ai_tensor *tensor);
uint64_t oasp_ai_tensor_elements(const oasp_ai_tensor *tensor);
uint64_t oasp_ai_tensor_bytes(const oasp_ai_tensor *tensor);
void *oasp_ai_tensor_data(oasp_ai_tensor *tensor);
const void *oasp_ai_tensor_const_data(const oasp_ai_tensor *tensor);
int oasp_ai_tensor_requires_grad(oasp_ai_tensor *tensor, int enabled, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_gradient(const oasp_ai_tensor *tensor);
int oasp_ai_tensor_zero_grad(oasp_ai_tensor *tensor, oasp_ai_error *error);
int oasp_ai_tensor_backward(oasp_ai_tensor *tensor, oasp_ai_tensor *gradient, int retain_graph, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_detach(oasp_ai_tensor *tensor, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_clone(oasp_ai_tensor *tensor, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_contiguous(oasp_ai_tensor *tensor, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_reshape(oasp_ai_tensor *tensor, const int64_t *shape, uint32_t rank, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_transpose(oasp_ai_tensor *tensor, uint32_t first, uint32_t second, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_slice(oasp_ai_tensor *tensor, uint32_t axis, int64_t start, int64_t stop, int64_t step,
                                     oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_cast(oasp_ai_tensor *tensor, oasp_ai_dtype dtype, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_to_device(oasp_ai_tensor *tensor, const char *device, oasp_ai_error *error);
int oasp_ai_tensor_device(const oasp_ai_tensor *tensor, char *device, size_t capacity);
int oasp_ai_tensor_copy_to_host(const oasp_ai_tensor *tensor, void *data, size_t size, oasp_ai_error *error);
int oasp_ai_tensor_all_reduce(oasp_ai_tensor *const *tensors, size_t count, oasp_ai_collective_reduction reduction, oasp_ai_error *error);
int oasp_ai_tensor_quantization(const oasp_ai_tensor *tensor, oasp_ai_quantization_info *info);
oasp_ai_tensor *oasp_ai_tensor_quantize(oasp_ai_tensor *tensor, uint32_t bits, int32_t axis, const char *calibration_method, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_tensor_dequantize(oasp_ai_tensor *tensor, oasp_ai_dtype dtype, oasp_ai_error *error);

oasp_ai_tensor *oasp_ai_unary(oasp_ai_context *context, oasp_ai_operator operation, oasp_ai_tensor *input, double argument, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_binary(oasp_ai_context *context, oasp_ai_operator operation, oasp_ai_tensor *left, oasp_ai_tensor *right, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_reduce(oasp_ai_context *context, oasp_ai_operator operation, oasp_ai_tensor *input, int32_t axis, int keep_dimensions,
                               oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_matmul(oasp_ai_context *context, oasp_ai_tensor *left, oasp_ai_tensor *right, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_linear(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *weight, oasp_ai_tensor *bias, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_softmax(oasp_ai_context *context, oasp_ai_tensor *input, int32_t axis, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_layer_norm(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *weight, oasp_ai_tensor *bias, double epsilon,
                                   int rms_only, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_cross_entropy(oasp_ai_context *context, oasp_ai_tensor *logits, oasp_ai_tensor *labels, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_cross_entropy_ex(oasp_ai_context *context, oasp_ai_tensor *logits, oasp_ai_tensor *labels, oasp_ai_tensor *weight, int64_t ignore_index, double label_smoothing, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_mse_loss(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_l1_loss(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_bce_loss(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_bce_with_logits_loss(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_bce_with_logits_loss_ex(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target, oasp_ai_tensor *positive_weight, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_nll_loss(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_kl_div_loss(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target, oasp_ai_reduction reduction, int log_target, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_huber_loss(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target, double delta, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_smooth_l1_loss(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target, double beta, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_focal_loss(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target, double alpha, double gamma, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_dice_loss(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target, double smooth, double epsilon, int from_logits, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_info_nce_loss(oasp_ai_context *context, oasp_ai_tensor *query, oasp_ai_tensor *key, double temperature, int symmetric, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_ctc_loss(oasp_ai_context *context, oasp_ai_tensor *log_probabilities, oasp_ai_tensor *targets, oasp_ai_tensor *input_lengths, oasp_ai_tensor *target_lengths, int64_t blank, int zero_infinity, oasp_ai_reduction reduction, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_attention(oasp_ai_context *context, oasp_ai_tensor *query, oasp_ai_tensor *key, oasp_ai_tensor *value, oasp_ai_tensor *mask,
                                  int causal, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_concat(oasp_ai_context *context, oasp_ai_tensor *const *inputs, size_t input_count, uint32_t axis, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_where(oasp_ai_context *context, oasp_ai_tensor *condition, oasp_ai_tensor *when_true, oasp_ai_tensor *when_false, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_gather(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *indices, uint32_t axis, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_conv2d(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *weight, oasp_ai_tensor *bias, uint32_t stride_height,
                               uint32_t stride_width, uint32_t padding_height, uint32_t padding_width, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_pool2d(oasp_ai_context *context, oasp_ai_tensor *input, uint32_t kernel_height, uint32_t kernel_width, uint32_t stride_height,
                               uint32_t stride_width, int average, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_rope(oasp_ai_context *context, oasp_ai_tensor *input, uint64_t position_offset, double base, oasp_ai_error *error);

oasp_ai_module *oasp_ai_module_create(oasp_ai_context *context, const char *name, oasp_ai_error *error);
oasp_ai_module *oasp_ai_module_linear(oasp_ai_context *context, uint64_t input_features, uint64_t output_features, int bias, oasp_ai_error *error);
oasp_ai_module *oasp_ai_module_embedding(oasp_ai_context *context, uint64_t vocabulary, uint64_t dimensions, oasp_ai_error *error);
oasp_ai_module *oasp_ai_module_activation(oasp_ai_context *context, oasp_ai_module_kind kind, double argument, oasp_ai_error *error);
oasp_ai_module *oasp_ai_module_normalization(oasp_ai_context *context, uint64_t dimensions, double epsilon, int rms_only, oasp_ai_error *error);
void oasp_ai_module_retain(oasp_ai_module *module);
void oasp_ai_module_release(oasp_ai_module *module);
int oasp_ai_module_register_module(oasp_ai_module *module, const char *name, oasp_ai_module *child, oasp_ai_error *error);
int oasp_ai_module_register_parameter(oasp_ai_module *module, const char *name, oasp_ai_tensor *tensor, oasp_ai_error *error);
int oasp_ai_module_register_buffer(oasp_ai_module *module, const char *name, oasp_ai_tensor *tensor, int persistent, oasp_ai_error *error);
size_t oasp_ai_module_parameter_count(const oasp_ai_module *module);
oasp_ai_tensor *oasp_ai_module_parameter(const oasp_ai_module *module, size_t index, const char **name);
oasp_ai_state_dict *oasp_ai_module_state_dict(const oasp_ai_module *module, oasp_ai_error *error);
void oasp_ai_state_dict_release(oasp_ai_state_dict *state);
size_t oasp_ai_state_dict_count(const oasp_ai_state_dict *state);
oasp_ai_tensor *oasp_ai_state_dict_tensor(const oasp_ai_state_dict *state, size_t index, const char **name, oasp_ai_state_kind *kind);
int oasp_ai_module_load_state_dict(oasp_ai_module *module, const oasp_ai_state_dict *state, int strict, oasp_ai_state_diagnostics *diagnostics, oasp_ai_error *error);
void oasp_ai_state_diagnostics_free(oasp_ai_state_diagnostics *diagnostics);
int oasp_ai_module_to_dtype(oasp_ai_module *module, oasp_ai_dtype dtype, oasp_ai_error *error);
int oasp_ai_module_to_device(oasp_ai_module *module, const char *device, oasp_ai_error *error);
void oasp_ai_module_train(oasp_ai_module *module, int training);
int oasp_ai_module_add(oasp_ai_module *module, oasp_ai_module *child, oasp_ai_error *error);
oasp_ai_tensor *oasp_ai_module_forward(oasp_ai_module *module, oasp_ai_tensor *input, oasp_ai_error *error);
int oasp_ai_module_save_weights(oasp_ai_module *module, const char *path, oasp_ai_error *error);
int oasp_ai_module_load_weights(oasp_ai_module *module, const char *path, int strict, oasp_ai_error *error);
int oasp_ai_module_save_safetensors(oasp_ai_module *module, const char *path, oasp_ai_error *error);
int oasp_ai_module_load_safetensors(oasp_ai_module *module, const char *path, int strict, oasp_ai_state_diagnostics *diagnostics, oasp_ai_error *error);

oasp_ai_optimizer *oasp_ai_optimizer_create(oasp_ai_module *module, oasp_ai_optimizer_kind kind, double learning_rate, double weight_decay, oasp_ai_error *error);
void oasp_ai_optimizer_release(oasp_ai_optimizer *optimizer);
int oasp_ai_optimizer_zero_grad(oasp_ai_optimizer *optimizer, oasp_ai_error *error);
int oasp_ai_optimizer_step(oasp_ai_optimizer *optimizer, oasp_ai_error *error);
void oasp_ai_optimizer_set_momentum(oasp_ai_optimizer *optimizer, double momentum);
void oasp_ai_optimizer_set_betas(oasp_ai_optimizer *optimizer, double beta1, double beta2, double epsilon);
int oasp_ai_optimizer_clip_grad_norm(oasp_ai_optimizer *optimizer, double maximum_norm, oasp_ai_error *error);
double oasp_ai_optimizer_learning_rate(const oasp_ai_optimizer *optimizer);
void oasp_ai_optimizer_set_learning_rate(oasp_ai_optimizer *optimizer, double learning_rate);

oasp_ai_scheduler *oasp_ai_scheduler_create(oasp_ai_optimizer *optimizer, oasp_ai_scheduler_kind kind, uint64_t total_steps, uint64_t warmup_or_step_size, double factor_or_minimum,
                                            oasp_ai_error *error);
void oasp_ai_scheduler_release(oasp_ai_scheduler *scheduler);
int oasp_ai_scheduler_step(oasp_ai_scheduler *scheduler, oasp_ai_error *error);
uint64_t oasp_ai_scheduler_current_step(const oasp_ai_scheduler *scheduler);
oasp_ai_loss_scaler *oasp_ai_loss_scaler_create(double initial_scale, double growth_factor, double backoff_factor, uint32_t growth_interval, oasp_ai_error *error);
void oasp_ai_loss_scaler_release(oasp_ai_loss_scaler *scaler);
double oasp_ai_loss_scaler_scale(const oasp_ai_loss_scaler *scaler);
oasp_ai_tensor *oasp_ai_loss_scaler_scale_loss(oasp_ai_loss_scaler *scaler, oasp_ai_tensor *loss, oasp_ai_error *error);
int oasp_ai_loss_scaler_step(oasp_ai_loss_scaler *scaler, oasp_ai_optimizer *optimizer, oasp_ai_error *error);

oasp_ai_graph *oasp_ai_graph_create(oasp_ai_context *context, const char *name, oasp_ai_error *error);
void oasp_ai_graph_release(oasp_ai_graph *graph);
uint32_t oasp_ai_graph_input(oasp_ai_graph *graph, const char *name, const oasp_ai_tensor_desc *desc, oasp_ai_error *error);
uint32_t oasp_ai_graph_constant(oasp_ai_graph *graph, const char *name, oasp_ai_tensor *tensor, oasp_ai_error *error);
uint32_t oasp_ai_graph_operation(oasp_ai_graph *graph, oasp_ai_operator operation, const uint32_t *inputs, size_t input_count, const void *attributes,
                                 size_t attributes_size, oasp_ai_error *error);
int oasp_ai_graph_set_outputs(oasp_ai_graph *graph, const uint32_t *outputs, size_t output_count, oasp_ai_error *error);
int oasp_ai_graph_verify(oasp_ai_graph *graph, oasp_ai_error *error);
oasp_ai_plan *oasp_ai_graph_compile(oasp_ai_graph *graph, const char *device, oasp_ai_error *error);
int oasp_ai_graph_save(oasp_ai_graph *graph, const char *path, oasp_ai_error *error);
oasp_ai_graph *oasp_ai_graph_load(oasp_ai_context *context, const char *path, oasp_ai_error *error);

void oasp_ai_plan_release(oasp_ai_plan *plan);
int oasp_ai_plan_execute(oasp_ai_plan *plan, oasp_ai_tensor *const *inputs, size_t input_count, oasp_ai_tensor ***outputs, size_t *output_count,
                         oasp_ai_error *error);
oasp_ai_event_handle *oasp_ai_plan_execute_async(oasp_ai_plan *plan, oasp_ai_tensor *const *inputs, size_t input_count, oasp_ai_error *error);
void oasp_ai_event_retain(oasp_ai_event_handle *event);
void oasp_ai_event_release(oasp_ai_event_handle *event);
int oasp_ai_event_ready(oasp_ai_event_handle *event);
int oasp_ai_event_wait(oasp_ai_event_handle *event, oasp_ai_error *error);
int oasp_ai_event_outputs(oasp_ai_event_handle *event, oasp_ai_tensor ***outputs, size_t *output_count, oasp_ai_error *error);

oasp_ai_kv_cache *oasp_ai_kv_cache_create(oasp_ai_context *context, uint32_t layers, uint32_t batch, uint32_t heads, uint32_t head_dimension,
                                          uint32_t page_tokens, oasp_ai_dtype dtype, oasp_ai_error *error);
void oasp_ai_kv_cache_release(oasp_ai_kv_cache *cache);
int oasp_ai_kv_cache_append(oasp_ai_kv_cache *cache, uint32_t layer, oasp_ai_tensor *keys, oasp_ai_tensor *values, oasp_ai_error *error);
uint64_t oasp_ai_kv_cache_length(const oasp_ai_kv_cache *cache);
int oasp_ai_sample(oasp_ai_context *context, oasp_ai_tensor *logits, double temperature, uint32_t top_k, double top_p,
                   const int64_t *history, size_t history_count, double repetition_penalty, int64_t *token, oasp_ai_error *error);

oasp_ai_dataset *oasp_ai_dataset_from_tensors(oasp_ai_tensor *inputs, oasp_ai_tensor *labels, oasp_ai_error *error);
oasp_ai_dataset *oasp_ai_dataset_mnist(oasp_ai_context *context, const char *images_path, const char *labels_path, int normalize, oasp_ai_error *error);
void oasp_ai_dataset_retain(oasp_ai_dataset *dataset);
void oasp_ai_dataset_release(oasp_ai_dataset *dataset);
uint64_t oasp_ai_dataset_size(const oasp_ai_dataset *dataset);
oasp_ai_dataloader *oasp_ai_dataloader_create(oasp_ai_dataset *dataset, const oasp_ai_dataloader_options *options, oasp_ai_error *error);
void oasp_ai_dataloader_release(oasp_ai_dataloader *loader);
oasp_ai_data_iterator *oasp_ai_dataloader_iterator(oasp_ai_dataloader *loader, uint64_t epoch, oasp_ai_error *error);
void oasp_ai_data_iterator_release(oasp_ai_data_iterator *iterator);
int oasp_ai_data_iterator_next(oasp_ai_data_iterator *iterator, oasp_ai_batch **batch, oasp_ai_error *error);
uint64_t oasp_ai_data_iterator_cursor(const oasp_ai_data_iterator *iterator);
void oasp_ai_batch_retain(oasp_ai_batch *batch);
void oasp_ai_batch_release(oasp_ai_batch *batch);
oasp_ai_tensor *oasp_ai_batch_inputs(oasp_ai_batch *batch);
oasp_ai_tensor *oasp_ai_batch_labels(oasp_ai_batch *batch);
uint64_t oasp_ai_batch_size(const oasp_ai_batch *batch);

oasp_ai_tokenizer *oasp_ai_tokenizer_load(const char *vocabulary_path, int add_unknown_token, oasp_ai_error *error);
void oasp_ai_tokenizer_release(oasp_ai_tokenizer *tokenizer);
int oasp_ai_tokenizer_encode(oasp_ai_tokenizer *tokenizer, const void *text, size_t text_size, int64_t **tokens, size_t *token_count,
                             oasp_ai_error *error);
int oasp_ai_tokenizer_decode(oasp_ai_tokenizer *tokenizer, const int64_t *tokens, size_t token_count, char **text, size_t *text_size,
                             oasp_ai_error *error);

int oasp_ai_save_checkpoint(const char *path, oasp_ai_module *module, oasp_ai_optimizer *optimizer, const void *metadata, size_t metadata_size,
                            oasp_ai_error *error);
int oasp_ai_load_checkpoint(const char *path, oasp_ai_module *module, oasp_ai_optimizer *optimizer, void **metadata, size_t *metadata_size,
                            oasp_ai_error *error);
int oasp_ai_save_checkpoint_full(const char *path, oasp_ai_module *module, oasp_ai_optimizer *optimizer, const oasp_ai_checkpoint_state *state, oasp_ai_error *error);
int oasp_ai_load_checkpoint_full(const char *path, oasp_ai_module *module, oasp_ai_optimizer *optimizer, oasp_ai_checkpoint_state *state, int strict,
                                 oasp_ai_error *error);
void oasp_ai_checkpoint_state_clear(oasp_ai_checkpoint_state *state);

int oasp_ai_save_bundle(const char *path, oasp_ai_module *module, const char *entry_class, const void *bytecode, size_t bytecode_size,
                        const void *configuration, size_t configuration_size, const void *dependency_manifest, size_t dependency_manifest_size, const oasp_ai_bundle_security *security, oasp_ai_error *error);
int oasp_ai_load_bundle(oasp_ai_context *context, const char *path, oasp_ai_module *module, char **entry_class, void **bytecode,
                        size_t *bytecode_size, void **configuration, size_t *configuration_size, void **dependency_manifest, size_t *dependency_manifest_size, const oasp_ai_bundle_security *security, oasp_ai_error *error);

int oasp_ai_graph_export_onnx(oasp_ai_graph *graph, const char *path, oasp_ai_error *error);
oasp_ai_graph *oasp_ai_graph_import_onnx(oasp_ai_context *context, const char *path, oasp_ai_error *error);

#endif
