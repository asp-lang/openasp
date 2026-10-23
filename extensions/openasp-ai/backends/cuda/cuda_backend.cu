// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include <openasp/ai.h>

#include <cublasLt.h>
#include <cublas_v2.h>
#include <cub/cub.cuh>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#if defined(OASP_AI_WITH_CUDNN)
#include <cudnn_v9.h>
#endif
#if defined(OASP_AI_WITH_NCCL)
#include <nccl.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

struct cuda_storage {
    void *pointer;
    uint64_t bytes;
    cudaEvent_t ready;
};

struct cuda_device_state {
    int ordinal;
    cudaStream_t allocation_stream;
    uint64_t memory_limit;
    uint64_t live_bytes;
    uint64_t reserved_bytes;
    uint64_t peak_bytes;
    uint64_t allocations;
    uint64_t workspace_bytes;
    std::mutex mutex;
    std::vector<cuda_storage *> cached;
};

struct cublas_lt_plan {
    uint64_t rows;
    uint64_t shared;
    uint64_t columns;
    uint64_t batches;
    uint64_t left_stride;
    uint64_t right_stride;
    cudaDataType_t input_type;
    cublasLtMatmulDesc_t operation;
    cublasLtMatrixLayout_t right_layout;
    cublasLtMatrixLayout_t left_layout;
    cublasLtMatrixLayout_t output_layout;
    cublasLtMatmulAlgo_t algorithm;
    uint64_t last_used;
};

#if defined(OASP_AI_WITH_CUDNN)
struct cuda_cudnn_state;
#endif

struct cuda_stream_state {
    cuda_device_state *device;
    cudaStream_t stream;
    cublasHandle_t cublas;
    cublasLtHandle_t cublas_lt;
    void *cublas_workspace;
    size_t cublas_workspace_bytes;
    __half *fp16_workspace;
    size_t fp16_workspace_bytes;
    std::vector<cublas_lt_plan *> cublas_lt_plans;
    uint64_t cublas_lt_clock;
#if defined(OASP_AI_WITH_CUDNN)
    cuda_cudnn_state *cudnn;
#endif
    bool fp16_tensor_cores;
    bool capturing;
};

#if defined(OASP_AI_WITH_CUDNN)
#include "cuda_cudnn_graph.cuh"
#endif

struct graph_value {
    oasp_ai_tensor_view view;
    std::vector<unsigned char> constant;
    oasp_ai_backend_storage constant_storage;
};

struct graph_operation {
    uint32_t op_id;
    uint16_t op_version;
    uint16_t flags;
    uint32_t output;
    std::vector<uint32_t> inputs;
    std::vector<unsigned char> attributes;
};

struct cuda_executable_state {
    cuda_device_state *device;
    std::vector<graph_value> values;
    std::vector<graph_operation> operations;
    std::vector<uint32_t> inputs;
    std::vector<uint32_t> outputs;
    std::vector<oasp_ai_backend_storage> intermediates;
    std::vector<oasp_ai_backend_storage> captured_inputs;
    std::vector<oasp_ai_backend_storage> captured_outputs;
    cudaGraph_t cuda_graph;
    cudaGraphExec_t cuda_graph_exec;
};

static int fail(oasp_ai_error *error, int code, const char *operation, const char *message, const char *suggestion) {
    if (error)
        *error = {sizeof(*error), code, 0, operation, message, suggestion};
    return -1;
}

static int cuda_fail(oasp_ai_error *error, const char *operation, cudaError_t status) {
    if (error)
        *error = {sizeof(*error), 5, static_cast<int32_t>(status), operation, cudaGetErrorString(status),
                  "Check the CUDA driver, device memory limit, and kernel arguments."};
    return -1;
}

static int cublas_fail(oasp_ai_error *error, const char *operation, cublasStatus_t status) {
    if (error)
        *error = {sizeof(*error), 5, static_cast<int32_t>(status), operation, "cuBLAS operation failed",
                  "Check tensor shapes, dtype, CUDA context, and available workspace."};
    return -1;
}

static bool environment_enabled(const char *name) {
    const char *value = std::getenv(name);
    return value && (!std::strcmp(value, "1") || !std::strcmp(value, "on") ||
                     !std::strcmp(value, "true") || !std::strcmp(value, "yes"));
}

#if defined(OASP_AI_WITH_CUDNN)
static bool environment_disabled(const char *name) {
    const char *value = std::getenv(name);
    return value && (!std::strcmp(value, "0") || !std::strcmp(value, "off") ||
                     !std::strcmp(value, "false") || !std::strcmp(value, "no"));
}
#endif

static size_t environment_megabytes(const char *name, size_t fallback) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        return fallback;
    char *end = nullptr;
    unsigned long long megabytes = std::strtoull(value, &end, 10);
    if (!end || *end || !megabytes || megabytes > 65536)
        return fallback;
    return static_cast<size_t>(megabytes) * 1024 * 1024;
}

static void destroy_cublas_lt_plan(cublas_lt_plan *plan) {
    if (!plan)
        return;
    if (plan->output_layout)
        cublasLtMatrixLayoutDestroy(plan->output_layout);
    if (plan->left_layout)
        cublasLtMatrixLayoutDestroy(plan->left_layout);
    if (plan->right_layout)
        cublasLtMatrixLayoutDestroy(plan->right_layout);
    if (plan->operation)
        cublasLtMatmulDescDestroy(plan->operation);
    delete plan;
}

static int mark_storage_used(cuda_storage *storage, cudaStream_t stream, oasp_ai_error *error) {
    if (!storage)
        return 0;
    cudaError_t status = cudaSuccess;
    if (!storage->ready)
        status = cudaEventCreateWithFlags(&storage->ready, cudaEventDisableTiming);
    if (status == cudaSuccess)
        status = cudaEventRecord(storage->ready, stream);
    return status == cudaSuccess ? 0 : cuda_fail(error, "CudaStorageEvent", status);
}

static int wait_storage_ready(cuda_storage *storage, cudaStream_t stream, oasp_ai_error *error) {
    if (!storage || !storage->ready)
        return 0;
    cudaError_t status = cudaStreamWaitEvent(stream, storage->ready, 0);
    return status == cudaSuccess ? 0 : cuda_fail(error, "CudaStorageWait", status);
}

__host__ __device__ static cuda_storage *storage_from(oasp_ai_backend_storage handle) {
    return reinterpret_cast<cuda_storage *>(handle);
}

__host__ __device__ static void *view_pointer(const oasp_ai_tensor_view &view) {
    return reinterpret_cast<unsigned char *>(view.storage) + view.byte_offset;
}

static uint32_t dtype_bytes(oasp_ai_dtype dtype) {
    switch (dtype) {
    case OASP_AI_DTYPE_BOOL:
    case OASP_AI_DTYPE_U8:
    case OASP_AI_DTYPE_I8:
        return 1;
    case OASP_AI_DTYPE_I16:
    case OASP_AI_DTYPE_F16:
    case OASP_AI_DTYPE_BF16:
        return 2;
    case OASP_AI_DTYPE_I32:
    case OASP_AI_DTYPE_F32:
        return 4;
    case OASP_AI_DTYPE_I64:
    case OASP_AI_DTYPE_F64:
        return 8;
    }
    return 0;
}

static int device_alloc(cuda_device_state *device, uint64_t bytes, oasp_ai_backend_storage *output, oasp_ai_error *error) {
    std::lock_guard<std::mutex> lock(device->mutex);
    if (device->memory_limit && bytes > device->memory_limit - std::min(device->live_bytes, device->memory_limit))
        return fail(error, 7, "CudaAlloc", "CUDA memory budget exceeded", "Release tensors or increase the configured device memory limit.");
    auto selected = device->cached.end();
    for (auto iterator = device->cached.begin(); iterator != device->cached.end(); ++iterator)
        if ((!(*iterator)->ready || cudaEventQuery((*iterator)->ready) == cudaSuccess) &&
            (*iterator)->bytes >= bytes && (selected == device->cached.end() || (*iterator)->bytes < (*selected)->bytes))
            selected = iterator;
    cuda_storage *storage = nullptr;
    if (selected != device->cached.end()) {
        storage = *selected;
        device->cached.erase(selected);
    } else {
        storage = new (std::nothrow) cuda_storage{};
        if (!storage)
            return fail(error, 7, "CudaAlloc", "unable to allocate CUDA storage metadata", "Reduce host memory pressure.");
        cudaError_t status = cudaMalloc(&storage->pointer, static_cast<size_t>(bytes));
        if (status != cudaSuccess) {
            delete storage;
            return cuda_fail(error, "CudaAlloc", status);
        }
        storage->bytes = bytes;
        device->reserved_bytes += bytes;
        device->allocations++;
    }
    cudaError_t status = cudaMemsetAsync(storage->pointer, 0, static_cast<size_t>(bytes), device->allocation_stream);
    if (status == cudaSuccess && mark_storage_used(storage, device->allocation_stream, error)) {
        device->cached.push_back(storage);
        return -1;
    }
    if (status != cudaSuccess) {
        device->cached.push_back(storage);
        return cuda_fail(error, "CudaMemset", status);
    }
    device->live_bytes += storage->bytes;
    device->peak_bytes = std::max(device->peak_bytes, device->live_bytes);
    *output = reinterpret_cast<oasp_ai_backend_storage>(storage);
    return 0;
}

static void device_release(cuda_device_state *device, oasp_ai_backend_storage handle) {
    cuda_storage *storage = storage_from(handle);
    if (!device || !storage)
        return;
    std::lock_guard<std::mutex> lock(device->mutex);
    device->live_bytes -= std::min(device->live_bytes, storage->bytes);
    device->cached.push_back(storage);
}

__device__ static uint64_t physical_index(const oasp_ai_tensor_view &view, uint64_t logical) {
    if (view.flags & OASP_AI_TENSOR_VIEW_CONTIGUOUS)
        return logical;
    uint64_t physical = 0;
    for (uint32_t axis = view.rank; axis; axis--) {
        uint64_t dimension = static_cast<uint64_t>(view.shape[axis - 1]);
        physical += (logical % dimension) * static_cast<uint64_t>(view.strides[axis - 1]);
        logical /= dimension;
    }
    return physical;
}

__device__ static uint64_t broadcast_index(const oasp_ai_tensor_view &input, const oasp_ai_tensor_view &output, uint64_t logical) {
    uint64_t index = 0;
    uint64_t multiplier = 1;
    for (uint32_t reverse = 0; reverse < input.rank; reverse++) {
        uint32_t input_axis = input.rank - reverse - 1;
        uint32_t output_axis = output.rank - reverse - 1;
        uint64_t coordinate = logical % static_cast<uint64_t>(output.shape[output_axis]);
        logical /= static_cast<uint64_t>(output.shape[output_axis]);
        if (input.shape[input_axis] != 1)
            index += coordinate * multiplier;
        multiplier *= static_cast<uint64_t>(input.shape[input_axis]);
    }
    return index;
}

__global__ static void contiguous_kernel(oasp_ai_tensor_view input, oasp_ai_tensor_view output, uint32_t element_bytes) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= output.element_count)
        return;
    const unsigned char *source = static_cast<const unsigned char *>(view_pointer(input)) +
                                  physical_index(input, index) * element_bytes;
    unsigned char *destination = static_cast<unsigned char *>(view_pointer(output)) +
                                 physical_index(output, index) * element_bytes;
    for (uint32_t byte = 0; byte < element_bytes; byte++)
        destination[byte] = source[byte];
}

__global__ static void binary_kernel(oasp_ai_tensor_view left, oasp_ai_tensor_view right, oasp_ai_tensor_view output, uint32_t operation) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= output.element_count)
        return;
    const float *left_data = static_cast<const float *>(view_pointer(left));
    const float *right_data = static_cast<const float *>(view_pointer(right));
    float *output_data = static_cast<float *>(view_pointer(output));
    float first = left_data[physical_index(left, broadcast_index(left, output, index))];
    float second = right_data[physical_index(right, broadcast_index(right, output, index))];
    float value = 0;
    switch (operation) {
    case OASP_AI_OP_ADD: value = first + second; break;
    case OASP_AI_OP_SUB: value = first - second; break;
    case OASP_AI_OP_MUL: value = first * second; break;
    case OASP_AI_OP_DIV: value = first / second; break;
    case OASP_AI_OP_POW: value = powf(first, second); break;
    case OASP_AI_OP_MAXIMUM: value = fmaxf(first, second); break;
    }
    output_data[physical_index(output, index)] = value;
}

__global__ static void unary_kernel(oasp_ai_tensor_view input, oasp_ai_tensor_view output, uint32_t operation, double argument) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= output.element_count)
        return;
    const float *input_data = static_cast<const float *>(view_pointer(input));
    float *output_data = static_cast<float *>(view_pointer(output));
    float value = input_data[physical_index(input, index)];
    switch (operation) {
    case OASP_AI_OP_EXP: value = expf(value); break;
    case OASP_AI_OP_LOG: value = logf(value); break;
    case OASP_AI_OP_SQRT: value = sqrtf(value); break;
    case OASP_AI_OP_RELU: value = fmaxf(value, 0.0f); break;
    case OASP_AI_OP_LEAKY_RELU: value = value > 0 ? value : static_cast<float>(argument) * value; break;
    case OASP_AI_OP_SIGMOID: value = 1.0f / (1.0f + expf(-value)); break;
    case OASP_AI_OP_TANH: value = tanhf(value); break;
    case OASP_AI_OP_GELU:
        value = 0.5f * value * (1.0f + tanhf(0.7978845608f * (value + 0.044715f * value * value * value)));
        break;
    case OASP_AI_OP_SILU: value = value / (1.0f + expf(-value)); break;
    case OASP_AI_OP_TEMPERATURE: value /= static_cast<float>(argument); break;
    }
    output_data[physical_index(output, index)] = value;
}

__global__ static void reduce_kernel(oasp_ai_tensor_view input, oasp_ai_tensor_view output, uint32_t operation, int32_t axis) {
    uint64_t output_index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (output_index >= output.element_count)
        return;
    const float *source = static_cast<const float *>(view_pointer(input));
    float *destination = static_cast<float *>(view_pointer(output));
    uint64_t reduction = axis < 0 ? input.element_count : static_cast<uint64_t>(input.shape[axis]);
    uint64_t inner = 1;
    if (axis >= 0)
        for (uint32_t current = static_cast<uint32_t>(axis) + 1; current < input.rank; current++)
            inner *= static_cast<uint64_t>(input.shape[current]);
    float aggregate = operation == OASP_AI_OP_MIN ? INFINITY : operation == OASP_AI_OP_MAX ? -INFINITY : 0.0f;
    for (uint64_t item = 0; item < reduction; item++) {
        uint64_t index = axis < 0 ? item : (output_index / inner) * reduction * inner + item * inner + output_index % inner;
        float value = source[physical_index(input, index)];
        if (operation == OASP_AI_OP_MAX)
            aggregate = fmaxf(aggregate, value);
        else if (operation == OASP_AI_OP_MIN)
            aggregate = fminf(aggregate, value);
        else if (operation == OASP_AI_OP_NORM)
            aggregate += value * value;
        else
            aggregate += value;
    }
    if (operation == OASP_AI_OP_MEAN)
        aggregate /= static_cast<float>(reduction);
    else if (operation == OASP_AI_OP_NORM)
        aggregate = sqrtf(aggregate);
    destination[physical_index(output, output_index)] = aggregate;
}

__global__ static void matmul_kernel(oasp_ai_tensor_view left, oasp_ai_tensor_view right, oasp_ai_tensor_view output,
                                     uint64_t rows, uint64_t shared, uint64_t columns) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= output.element_count)
        return;
    uint64_t matrix_size = rows * columns;
    uint64_t batch = index / matrix_size;
    uint64_t matrix_index = index % matrix_size;
    uint64_t row = matrix_index / columns;
    uint64_t column = matrix_index % columns;
    uint64_t left_matrix = rows * shared;
    uint64_t right_matrix = shared * columns;
    uint64_t left_batch = left.element_count == left_matrix ? 0 : batch;
    uint64_t right_batch = right.element_count == right_matrix ? 0 : batch;
    const float *first = static_cast<const float *>(view_pointer(left));
    const float *second = static_cast<const float *>(view_pointer(right));
    float *destination = static_cast<float *>(view_pointer(output));
    float total = 0;
    for (uint64_t inner = 0; inner < shared; inner++)
        total += first[physical_index(left, left_batch * left_matrix + row * shared + inner)] *
                 second[physical_index(right, right_batch * right_matrix + inner * columns + column)];
    destination[physical_index(output, index)] = total;
}

__global__ static void softmax_kernel(oasp_ai_tensor_view input, oasp_ai_tensor_view output, uint64_t inner, uint64_t width) {
    uint64_t group_position = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    uint64_t groups = input.element_count / width;
    if (group_position >= groups)
        return;
    uint64_t group = group_position / inner;
    uint64_t position = group_position % inner;
    const float *source = static_cast<const float *>(view_pointer(input));
    float *destination = static_cast<float *>(view_pointer(output));
    float maximum = -INFINITY;
    for (uint64_t item = 0; item < width; item++)
        maximum = fmaxf(maximum, source[physical_index(input, group * width * inner + item * inner + position)]);
    float total = 0;
    for (uint64_t item = 0; item < width; item++)
        total += expf(source[physical_index(input, group * width * inner + item * inner + position)] - maximum);
    for (uint64_t item = 0; item < width; item++) {
        uint64_t index = group * width * inner + item * inner + position;
        destination[physical_index(output, index)] = expf(source[physical_index(input, index)] - maximum) / total;
    }
}

__global__ static void cross_entropy_kernel(oasp_ai_tensor_view logits, oasp_ai_tensor_view labels,
                                            oasp_ai_tensor_view weight, oasp_ai_tensor_view output,
                                            oasp_ai_tensor_view normalizer,
                                            oasp_ai_cross_entropy_attributes options) {
    if (blockIdx.x || threadIdx.x)
        return;
    const float *values = static_cast<const float *>(view_pointer(logits));
    const int64_t *targets = static_cast<const int64_t *>(view_pointer(labels));
    const float *weights = options.has_weight ? static_cast<const float *>(view_pointer(weight)) : nullptr;
    float *loss = static_cast<float *>(view_pointer(output));
    uint64_t batch = static_cast<uint64_t>(logits.shape[0]);
    uint64_t classes = static_cast<uint64_t>(logits.shape[1]);
    float total_loss = 0;
    float denominator = 0;
    for (uint64_t row = 0; row < batch; row++) {
        int64_t label = targets[physical_index(labels, row)];
        if (label == options.ignore_index) {
            if (options.reduction == OASP_AI_REDUCTION_NONE)
                loss[physical_index(output, row)] = 0;
            continue;
        }
        float maximum = -INFINITY;
        for (uint64_t column = 0; column < classes; column++)
            maximum = fmaxf(maximum, values[physical_index(logits, row * classes + column)]);
        float total = 0;
        for (uint64_t column = 0; column < classes; column++)
            total += expf(values[physical_index(logits, row * classes + column)] - maximum);
        float log_sum_exp = logf(total) + maximum;
        float target_weight = options.has_weight ? weights[static_cast<uint64_t>(label)] : 1.0f;
        float nll = (log_sum_exp -
                     values[physical_index(logits, row * classes + static_cast<uint64_t>(label))]) *
                    target_weight;
        float smooth = 0;
        for (uint64_t column = 0; column < classes; column++) {
            float class_weight = options.has_weight ? weights[column] : 1.0f;
            smooth += (log_sum_exp - values[physical_index(logits, row * classes + column)]) *
                      class_weight;
        }
        float row_loss = static_cast<float>(1.0 - options.label_smoothing) * nll +
                         static_cast<float>(options.label_smoothing) * smooth /
                             static_cast<float>(classes);
        if (options.reduction == OASP_AI_REDUCTION_NONE)
            loss[physical_index(output, row)] = row_loss;
        else
            total_loss += row_loss;
        denominator += target_weight;
    }
    if (options.reduction != OASP_AI_REDUCTION_NONE)
        loss[physical_index(output, 0)] =
            options.reduction == OASP_AI_REDUCTION_MEAN ? total_loss / denominator : total_loss;
    static_cast<float *>(view_pointer(normalizer))[physical_index(normalizer, 0)] =
        options.reduction == OASP_AI_REDUCTION_MEAN ? denominator : 1.0f;
}

__device__ static float ctc_log_add(float left, float right) {
    if (left == -INFINITY)
        return right;
    if (right == -INFINITY)
        return left;
    float maximum = fmaxf(left, right);
    return maximum + log1pf(expf(-fabsf(left - right)));
}

__global__ static void ctc_forward_kernel(oasp_ai_tensor_view log_probabilities,
                                          oasp_ai_tensor_view targets,
                                          oasp_ai_tensor_view input_lengths,
                                          oasp_ai_tensor_view target_lengths,
                                          oasp_ai_tensor_view alpha,
                                          oasp_ai_tensor_view per_sample,
                                          int64_t blank) {
    uint64_t batch = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    uint64_t batch_count = static_cast<uint64_t>(log_probabilities.shape[1]);
    if (batch >= batch_count)
        return;
    uint64_t maximum_time = static_cast<uint64_t>(log_probabilities.shape[0]);
    uint64_t classes = static_cast<uint64_t>(log_probabilities.shape[2]);
    uint64_t maximum_target_length = static_cast<uint64_t>(targets.shape[1]);
    uint64_t maximum_states = static_cast<uint64_t>(alpha.shape[2]);
    const float *log_values = static_cast<const float *>(view_pointer(log_probabilities));
    const int64_t *target_values = static_cast<const int64_t *>(view_pointer(targets));
    const int64_t *input_length_values = static_cast<const int64_t *>(view_pointer(input_lengths));
    const int64_t *target_length_values = static_cast<const int64_t *>(view_pointer(target_lengths));
    float *alpha_values = static_cast<float *>(view_pointer(alpha));
    float *sample_values = static_cast<float *>(view_pointer(per_sample));
    for (uint64_t index = 0; index < maximum_time * maximum_states; index++)
        alpha_values[physical_index(alpha, batch * maximum_time * maximum_states + index)] = -INFINITY;
    int64_t input_length = input_length_values[physical_index(input_lengths, batch)];
    int64_t target_length = target_length_values[physical_index(target_lengths, batch)];
    uint64_t repeats = 0;
    bool valid = input_length > 0 && input_length <= static_cast<int64_t>(maximum_time) &&
                 target_length >= 0 && target_length <= static_cast<int64_t>(maximum_target_length);
    for (int64_t position = 0; valid && position < target_length; position++) {
        int64_t label = target_values[physical_index(
            targets, batch * maximum_target_length + static_cast<uint64_t>(position))];
        valid = label >= 0 && label < static_cast<int64_t>(classes) && label != blank;
        if (position) {
            int64_t previous = target_values[physical_index(
                targets, batch * maximum_target_length + static_cast<uint64_t>(position - 1))];
            repeats += label == previous;
        }
    }
    valid = valid && static_cast<uint64_t>(input_length) >=
                         static_cast<uint64_t>(target_length) + repeats;
    float sample_loss = INFINITY;
    if (valid) {
        uint64_t states = static_cast<uint64_t>(target_length) * 2 + 1;
#define CTC_ALPHA_INDEX(t, s) ((batch * maximum_time + static_cast<uint64_t>(t)) * maximum_states + static_cast<uint64_t>(s))
#define CTC_LOGP_INDEX(t, c) ((static_cast<uint64_t>(t) * batch_count + batch) * classes + static_cast<uint64_t>(c))
        alpha_values[physical_index(alpha, CTC_ALPHA_INDEX(0, 0))] =
            log_values[physical_index(log_probabilities, CTC_LOGP_INDEX(0, blank))];
        if (target_length) {
            int64_t label = target_values[physical_index(targets, batch * maximum_target_length)];
            alpha_values[physical_index(alpha, CTC_ALPHA_INDEX(0, 1))] =
                log_values[physical_index(log_probabilities, CTC_LOGP_INDEX(0, label))];
        }
        for (int64_t time = 1; time < input_length; time++)
            for (uint64_t state = 0; state < states; state++) {
                int64_t label = state % 2
                                    ? target_values[physical_index(
                                          targets, batch * maximum_target_length + state / 2)]
                                    : blank;
                float value = alpha_values[physical_index(
                    alpha, CTC_ALPHA_INDEX(time - 1, state))];
                if (state)
                    value = ctc_log_add(
                        value, alpha_values[physical_index(
                                   alpha, CTC_ALPHA_INDEX(time - 1, state - 1))]);
                if (state > 1 && label != blank) {
                    int64_t previous = state > 2
                                           ? target_values[physical_index(
                                                 targets, batch * maximum_target_length +
                                                              state / 2 - 1)]
                                           : blank;
                    if (label != previous)
                        value = ctc_log_add(
                            value, alpha_values[physical_index(
                                       alpha, CTC_ALPHA_INDEX(time - 1, state - 2))]);
                }
                value += log_values[physical_index(
                    log_probabilities, CTC_LOGP_INDEX(time, label))];
                alpha_values[physical_index(alpha, CTC_ALPHA_INDEX(time, state))] = value;
            }
        float likelihood = alpha_values[physical_index(
            alpha, CTC_ALPHA_INDEX(input_length - 1, states - 1))];
        if (states > 1)
            likelihood = ctc_log_add(
                likelihood, alpha_values[physical_index(
                                alpha, CTC_ALPHA_INDEX(input_length - 1, states - 2))]);
        sample_loss = -likelihood;
#undef CTC_LOGP_INDEX
#undef CTC_ALPHA_INDEX
    }
    sample_values[physical_index(per_sample, batch)] = sample_loss;
}

__global__ static void ctc_reduce_kernel(oasp_ai_tensor_view target_lengths,
                                         oasp_ai_tensor_view per_sample,
                                         oasp_ai_tensor_view output,
                                         oasp_ai_ctc_attributes options) {
    if (blockIdx.x || threadIdx.x)
        return;
    uint64_t batch_count = per_sample.element_count;
    const int64_t *lengths = static_cast<const int64_t *>(view_pointer(target_lengths));
    const float *samples = static_cast<const float *>(view_pointer(per_sample));
    float *loss = static_cast<float *>(view_pointer(output));
    float aggregate = 0;
    for (uint64_t batch = 0; batch < batch_count; batch++) {
        float value = samples[physical_index(per_sample, batch)];
        float reduced = options.zero_infinity && !isfinite(value) ? 0 : value;
        if (options.reduction == OASP_AI_REDUCTION_NONE)
            loss[physical_index(output, batch)] = reduced;
        else if (options.reduction == OASP_AI_REDUCTION_MEAN) {
            int64_t length = lengths[physical_index(target_lengths, batch)];
            aggregate += reduced / static_cast<float>(length > 0 ? length : 1);
        } else
            aggregate += reduced;
    }
    if (options.reduction != OASP_AI_REDUCTION_NONE)
        loss[physical_index(output, 0)] =
            options.reduction == OASP_AI_REDUCTION_MEAN
                ? aggregate / static_cast<float>(batch_count)
                : aggregate;
}

__global__ static void ctc_backward_kernel(oasp_ai_tensor_view log_probabilities,
                                           oasp_ai_tensor_view targets,
                                           oasp_ai_tensor_view input_lengths,
                                           oasp_ai_tensor_view target_lengths,
                                           oasp_ai_tensor_view alpha,
                                           oasp_ai_tensor_view per_sample,
                                           oasp_ai_tensor_view upstream,
                                           oasp_ai_tensor_view gradient,
                                           oasp_ai_tensor_view beta,
                                           oasp_ai_ctc_attributes options) {
    uint64_t batch = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    uint64_t batch_count = static_cast<uint64_t>(log_probabilities.shape[1]);
    if (batch >= batch_count)
        return;
    uint64_t maximum_time = static_cast<uint64_t>(log_probabilities.shape[0]);
    uint64_t classes = static_cast<uint64_t>(log_probabilities.shape[2]);
    uint64_t maximum_target_length = static_cast<uint64_t>(targets.shape[1]);
    uint64_t maximum_states = static_cast<uint64_t>(alpha.shape[2]);
    const float *log_values = static_cast<const float *>(view_pointer(log_probabilities));
    const int64_t *target_values = static_cast<const int64_t *>(view_pointer(targets));
    const int64_t *input_length_values = static_cast<const int64_t *>(view_pointer(input_lengths));
    const int64_t *target_length_values = static_cast<const int64_t *>(view_pointer(target_lengths));
    const float *alpha_values = static_cast<const float *>(view_pointer(alpha));
    const float *sample_values = static_cast<const float *>(view_pointer(per_sample));
    const float *upstream_values = static_cast<const float *>(view_pointer(upstream));
    float *gradient_values = static_cast<float *>(view_pointer(gradient));
    float *beta_values = static_cast<float *>(view_pointer(beta));
    for (uint64_t time = 0; time < maximum_time; time++)
        for (uint64_t class_index = 0; class_index < classes; class_index++)
            gradient_values[physical_index(
                gradient, (time * batch_count + batch) * classes + class_index)] = 0;
    for (uint64_t index = 0; index < maximum_time * maximum_states; index++)
        beta_values[physical_index(beta, batch * maximum_time * maximum_states + index)] =
            -INFINITY;
    int64_t input_length = input_length_values[physical_index(input_lengths, batch)];
    int64_t target_length = target_length_values[physical_index(target_lengths, batch)];
    float sample_loss = sample_values[physical_index(per_sample, batch)];
    if (!isfinite(sample_loss) || input_length <= 0)
        return;
    uint64_t states = static_cast<uint64_t>(target_length) * 2 + 1;
#define CTC_BETA_INDEX(t, s) ((batch * maximum_time + static_cast<uint64_t>(t)) * maximum_states + static_cast<uint64_t>(s))
#define CTC_ALPHA_INDEX(t, s) ((batch * maximum_time + static_cast<uint64_t>(t)) * maximum_states + static_cast<uint64_t>(s))
#define CTC_LOGP_INDEX(t, c) ((static_cast<uint64_t>(t) * batch_count + batch) * classes + static_cast<uint64_t>(c))
    beta_values[physical_index(beta, CTC_BETA_INDEX(input_length - 1, states - 1))] = 0;
    if (states > 1)
        beta_values[physical_index(beta, CTC_BETA_INDEX(input_length - 1, states - 2))] = 0;
    for (int64_t time = input_length - 2; time >= 0; time--)
        for (uint64_t state = 0; state < states; state++) {
            float value = -INFINITY;
            for (uint64_t advance = 0; advance < 3; advance++) {
                uint64_t next = state + advance;
                if (next >= states)
                    continue;
                int64_t next_label = next % 2
                                         ? target_values[physical_index(
                                               targets, batch * maximum_target_length + next / 2)]
                                         : options.blank;
                int64_t current_label = state % 2
                                            ? target_values[physical_index(
                                                  targets, batch * maximum_target_length + state / 2)]
                                            : options.blank;
                if (advance == 2 &&
                    (next_label == options.blank || next_label == current_label))
                    continue;
                value = ctc_log_add(
                    value,
                    beta_values[physical_index(beta, CTC_BETA_INDEX(time + 1, next))] +
                        log_values[physical_index(
                            log_probabilities, CTC_LOGP_INDEX(time + 1, next_label))]);
            }
            beta_values[physical_index(beta, CTC_BETA_INDEX(time, state))] = value;
        }
    float scale = upstream_values[physical_index(
        upstream, options.reduction == OASP_AI_REDUCTION_NONE ? batch : 0)];
    if (options.reduction == OASP_AI_REDUCTION_MEAN)
        scale /= static_cast<float>(batch_count) *
                 static_cast<float>(target_length > 0 ? target_length : 1);
    for (int64_t time = 0; time < input_length; time++) {
        for (uint64_t class_index = 0; class_index < classes; class_index++)
            gradient_values[physical_index(
                gradient, CTC_LOGP_INDEX(time, class_index))] =
                expf(log_values[physical_index(
                    log_probabilities, CTC_LOGP_INDEX(time, class_index))]) *
                scale;
        for (uint64_t state = 0; state < states; state++) {
            int64_t label = state % 2
                                ? target_values[physical_index(
                                      targets, batch * maximum_target_length + state / 2)]
                                : options.blank;
            uint64_t index = CTC_LOGP_INDEX(time, label);
            float posterior = expf(
                alpha_values[physical_index(alpha, CTC_ALPHA_INDEX(time, state))] +
                beta_values[physical_index(beta, CTC_BETA_INDEX(time, state))] +
                sample_loss);
            gradient_values[physical_index(gradient, index)] -= posterior * scale;
        }
    }
#undef CTC_LOGP_INDEX
#undef CTC_ALPHA_INDEX
#undef CTC_BETA_INDEX
}

__global__ static void nll_kernel(oasp_ai_tensor_view input, oasp_ai_tensor_view target,
                                  oasp_ai_tensor_view output, uint32_t reduction) {
    uint64_t batch = static_cast<uint64_t>(input.shape[0]);
    uint64_t classes = static_cast<uint64_t>(input.shape[1]);
    const float *values = static_cast<const float *>(view_pointer(input));
    const int64_t *labels = static_cast<const int64_t *>(view_pointer(target));
    float *destination = static_cast<float *>(view_pointer(output));
    if (reduction == OASP_AI_REDUCTION_NONE) {
        uint64_t row = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
        if (row < batch)
            destination[physical_index(output, row)] =
                -values[physical_index(input, row * classes + static_cast<uint64_t>(labels[physical_index(target, row)]))];
        return;
    }
    if (blockIdx.x || threadIdx.x)
        return;
    float total = 0;
    for (uint64_t row = 0; row < batch; row++) {
        uint64_t label = static_cast<uint64_t>(labels[physical_index(target, row)]);
        total -= values[physical_index(input, row * classes + label)];
    }
    destination[physical_index(output, 0)] =
        reduction == OASP_AI_REDUCTION_MEAN ? total / static_cast<float>(batch) : total;
}

__global__ static void norm_kernel(oasp_ai_tensor_view input, oasp_ai_tensor_view weight, oasp_ai_tensor_view bias,
                                   oasp_ai_tensor_view output, double epsilon, int rms_only, int has_weight, int has_bias) {
    uint64_t row = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    uint64_t width = static_cast<uint64_t>(input.shape[input.rank - 1]);
    if (row >= input.element_count / width)
        return;
    const float *source = static_cast<const float *>(view_pointer(input));
    const float *scale = has_weight ? static_cast<const float *>(view_pointer(weight)) : nullptr;
    const float *shift = has_bias ? static_cast<const float *>(view_pointer(bias)) : nullptr;
    float *destination = static_cast<float *>(view_pointer(output));
    float mean = 0;
    if (!rms_only) {
        for (uint64_t column = 0; column < width; column++)
            mean += source[physical_index(input, row * width + column)];
        mean /= static_cast<float>(width);
    }
    float variance = 0;
    for (uint64_t column = 0; column < width; column++) {
        float centered = source[physical_index(input, row * width + column)] - (rms_only ? 0.0f : mean);
        variance += centered * centered;
    }
    float inverse = rsqrtf(variance / static_cast<float>(width) + static_cast<float>(epsilon));
    for (uint64_t column = 0; column < width; column++) {
        float value = (source[physical_index(input, row * width + column)] - (rms_only ? 0.0f : mean)) * inverse;
        if (has_weight)
            value *= scale[physical_index(weight, column)];
        if (has_bias)
            value += shift[physical_index(bias, column)];
        destination[physical_index(output, row * width + column)] = value;
    }
}

__global__ static void norm_stats_kernel(oasp_ai_tensor_view input, float *means, float *inverses,
                                         double epsilon, int rms_only) {
    uint64_t row = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    uint64_t width = static_cast<uint64_t>(input.shape[input.rank - 1]);
    uint64_t rows = input.element_count / width;
    if (row >= rows)
        return;
    const float *source = static_cast<const float *>(view_pointer(input));
    float mean = 0;
    if (!rms_only) {
        for (uint64_t column = 0; column < width; column++)
            mean += source[physical_index(input, row * width + column)];
        mean /= static_cast<float>(width);
    }
    float variance = 0;
    for (uint64_t column = 0; column < width; column++) {
        float centered = source[physical_index(input, row * width + column)] - (rms_only ? 0.0f : mean);
        variance += centered * centered;
    }
    means[row] = mean;
    inverses[row] = rsqrtf(variance / static_cast<float>(width) + static_cast<float>(epsilon));
}

__global__ static void norm_input_backward_kernel(oasp_ai_tensor_view input, oasp_ai_tensor_view weight,
                                                  oasp_ai_tensor_view upstream, const float *means,
                                                  const float *inverses, oasp_ai_tensor_view input_gradient,
                                                  int rms_only, int has_weight) {
    uint64_t row = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    uint64_t width = static_cast<uint64_t>(input.shape[input.rank - 1]);
    uint64_t rows = input.element_count / width;
    if (row >= rows)
        return;
    const float *source = static_cast<const float *>(view_pointer(input));
    const float *scale = has_weight ? static_cast<const float *>(view_pointer(weight)) : nullptr;
    const float *gradient = static_cast<const float *>(view_pointer(upstream));
    float *destination = static_cast<float *>(view_pointer(input_gradient));
    float sum_gradient = 0;
    float sum_gradient_normalized = 0;
    for (uint64_t column = 0; column < width; column++) {
        uint64_t index = row * width + column;
        float normalized = (source[physical_index(input, index)] - (rms_only ? 0.0f : means[row])) * inverses[row];
        float scaled = gradient[physical_index(upstream, index)] *
                       (has_weight ? scale[physical_index(weight, column)] : 1.0f);
        sum_gradient += scaled;
        sum_gradient_normalized += scaled * normalized;
    }
    for (uint64_t column = 0; column < width; column++) {
        uint64_t index = row * width + column;
        float normalized = (source[physical_index(input, index)] - (rms_only ? 0.0f : means[row])) * inverses[row];
        float scaled = gradient[physical_index(upstream, index)] *
                       (has_weight ? scale[physical_index(weight, column)] : 1.0f);
        float value = rms_only
                          ? inverses[row] * (scaled - normalized * sum_gradient_normalized / static_cast<float>(width))
                          : inverses[row] * (scaled - sum_gradient / static_cast<float>(width) -
                                            normalized * sum_gradient_normalized / static_cast<float>(width));
        destination[physical_index(input_gradient, index)] = value;
    }
}

__global__ static void norm_parameter_backward_kernel(oasp_ai_tensor_view input, oasp_ai_tensor_view upstream,
                                                      const float *means, const float *inverses,
                                                      oasp_ai_tensor_view weight_gradient,
                                                      oasp_ai_tensor_view bias_gradient,
                                                      int rms_only, int has_weight, int has_bias) {
    uint64_t column = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    uint64_t width = static_cast<uint64_t>(input.shape[input.rank - 1]);
    uint64_t rows = input.element_count / width;
    if (column >= width)
        return;
    const float *source = static_cast<const float *>(view_pointer(input));
    const float *gradient = static_cast<const float *>(view_pointer(upstream));
    float weight_sum = 0;
    float bias_sum = 0;
    for (uint64_t row = 0; row < rows; row++) {
        uint64_t index = row * width + column;
        float upstream_value = gradient[physical_index(upstream, index)];
        float normalized = (source[physical_index(input, index)] - (rms_only ? 0.0f : means[row])) * inverses[row];
        weight_sum += upstream_value * normalized;
        bias_sum += upstream_value;
    }
    if (has_weight)
        static_cast<float *>(view_pointer(weight_gradient))[physical_index(weight_gradient, column)] = weight_sum;
    if (has_bias)
        static_cast<float *>(view_pointer(bias_gradient))[physical_index(bias_gradient, column)] = bias_sum;
}

__global__ static void fill_kernel(oasp_ai_tensor_view output, float value) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index < output.element_count)
        static_cast<float *>(view_pointer(output))[physical_index(output, index)] = value;
}

__global__ static void sum_to_shape_kernel(oasp_ai_tensor_view input, oasp_ai_tensor_view output) {
    uint64_t output_index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (output_index >= output.element_count)
        return;
    const float *source = static_cast<const float *>(view_pointer(input));
    float total = 0;
    for (uint64_t input_index = 0; input_index < input.element_count; input_index++)
        if (broadcast_index(output, input, input_index) == output_index)
            total += source[physical_index(input, input_index)];
    static_cast<float *>(view_pointer(output))[physical_index(output, output_index)] = total;
}

__global__ static void unary_backward_kernel(oasp_ai_tensor_view input, oasp_ai_tensor_view output,
                                             oasp_ai_tensor_view upstream, oasp_ai_tensor_view gradient,
                                             uint32_t operation, double argument) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= gradient.element_count)
        return;
    const float x = static_cast<const float *>(view_pointer(input))[physical_index(input, index)];
    const float y = static_cast<const float *>(view_pointer(output))[physical_index(output, index)];
    const float dy = static_cast<const float *>(view_pointer(upstream))[physical_index(upstream, index)];
    float derivative = 1;
    switch (operation) {
    case OASP_AI_OP_EXP: derivative = y; break;
    case OASP_AI_OP_LOG: derivative = 1.0f / x; break;
    case OASP_AI_OP_SQRT: derivative = 0.5f / y; break;
    case OASP_AI_OP_RELU: derivative = x > 0; break;
    case OASP_AI_OP_LEAKY_RELU: derivative = x > 0 ? 1.0f : static_cast<float>(argument); break;
    case OASP_AI_OP_SIGMOID: derivative = y * (1.0f - y); break;
    case OASP_AI_OP_TANH: derivative = 1.0f - y * y; break;
    case OASP_AI_OP_SILU: {
        float sigmoid = 1.0f / (1.0f + expf(-x));
        derivative = sigmoid + x * sigmoid * (1.0f - sigmoid);
        break;
    }
    case OASP_AI_OP_GELU: {
        float inner = 0.7978845608f * (x + 0.044715f * x * x * x);
        float hyperbolic = tanhf(inner);
        derivative = 0.5f * (1.0f + hyperbolic) +
                     0.5f * x * (1.0f - hyperbolic * hyperbolic) * 0.7978845608f * (1.0f + 0.134145f * x * x);
        break;
    }
    }
    static_cast<float *>(view_pointer(gradient))[physical_index(gradient, index)] = dy * derivative;
}

__device__ static float binary_derivative(uint32_t operation, bool left, float first, float second) {
    if (operation == OASP_AI_OP_ADD)
        return 1;
    if (operation == OASP_AI_OP_SUB)
        return left ? 1.0f : -1.0f;
    if (operation == OASP_AI_OP_MUL)
        return left ? second : first;
    if (operation == OASP_AI_OP_DIV)
        return left ? 1.0f / second : -first / (second * second);
    if (operation == OASP_AI_OP_POW)
        return left ? second * powf(first, second - 1.0f) : powf(first, second) * logf(first);
    if (operation == OASP_AI_OP_MAXIMUM)
        return first == second ? 0.5f : (left ? first > second : second > first);
    return 0;
}

__global__ static void binary_backward_kernel(oasp_ai_tensor_view left, oasp_ai_tensor_view right,
                                              oasp_ai_tensor_view upstream, oasp_ai_tensor_view output_shape,
                                              oasp_ai_tensor_view gradient, uint32_t operation, int left_side) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= gradient.element_count)
        return;
    const float *left_data = static_cast<const float *>(view_pointer(left));
    const float *right_data = static_cast<const float *>(view_pointer(right));
    const float *upstream_data = static_cast<const float *>(view_pointer(upstream));
    float first = left_data[physical_index(left, broadcast_index(left, output_shape, index))];
    float second = right_data[physical_index(right, broadcast_index(right, output_shape, index))];
    static_cast<float *>(view_pointer(gradient))[physical_index(gradient, index)] =
        upstream_data[physical_index(upstream, index)] *
        binary_derivative(operation, left_side != 0, first, second);
}

__global__ static void reduce_backward_kernel(oasp_ai_tensor_view upstream, oasp_ai_tensor_view gradient,
                                              uint32_t operation, int32_t axis) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= gradient.element_count)
        return;
    uint64_t reduction = axis < 0 ? gradient.element_count : static_cast<uint64_t>(gradient.shape[axis]);
    uint64_t inner = 1;
    if (axis >= 0)
        for (uint32_t current = static_cast<uint32_t>(axis) + 1; current < gradient.rank; current++)
            inner *= static_cast<uint64_t>(gradient.shape[current]);
    uint64_t upstream_index = axis < 0 ? 0 : (index / (reduction * inner)) * inner + index % inner;
    float value = static_cast<const float *>(view_pointer(upstream))[physical_index(upstream, upstream_index)];
    if (operation == OASP_AI_OP_MEAN)
        value /= static_cast<float>(reduction);
    static_cast<float *>(view_pointer(gradient))[physical_index(gradient, index)] = value;
}

__global__ static void softmax_backward_kernel(oasp_ai_tensor_view output, oasp_ai_tensor_view upstream,
                                               oasp_ai_tensor_view gradient, uint64_t inner, uint64_t width) {
    uint64_t group_position = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    uint64_t groups = output.element_count / width;
    if (group_position >= groups)
        return;
    uint64_t group = group_position / inner;
    uint64_t position = group_position % inner;
    const float *values = static_cast<const float *>(view_pointer(output));
    const float *upstream_data = static_cast<const float *>(view_pointer(upstream));
    float *destination = static_cast<float *>(view_pointer(gradient));
    float dot = 0;
    for (uint64_t item = 0; item < width; item++) {
        uint64_t index = group * width * inner + item * inner + position;
        dot += upstream_data[physical_index(upstream, index)] * values[physical_index(output, index)];
    }
    for (uint64_t item = 0; item < width; item++) {
        uint64_t index = group * width * inner + item * inner + position;
        destination[physical_index(gradient, index)] =
            values[physical_index(output, index)] * (upstream_data[physical_index(upstream, index)] - dot);
    }
}

__global__ static void cross_entropy_backward_kernel(oasp_ai_tensor_view logits, oasp_ai_tensor_view labels,
                                                     oasp_ai_tensor_view upstream, oasp_ai_tensor_view normalizer,
                                                     oasp_ai_tensor_view weight, oasp_ai_tensor_view gradient,
                                                     oasp_ai_cross_entropy_attributes options) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= logits.element_count)
        return;
    uint64_t classes = static_cast<uint64_t>(logits.shape[1]);
    uint64_t row = index / classes;
    uint64_t column = index % classes;
    const float *values = static_cast<const float *>(view_pointer(logits));
    const int64_t *targets = static_cast<const int64_t *>(view_pointer(labels));
    int64_t label = targets[physical_index(labels, row)];
    if (label == options.ignore_index) {
        static_cast<float *>(view_pointer(gradient))[physical_index(gradient, index)] = 0;
        return;
    }
    const float *weights = options.has_weight ? static_cast<const float *>(view_pointer(weight)) : nullptr;
    float maximum = -INFINITY;
    for (uint64_t item = 0; item < classes; item++)
        maximum = fmaxf(maximum, values[physical_index(logits, row * classes + item)]);
    float total = 0;
    for (uint64_t item = 0; item < classes; item++)
        total += expf(values[physical_index(logits, row * classes + item)] - maximum);
    float probability = expf(values[physical_index(logits, index)] - maximum) / total;
    float target_weight = options.has_weight ? weights[static_cast<uint64_t>(label)] : 1.0f;
    float weight_sum = 0;
    for (uint64_t item = 0; item < classes; item++)
        weight_sum += options.has_weight ? weights[item] : 1.0f;
    float class_weight = options.has_weight ? weights[column] : 1.0f;
    float value = static_cast<float>(1.0 - options.label_smoothing) * target_weight *
                      (probability - (column == static_cast<uint64_t>(label))) +
                  static_cast<float>(options.label_smoothing) / static_cast<float>(classes) *
                      (probability * weight_sum - class_weight);
    float scale = static_cast<const float *>(view_pointer(upstream))[
        physical_index(upstream, options.reduction == OASP_AI_REDUCTION_NONE ? row : 0)];
    if (options.reduction == OASP_AI_REDUCTION_MEAN)
        scale /= static_cast<const float *>(view_pointer(normalizer))[physical_index(normalizer, 0)];
    static_cast<float *>(view_pointer(gradient))[physical_index(gradient, index)] =
        value * scale;
}

__global__ static void nll_backward_kernel(oasp_ai_tensor_view target, oasp_ai_tensor_view upstream,
                                           oasp_ai_tensor_view gradient, uint32_t reduction) {
    uint64_t row = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    uint64_t batch = static_cast<uint64_t>(target.shape[0]);
    if (row >= batch)
        return;
    uint64_t classes = static_cast<uint64_t>(gradient.shape[1]);
    const int64_t *labels = static_cast<const int64_t *>(view_pointer(target));
    const float *upstream_values = static_cast<const float *>(view_pointer(upstream));
    float scale = upstream_values[physical_index(upstream, reduction == OASP_AI_REDUCTION_NONE ? row : 0)];
    if (reduction == OASP_AI_REDUCTION_MEAN)
        scale /= static_cast<float>(batch);
    uint64_t label = static_cast<uint64_t>(labels[physical_index(target, row)]);
    static_cast<float *>(view_pointer(gradient))[physical_index(gradient, row * classes + label)] = -scale;
}

__global__ static void optimizer_kernel(oasp_ai_tensor_view parameter, oasp_ai_tensor_view gradient,
                                        oasp_ai_tensor_view first_moment, oasp_ai_tensor_view second_moment,
                                        oasp_ai_optimizer_attributes options) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= parameter.element_count)
        return;
    float *parameters = static_cast<float *>(view_pointer(parameter));
    const float *gradients = static_cast<const float *>(view_pointer(gradient));
    float *first = static_cast<float *>(view_pointer(first_moment));
    float *second = static_cast<float *>(view_pointer(second_moment));
    uint64_t parameter_index = physical_index(parameter, index);
    float value = parameters[parameter_index];
    float derivative = gradients[physical_index(gradient, index)];
    float update;
    if (options.optimizer == OASP_AI_OPTIMIZER_SGD) {
        float velocity = static_cast<float>(options.momentum) * first[physical_index(first_moment, index)] + derivative;
        first[physical_index(first_moment, index)] = velocity;
        update = velocity + static_cast<float>(options.weight_decay) * value;
    } else {
        float first_value = static_cast<float>(options.beta1) * first[physical_index(first_moment, index)] +
                            static_cast<float>(1.0 - options.beta1) * derivative;
        float second_value = static_cast<float>(options.beta2) * second[physical_index(second_moment, index)] +
                             static_cast<float>(1.0 - options.beta2) * derivative * derivative;
        first[physical_index(first_moment, index)] = first_value;
        second[physical_index(second_moment, index)] = second_value;
        float corrected_first = first_value / static_cast<float>(1.0 - pow(options.beta1, static_cast<double>(options.step)));
        float corrected_second = second_value / static_cast<float>(1.0 - pow(options.beta2, static_cast<double>(options.step)));
        update = corrected_first / (sqrtf(corrected_second) + static_cast<float>(options.epsilon));
        if (options.optimizer == OASP_AI_OPTIMIZER_ADAMW)
            update += static_cast<float>(options.weight_decay) * value;
    }
    parameters[parameter_index] = value - static_cast<float>(options.learning_rate) * update;
}

__global__ static void unscale_and_check_kernel(oasp_ai_tensor_view gradient, float *finite,
                                                float inverse_scale) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= gradient.element_count)
        return;
    float *values = static_cast<float *>(view_pointer(gradient));
    uint64_t physical = physical_index(gradient, index);
    float value = values[physical];
    if (!isfinite(value))
        atomicExch(reinterpret_cast<unsigned int *>(finite), __float_as_uint(0.0f));
    else
        values[physical] = value * inverse_scale;
}

__device__ static bool attention_masked(oasp_ai_tensor_view mask, uint64_t batch, uint64_t row, uint64_t column,
                                        uint64_t queries, uint64_t keys, int causal, int has_mask) {
    if (causal && column > row)
        return true;
    if (!has_mask)
        return false;
    uint64_t index = mask.element_count == queries * keys ? row * keys + column : batch * queries * keys + row * keys + column;
    return !static_cast<const uint8_t *>(view_pointer(mask))[physical_index(mask, index)];
}

static bool row_major_contiguous(const oasp_ai_tensor_view &view) {
    int64_t stride = 1;
    for (uint32_t axis = view.rank; axis; axis--) {
        if (view.strides[axis - 1] != stride)
            return false;
        stride *= view.shape[axis - 1];
    }
    return true;
}

__global__ static void attention_mask_kernel(oasp_ai_tensor_view mask, float *scores, uint64_t batches,
                                             uint64_t queries, uint64_t keys, int causal, int has_mask) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= batches * queries * keys)
        return;
    uint64_t batch = index / (queries * keys);
    uint64_t row = index / keys % queries;
    uint64_t column = index % keys;
    if (attention_masked(mask, batch, row, column, queries, keys, causal, has_mask))
        scores[index] = -INFINITY;
}

__global__ static void attention_scores_kernel(oasp_ai_tensor_view query, oasp_ai_tensor_view key,
                                               oasp_ai_tensor_view mask, float *scores, int causal, int has_mask) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    uint64_t queries = static_cast<uint64_t>(query.shape[query.rank - 2]);
    uint64_t keys = static_cast<uint64_t>(key.shape[key.rank - 2]);
    uint64_t width = static_cast<uint64_t>(query.shape[query.rank - 1]);
    uint64_t batches = query.element_count / (queries * width);
    if (index >= batches * queries * keys)
        return;
    uint64_t batch = index / (queries * keys);
    uint64_t row = index / keys % queries;
    uint64_t column = index % keys;
    if (attention_masked(mask, batch, row, column, queries, keys, causal, has_mask)) {
        scores[index] = -INFINITY;
        return;
    }
    const float *queries_data = static_cast<const float *>(view_pointer(query));
    const float *keys_data = static_cast<const float *>(view_pointer(key));
    float score = 0;
    for (uint64_t inner = 0; inner < width; inner++)
        score += queries_data[physical_index(query, batch * queries * width + row * width + inner)] *
                 keys_data[physical_index(key, batch * keys * width + column * width + inner)];
    scores[index] = score * rsqrtf(static_cast<float>(width));
}

__global__ static void attention_softmax_kernel(float *scores, uint64_t rows, uint64_t keys) {
    uint64_t row = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (row >= rows)
        return;
    float *values = scores + row * keys;
    float maximum = -INFINITY;
    for (uint64_t column = 0; column < keys; column++)
        maximum = fmaxf(maximum, values[column]);
    if (maximum == -INFINITY) {
        for (uint64_t column = 0; column < keys; column++)
            values[column] = 0;
        return;
    }
    float total = 0;
    for (uint64_t column = 0; column < keys; column++) {
        float probability = values[column] == -INFINITY ? 0 : expf(values[column] - maximum);
        values[column] = probability;
        total += probability;
    }
    if (total)
        for (uint64_t column = 0; column < keys; column++)
            values[column] /= total;
}

__global__ static void attention_output_kernel(const float *probabilities, oasp_ai_tensor_view value,
                                               oasp_ai_tensor_view output) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= output.element_count)
        return;
    uint64_t queries = static_cast<uint64_t>(output.shape[output.rank - 2]);
    uint64_t keys = static_cast<uint64_t>(value.shape[value.rank - 2]);
    uint64_t value_width = static_cast<uint64_t>(value.shape[value.rank - 1]);
    uint64_t batch = index / (queries * value_width);
    uint64_t row = index / value_width % queries;
    uint64_t channel = index % value_width;
    const float *values = static_cast<const float *>(view_pointer(value));
    float result = 0;
    for (uint64_t column = 0; column < keys; column++)
        result += probabilities[batch * queries * keys + row * keys + column] *
                  values[physical_index(value, batch * keys * value_width + column * value_width + channel)];
    static_cast<float *>(view_pointer(output))[physical_index(output, index)] = result;
}

__global__ static void attention_probability_gradient_kernel(oasp_ai_tensor_view value, oasp_ai_tensor_view upstream,
                                                             float *probability_gradient, uint64_t queries) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    uint64_t keys = static_cast<uint64_t>(value.shape[value.rank - 2]);
    uint64_t value_width = static_cast<uint64_t>(value.shape[value.rank - 1]);
    uint64_t batches = upstream.element_count / (queries * value_width);
    if (index >= batches * queries * keys)
        return;
    uint64_t batch = index / (queries * keys);
    uint64_t row = index / keys % queries;
    uint64_t column = index % keys;
    const float *values = static_cast<const float *>(view_pointer(value));
    const float *upstream_data = static_cast<const float *>(view_pointer(upstream));
    float result = 0;
    for (uint64_t channel = 0; channel < value_width; channel++)
        result += upstream_data[physical_index(upstream, batch * queries * value_width + row * value_width + channel)] *
                  values[physical_index(value, batch * keys * value_width + column * value_width + channel)];
    probability_gradient[index] = result;
}

__global__ static void attention_score_gradient_kernel(const float *probabilities, float *gradient,
                                                       uint64_t rows, uint64_t keys, float scale) {
    uint64_t row = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (row >= rows)
        return;
    uint64_t offset = row * keys;
    float dot = 0;
    for (uint64_t column = 0; column < keys; column++)
        dot += probabilities[offset + column] * gradient[offset + column];
    for (uint64_t column = 0; column < keys; column++)
        gradient[offset + column] = probabilities[offset + column] * (gradient[offset + column] - dot) * scale;
}

__global__ static void attention_value_gradient_kernel(const float *probabilities, oasp_ai_tensor_view upstream,
                                                       oasp_ai_tensor_view value_gradient, uint64_t queries) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= value_gradient.element_count)
        return;
    uint64_t keys = static_cast<uint64_t>(value_gradient.shape[value_gradient.rank - 2]);
    uint64_t width = static_cast<uint64_t>(value_gradient.shape[value_gradient.rank - 1]);
    uint64_t batch = index / (keys * width);
    uint64_t column = index / width % keys;
    uint64_t channel = index % width;
    const float *upstream_data = static_cast<const float *>(view_pointer(upstream));
    float result = 0;
    for (uint64_t row = 0; row < queries; row++)
        result += probabilities[batch * queries * keys + row * keys + column] *
                  upstream_data[physical_index(upstream, batch * queries * width + row * width + channel)];
    static_cast<float *>(view_pointer(value_gradient))[physical_index(value_gradient, index)] = result;
}

__global__ static void attention_query_gradient_kernel(const float *score_gradient, oasp_ai_tensor_view key,
                                                       oasp_ai_tensor_view query_gradient) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= query_gradient.element_count)
        return;
    uint64_t queries = static_cast<uint64_t>(query_gradient.shape[query_gradient.rank - 2]);
    uint64_t keys = static_cast<uint64_t>(key.shape[key.rank - 2]);
    uint64_t width = static_cast<uint64_t>(query_gradient.shape[query_gradient.rank - 1]);
    uint64_t batch = index / (queries * width);
    uint64_t row = index / width % queries;
    uint64_t channel = index % width;
    const float *keys_data = static_cast<const float *>(view_pointer(key));
    float result = 0;
    for (uint64_t column = 0; column < keys; column++)
        result += score_gradient[batch * queries * keys + row * keys + column] *
                  keys_data[physical_index(key, batch * keys * width + column * width + channel)];
    static_cast<float *>(view_pointer(query_gradient))[physical_index(query_gradient, index)] = result;
}

__global__ static void attention_key_gradient_kernel(const float *score_gradient, oasp_ai_tensor_view query,
                                                     oasp_ai_tensor_view key_gradient) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= key_gradient.element_count)
        return;
    uint64_t queries = static_cast<uint64_t>(query.shape[query.rank - 2]);
    uint64_t keys = static_cast<uint64_t>(key_gradient.shape[key_gradient.rank - 2]);
    uint64_t width = static_cast<uint64_t>(key_gradient.shape[key_gradient.rank - 1]);
    uint64_t batch = index / (keys * width);
    uint64_t column = index / width % keys;
    uint64_t channel = index % width;
    const float *queries_data = static_cast<const float *>(view_pointer(query));
    float result = 0;
    for (uint64_t row = 0; row < queries; row++)
        result += score_gradient[batch * queries * keys + row * keys + column] *
                  queries_data[physical_index(query, batch * queries * width + row * width + channel)];
    static_cast<float *>(view_pointer(key_gradient))[physical_index(key_gradient, index)] = result;
}

__global__ static void f32_to_f16_kernel(const float *input, __half *output, uint64_t count) {
    uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index < count)
        output[index] = __float2half(input[index]);
}

__global__ static void finalize_reduction_kernel(float *value, float divisor, int square_root) {
    if (!blockIdx.x && !threadIdx.x)
        *value = square_root ? sqrtf(*value) : *value / divisor;
}

struct square_transform {
    __host__ __device__ float operator()(float value) const {
        return value * value;
    }
};

static int execute_cublas_lt_matmul(cuda_stream_state *state, const oasp_ai_tensor_view &left,
                                    const oasp_ai_tensor_view &right, oasp_ai_tensor_view &output,
                                    float alpha, oasp_ai_error *error) {
    uint64_t rows = static_cast<uint64_t>(left.shape[left.rank - 2]);
    uint64_t shared = static_cast<uint64_t>(left.shape[left.rank - 1]);
    uint64_t columns = static_cast<uint64_t>(right.shape[right.rank - 1]);
    uint64_t batches = output.element_count / (rows * columns);
    uint64_t left_matrix = rows * shared;
    uint64_t right_matrix = shared * columns;
    uint64_t left_stride = left.element_count == left_matrix ? 0 : left_matrix;
    uint64_t right_stride = right.element_count == right_matrix ? 0 : right_matrix;
    const void *left_pointer = view_pointer(left);
    const void *right_pointer = view_pointer(right);
    cudaDataType_t input_type = CUDA_R_32F;
    constexpr unsigned threads = 256;
    size_t left_half_bytes = static_cast<size_t>(left.element_count) * sizeof(__half);
    size_t right_half_bytes = static_cast<size_t>(right.element_count) * sizeof(__half);
    if (state->fp16_tensor_cores && left_half_bytes + right_half_bytes <= state->fp16_workspace_bytes) {
        __half *left_half = state->fp16_workspace;
        __half *right_half = reinterpret_cast<__half *>(
            reinterpret_cast<unsigned char *>(state->fp16_workspace) + left_half_bytes);
        f32_to_f16_kernel<<<static_cast<unsigned>((left.element_count + threads - 1) / threads), threads, 0, state->stream>>>(
            static_cast<const float *>(left_pointer), left_half, left.element_count);
        f32_to_f16_kernel<<<static_cast<unsigned>((right.element_count + threads - 1) / threads), threads, 0, state->stream>>>(
            static_cast<const float *>(right_pointer), right_half, right.element_count);
        left_pointer = left_half;
        right_pointer = right_half;
        input_type = CUDA_R_16F;
    }

    cublas_lt_plan *plan = nullptr;
    for (cublas_lt_plan *candidate : state->cublas_lt_plans)
        if (candidate->rows == rows && candidate->shared == shared && candidate->columns == columns &&
            candidate->batches == batches && candidate->left_stride == left_stride &&
            candidate->right_stride == right_stride && candidate->input_type == input_type) {
            plan = candidate;
            break;
        }
    cublasStatus_t status = CUBLAS_STATUS_SUCCESS;
    if (!plan) {
        plan = new (std::nothrow) cublas_lt_plan{
            rows, shared, columns, batches, left_stride, right_stride, input_type, nullptr, nullptr, nullptr, nullptr, {}, 0};
        if (!plan)
            return fail(error, 7, "CublasLtMatMul", "unable to allocate cuBLASLt plan metadata",
                        "Reduce host memory pressure.");
        status = cublasLtMatmulDescCreate(&plan->operation, CUBLAS_COMPUTE_32F, CUDA_R_32F);
        if (status == CUBLAS_STATUS_SUCCESS)
            status = cublasLtMatrixLayoutCreate(&plan->right_layout, input_type, columns, shared,
                                                static_cast<int64_t>(columns));
        if (status == CUBLAS_STATUS_SUCCESS)
            status = cublasLtMatrixLayoutCreate(&plan->left_layout, input_type, shared, rows,
                                                static_cast<int64_t>(shared));
        if (status == CUBLAS_STATUS_SUCCESS)
            status = cublasLtMatrixLayoutCreate(&plan->output_layout, CUDA_R_32F, columns, rows,
                                                static_cast<int64_t>(columns));
        int batch_count = static_cast<int>(batches);
        int64_t right_batch_stride = static_cast<int64_t>(right_stride);
        int64_t left_batch_stride = static_cast<int64_t>(left_stride);
        int64_t output_batch_stride = static_cast<int64_t>(rows * columns);
        if (status == CUBLAS_STATUS_SUCCESS && batches > 1)
            status = cublasLtMatrixLayoutSetAttribute(plan->right_layout, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT,
                                                      &batch_count, sizeof(batch_count));
        if (status == CUBLAS_STATUS_SUCCESS && batches > 1)
            status = cublasLtMatrixLayoutSetAttribute(plan->right_layout, CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET,
                                                      &right_batch_stride, sizeof(right_batch_stride));
        if (status == CUBLAS_STATUS_SUCCESS && batches > 1)
            status = cublasLtMatrixLayoutSetAttribute(plan->left_layout, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT,
                                                      &batch_count, sizeof(batch_count));
        if (status == CUBLAS_STATUS_SUCCESS && batches > 1)
            status = cublasLtMatrixLayoutSetAttribute(plan->left_layout, CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET,
                                                      &left_batch_stride, sizeof(left_batch_stride));
        if (status == CUBLAS_STATUS_SUCCESS && batches > 1)
            status = cublasLtMatrixLayoutSetAttribute(plan->output_layout, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT,
                                                      &batch_count, sizeof(batch_count));
        if (status == CUBLAS_STATUS_SUCCESS && batches > 1)
            status = cublasLtMatrixLayoutSetAttribute(plan->output_layout, CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET,
                                                      &output_batch_stride, sizeof(output_batch_stride));
        cublasLtMatmulPreference_t preference = nullptr;
        if (status == CUBLAS_STATUS_SUCCESS)
            status = cublasLtMatmulPreferenceCreate(&preference);
        if (status == CUBLAS_STATUS_SUCCESS)
            status = cublasLtMatmulPreferenceSetAttribute(preference, CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES,
                                                          &state->cublas_workspace_bytes,
                                                          sizeof(state->cublas_workspace_bytes));
        cublasLtMatmulHeuristicResult_t heuristic{};
        int returned = 0;
        if (status == CUBLAS_STATUS_SUCCESS)
            status = cublasLtMatmulAlgoGetHeuristic(state->cublas_lt, plan->operation, plan->right_layout,
                                                    plan->left_layout, plan->output_layout, plan->output_layout,
                                                    preference, 1, &heuristic, &returned);
        if (preference)
            cublasLtMatmulPreferenceDestroy(preference);
        if (status == CUBLAS_STATUS_SUCCESS && returned)
            plan->algorithm = heuristic.algo;
        else if (status == CUBLAS_STATUS_SUCCESS)
            status = CUBLAS_STATUS_NOT_SUPPORTED;
        if (status != CUBLAS_STATUS_SUCCESS) {
            destroy_cublas_lt_plan(plan);
            return cublas_fail(error, "CublasLtMatMulPlan", status);
        }
        if (state->cublas_lt_plans.size() >= 64) {
            auto oldest = std::min_element(
                state->cublas_lt_plans.begin(), state->cublas_lt_plans.end(),
                [](const cublas_lt_plan *left_plan, const cublas_lt_plan *right_plan) {
                    return left_plan->last_used < right_plan->last_used;
                });
            destroy_cublas_lt_plan(*oldest);
            state->cublas_lt_plans.erase(oldest);
        }
        state->cublas_lt_plans.push_back(plan);
    }
    plan->last_used = ++state->cublas_lt_clock;
    float beta = 0;
    status = cublasLtMatmul(state->cublas_lt, plan->operation, &alpha, right_pointer, plan->right_layout,
                            left_pointer, plan->left_layout, &beta, view_pointer(output), plan->output_layout,
                            view_pointer(output), plan->output_layout, &plan->algorithm, state->cublas_workspace,
                            state->cublas_workspace_bytes, state->stream);
    return status == CUBLAS_STATUS_SUCCESS ? 0 : cublas_fail(error, "CublasLtMatMul", status);
}

static int execute_cub_reduction(cuda_stream_state *state, const oasp_ai_tensor_view &input,
                                 oasp_ai_tensor_view &output, uint32_t operation, oasp_ai_error *error) {
    const float *source = static_cast<const float *>(view_pointer(input));
    float *destination = static_cast<float *>(view_pointer(output));
    size_t temporary_bytes = 0;
    cudaError_t status = cudaSuccess;
    if (operation == OASP_AI_OP_SUM || operation == OASP_AI_OP_MEAN)
        status = cub::DeviceReduce::Sum(nullptr, temporary_bytes, source, destination,
                                        input.element_count, state->stream);
    else if (operation == OASP_AI_OP_MAX)
        status = cub::DeviceReduce::Max(nullptr, temporary_bytes, source, destination,
                                        input.element_count, state->stream);
    else if (operation == OASP_AI_OP_MIN)
        status = cub::DeviceReduce::Min(nullptr, temporary_bytes, source, destination,
                                        input.element_count, state->stream);
    else {
        cub::TransformInputIterator<float, square_transform, const float *> squared(source, square_transform{});
        status = cub::DeviceReduce::Sum(nullptr, temporary_bytes, squared, destination,
                                        input.element_count, state->stream);
    }
    void *temporary = nullptr;
    if (status == cudaSuccess)
        status = cudaMallocAsync(&temporary, temporary_bytes, state->stream);
    if (status == cudaSuccess && (operation == OASP_AI_OP_SUM || operation == OASP_AI_OP_MEAN))
        status = cub::DeviceReduce::Sum(temporary, temporary_bytes, source, destination,
                                        input.element_count, state->stream);
    else if (status == cudaSuccess && operation == OASP_AI_OP_MAX)
        status = cub::DeviceReduce::Max(temporary, temporary_bytes, source, destination,
                                        input.element_count, state->stream);
    else if (status == cudaSuccess && operation == OASP_AI_OP_MIN)
        status = cub::DeviceReduce::Min(temporary, temporary_bytes, source, destination,
                                        input.element_count, state->stream);
    else if (status == cudaSuccess) {
        cub::TransformInputIterator<float, square_transform, const float *> squared(source, square_transform{});
        status = cub::DeviceReduce::Sum(temporary, temporary_bytes, squared, destination,
                                        input.element_count, state->stream);
    }
    if (status == cudaSuccess && operation == OASP_AI_OP_MEAN)
        finalize_reduction_kernel<<<1, 1, 0, state->stream>>>(
            destination, static_cast<float>(input.element_count), 0);
    else if (status == cudaSuccess && operation == OASP_AI_OP_NORM)
        finalize_reduction_kernel<<<1, 1, 0, state->stream>>>(destination, 1, 1);
    cudaError_t free_status = temporary ? cudaFreeAsync(temporary, state->stream) : cudaSuccess;
    if (status == cudaSuccess)
        status = free_status;
    return status == cudaSuccess ? 0 : cuda_fail(error, "CubReduce", status);
}

static int execute_views(cuda_stream_state *stream, uint32_t operation, const oasp_ai_tensor_view *inputs, size_t input_count,
                         oasp_ai_tensor_view *outputs, size_t output_count, const void *attributes, size_t attributes_size,
                         oasp_ai_error *error) {
    if (!stream || !outputs || !output_count)
        return fail(error, 5, "CudaExecuteOp", "invalid CUDA operator bindings", "Pass valid input and output tensor views.");
    cudaError_t cuda_status = cudaSetDevice(stream->device->ordinal);
    if (cuda_status != cudaSuccess)
        return cuda_fail(error, "CudaSetDevice", cuda_status);
    std::vector<cuda_storage *> input_storages;
    std::vector<cuda_storage *> output_storages;
    std::vector<oasp_ai_tensor_view> resolved_inputs;
    if (input_count)
        resolved_inputs.assign(inputs, inputs + input_count);
    std::vector<oasp_ai_tensor_view> resolved_outputs(outputs, outputs + output_count);
    for (oasp_ai_tensor_view &view : resolved_inputs) {
        cuda_storage *storage = storage_from(view.storage);
        if (!storage)
            return fail(error, 5, "CudaExecuteOp", "input storage handle is invalid", "Use tensors allocated by this CUDA backend.");
        if (!stream->capturing && wait_storage_ready(storage, stream->stream, error))
            return -1;
        input_storages.push_back(storage);
        view.storage = reinterpret_cast<oasp_ai_backend_storage>(storage->pointer);
    }
    for (oasp_ai_tensor_view &view : resolved_outputs) {
        cuda_storage *storage = storage_from(view.storage);
        if (!storage)
            return fail(error, 5, "CudaExecuteOp", "output storage handle is invalid", "Use tensors allocated by this CUDA backend.");
        if (!stream->capturing && wait_storage_ready(storage, stream->stream, error))
            return -1;
        output_storages.push_back(storage);
        view.storage = reinterpret_cast<oasp_ai_backend_storage>(storage->pointer);
    }
    inputs = resolved_inputs.data();
    outputs = resolved_outputs.data();
    for (size_t index = 0; index < input_count; index++)
        if (inputs[index].dtype != OASP_AI_DTYPE_F32 && operation != OASP_AI_OP_CONTIGUOUS &&
            !((operation == OASP_AI_OP_CROSS_ENTROPY || operation == OASP_AI_BACKEND_OP_CROSS_ENTROPY_BACKWARD) && index == 1) &&
            !((operation == OASP_AI_OP_CTC_LOSS || operation == OASP_AI_BACKEND_OP_CTC_LOSS_BACKWARD) &&
              index >= 1 && index <= 3 && inputs[index].dtype == OASP_AI_DTYPE_I64))
            if (!(((operation == OASP_AI_OP_NLL && index == 1) ||
                   (operation == OASP_AI_BACKEND_OP_NLL_BACKWARD && index == 0)) &&
                  inputs[index].dtype == OASP_AI_DTYPE_I64))
            if (!((operation == OASP_AI_OP_ATTENTION || operation == OASP_AI_BACKEND_OP_ATTENTION_BACKWARD) &&
                  index == 3 && inputs[index].dtype == OASP_AI_DTYPE_BOOL))
            return fail(error, 5, "CudaExecuteOp", "CUDA backend currently requires float32 tensors", "Cast tensors to float32.");
    for (size_t index = 0; index < output_count; index++)
        if (outputs[index].dtype != OASP_AI_DTYPE_F32 && operation != OASP_AI_OP_CONTIGUOUS)
            return fail(error, 5, "CudaExecuteOp", "CUDA backend currently requires float32 outputs", "Cast tensors to float32.");
    constexpr unsigned threads = 256;
    uint64_t work = outputs[0].element_count;
    if (operation == OASP_AI_OP_CONTIGUOUS && input_count == 1 && output_count == 1 &&
        inputs[0].dtype == outputs[0].dtype && inputs[0].element_count == outputs[0].element_count) {
        uint32_t element_bytes = dtype_bytes(inputs[0].dtype);
        if (!element_bytes)
            return fail(error, 5, "CudaContiguous", "unsupported tensor dtype", "Use a tensor dtype supported by the CUDA backend.");
        contiguous_kernel<<<static_cast<unsigned>((work + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], outputs[0], element_bytes);
    } else if (operation >= OASP_AI_OP_ADD && operation <= OASP_AI_OP_MAXIMUM && input_count == 2)
        binary_kernel<<<static_cast<unsigned>((work + threads - 1) / threads), threads, 0, stream->stream>>>(inputs[0], inputs[1], outputs[0], operation);
    else if (operation >= OASP_AI_OP_EXP && operation <= OASP_AI_OP_SILU && input_count == 1) {
        double argument = attributes_size == sizeof(oasp_ai_unary_attributes)
                              ? static_cast<const oasp_ai_unary_attributes *>(attributes)->argument : 0.01;
        unary_kernel<<<static_cast<unsigned>((work + threads - 1) / threads), threads, 0, stream->stream>>>(inputs[0], outputs[0], operation, argument);
    } else if ((operation == OASP_AI_OP_SUM || operation == OASP_AI_OP_MEAN || operation == OASP_AI_OP_MAX ||
                operation == OASP_AI_OP_MIN || operation == OASP_AI_OP_NORM) && input_count == 1) {
        int32_t axis = attributes_size == sizeof(oasp_ai_reduce_attributes)
                           ? static_cast<const oasp_ai_reduce_attributes *>(attributes)->axis : -1;
        if (axis < 0 && row_major_contiguous(inputs[0]) && row_major_contiguous(outputs[0]) &&
            outputs[0].element_count == 1) {
            if (execute_cub_reduction(stream, inputs[0], outputs[0], operation, error))
                return -1;
        } else
            reduce_kernel<<<static_cast<unsigned>((work + threads - 1) / threads), threads, 0, stream->stream>>>(
                inputs[0], outputs[0], operation, axis);
    } else if ((operation == OASP_AI_OP_MATMUL || operation == OASP_AI_OP_BATCHED_MATMUL) && input_count == 2) {
        uint64_t rows = static_cast<uint64_t>(inputs[0].shape[inputs[0].rank - 2]);
        uint64_t shared = static_cast<uint64_t>(inputs[0].shape[inputs[0].rank - 1]);
        uint64_t columns = static_cast<uint64_t>(inputs[1].shape[inputs[1].rank - 1]);
        uint32_t left_rank = inputs[0].rank;
        uint32_t right_rank = inputs[1].rank;
        bool left_row_major = inputs[0].strides[left_rank - 1] == 1 &&
                              inputs[0].strides[left_rank - 2] == static_cast<int64_t>(shared);
        bool left_transposed = inputs[0].strides[left_rank - 2] == 1 &&
                               inputs[0].strides[left_rank - 1] == static_cast<int64_t>(rows);
        bool right_row_major = inputs[1].strides[right_rank - 1] == 1 &&
                               inputs[1].strides[right_rank - 2] == static_cast<int64_t>(columns);
        bool right_transposed = inputs[1].strides[right_rank - 2] == 1 &&
                                inputs[1].strides[right_rank - 1] == static_cast<int64_t>(shared);
        bool cublas_layout = (left_row_major || left_transposed) && (right_row_major || right_transposed) &&
                             outputs[0].strides[outputs[0].rank - 1] == 1;
        bool cublas_lt_layout = row_major_contiguous(inputs[0]) && row_major_contiguous(inputs[1]) &&
                                row_major_contiguous(outputs[0]);
        uint64_t batches = work / (rows * columns);
        if (cublas_lt_layout) {
            if (execute_cublas_lt_matmul(stream, inputs[0], inputs[1], outputs[0], 1.0f, error))
                return -1;
        } else if (cublas_layout) {
            float alpha = 1;
            float beta = 0;
            const void *left_pointer = view_pointer(inputs[0]);
            const void *right_pointer = view_pointer(inputs[1]);
            cudaDataType_t input_type = CUDA_R_32F;
            cublasGemmAlgo_t algorithm = CUBLAS_GEMM_DEFAULT;
            size_t left_half_bytes = static_cast<size_t>(inputs[0].element_count) * sizeof(__half);
            size_t right_half_bytes = static_cast<size_t>(inputs[1].element_count) * sizeof(__half);
            if (stream->fp16_tensor_cores &&
                left_half_bytes + right_half_bytes <= stream->fp16_workspace_bytes) {
                __half *left_half = stream->fp16_workspace;
                __half *right_half = reinterpret_cast<__half *>(
                    reinterpret_cast<unsigned char *>(stream->fp16_workspace) + left_half_bytes);
                f32_to_f16_kernel<<<static_cast<unsigned>((inputs[0].element_count + threads - 1) / threads),
                                    threads, 0, stream->stream>>>(
                    static_cast<const float *>(left_pointer), left_half, inputs[0].element_count);
                f32_to_f16_kernel<<<static_cast<unsigned>((inputs[1].element_count + threads - 1) / threads),
                                    threads, 0, stream->stream>>>(
                    static_cast<const float *>(right_pointer), right_half, inputs[1].element_count);
                left_pointer = left_half;
                right_pointer = right_half;
                input_type = CUDA_R_16F;
                algorithm = CUBLAS_GEMM_DEFAULT_TENSOR_OP;
            }
            cublasStatus_t status = cublasSetStream(stream->cublas, stream->stream);
            if (status == CUBLAS_STATUS_SUCCESS)
                status = cublasGemmStridedBatchedEx(
                    stream->cublas, right_transposed ? CUBLAS_OP_T : CUBLAS_OP_N,
                    left_transposed ? CUBLAS_OP_T : CUBLAS_OP_N,
                    static_cast<int>(columns), static_cast<int>(rows), static_cast<int>(shared),
                    &alpha, right_pointer, input_type,
                    static_cast<int>(right_transposed ? shared : columns),
                    static_cast<long long>(shared * columns), left_pointer, input_type,
                    static_cast<int>(left_transposed ? rows : shared),
                    static_cast<long long>(rows * shared), &beta, view_pointer(outputs[0]), CUDA_R_32F,
                    static_cast<int>(columns), static_cast<long long>(rows * columns),
                    static_cast<int>(batches), CUBLAS_COMPUTE_32F, algorithm);
            if (status != CUBLAS_STATUS_SUCCESS)
                return cublas_fail(error, "CublasMatMul", status);
        } else
            matmul_kernel<<<static_cast<unsigned>((work + threads - 1) / threads), threads, 0, stream->stream>>>(
                inputs[0], inputs[1], outputs[0], rows, shared, columns);
    } else if (operation == OASP_AI_OP_SOFTMAX && input_count == 1) {
        int32_t axis = attributes_size == sizeof(oasp_ai_axis_attributes)
                           ? static_cast<const oasp_ai_axis_attributes *>(attributes)->axis : static_cast<int32_t>(inputs[0].rank) - 1;
        uint64_t inner = 1;
        for (uint32_t current = static_cast<uint32_t>(axis) + 1; current < inputs[0].rank; current++)
            inner *= static_cast<uint64_t>(inputs[0].shape[current]);
        uint64_t width = static_cast<uint64_t>(inputs[0].shape[axis]);
        uint64_t groups = inputs[0].element_count / width;
        softmax_kernel<<<static_cast<unsigned>((groups + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], outputs[0], inner, width);
    } else if (operation == OASP_AI_OP_CROSS_ENTROPY && input_count >= 2 && output_count == 2) {
        oasp_ai_cross_entropy_attributes options = {OASP_AI_REDUCTION_MEAN, -100, 0, 0};
        if (attributes_size == sizeof(options))
            options = *static_cast<const oasp_ai_cross_entropy_attributes *>(attributes);
        oasp_ai_tensor_view empty{};
        cross_entropy_kernel<<<1, 1, 0, stream->stream>>>(
            inputs[0], inputs[1], options.has_weight ? inputs[2] : empty, outputs[0],
            outputs[1], options);
    } else if (operation == OASP_AI_OP_CTC_LOSS && input_count == 4 &&
               output_count == 3 && attributes_size == sizeof(oasp_ai_ctc_attributes)) {
        const auto options = *static_cast<const oasp_ai_ctc_attributes *>(attributes);
        uint64_t batch = static_cast<uint64_t>(inputs[0].shape[1]);
        ctc_forward_kernel<<<static_cast<unsigned>((batch + threads - 1) / threads),
                             threads, 0, stream->stream>>>(
            inputs[0], inputs[1], inputs[2], inputs[3], outputs[1], outputs[2],
            options.blank);
        ctc_reduce_kernel<<<1, 1, 0, stream->stream>>>(
            inputs[3], outputs[2], outputs[0], options);
    }
    else if (operation == OASP_AI_OP_NLL && input_count == 2 &&
             attributes_size == sizeof(oasp_ai_loss_attributes)) {
        uint64_t batch = static_cast<uint64_t>(inputs[0].shape[0]);
        nll_kernel<<<static_cast<unsigned>((batch + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], inputs[1], outputs[0],
            static_cast<const oasp_ai_loss_attributes *>(attributes)->reduction);
    }
    else if ((operation == OASP_AI_OP_LAYER_NORM || operation == OASP_AI_OP_RMS_NORM) && input_count >= 1) {
        oasp_ai_norm_attributes options = {1e-5, operation == OASP_AI_OP_RMS_NORM};
        if (attributes_size == sizeof(options))
            options = *static_cast<const oasp_ai_norm_attributes *>(attributes);
        oasp_ai_tensor_view empty{};
        uint64_t rows = inputs[0].element_count / static_cast<uint64_t>(inputs[0].shape[inputs[0].rank - 1]);
        bool executed = false;
#if defined(OASP_AI_WITH_CUDNN)
        if (input_count > 2 && row_major_contiguous(inputs[0]) && row_major_contiguous(outputs[0]) &&
            inputs[1].element_count == static_cast<uint64_t>(inputs[0].shape[inputs[0].rank - 1]) &&
            inputs[2].element_count == inputs[1].element_count)
            executed = cuda_cudnn_execute_norm(stream->cudnn, inputs[0], inputs[1], inputs[2],
                                               outputs[0], options.epsilon, options.rms_only,
                                               !stream->capturing);
#endif
        if (!executed)
            norm_kernel<<<static_cast<unsigned>((rows + threads - 1) / threads), threads, 0, stream->stream>>>(
                inputs[0], input_count > 1 ? inputs[1] : empty, input_count > 2 ? inputs[2] : empty, outputs[0],
                options.epsilon, options.rms_only, input_count > 1, input_count > 2);
    } else if (operation == OASP_AI_BACKEND_OP_NORM_BACKWARD && input_count >= 2 &&
               attributes_size == sizeof(oasp_ai_norm_attributes)) {
        const auto *options = static_cast<const oasp_ai_norm_attributes *>(attributes);
        int has_weight = output_count > 1;
        int has_bias = output_count > 2;
        oasp_ai_tensor_view empty{};
        oasp_ai_tensor_view weight = has_weight ? inputs[1] : empty;
        oasp_ai_tensor_view upstream = inputs[input_count - 1];
        uint64_t width = static_cast<uint64_t>(inputs[0].shape[inputs[0].rank - 1]);
        uint64_t rows = inputs[0].element_count / width;
        float *statistics = nullptr;
        cuda_status = cudaMallocAsync(reinterpret_cast<void **>(&statistics),
                                      static_cast<size_t>(rows * 2) * sizeof(*statistics), stream->stream);
        if (cuda_status != cudaSuccess)
            return cuda_fail(error, "CudaNormBackwardWorkspace", cuda_status);
        float *means = statistics;
        float *inverses = statistics + rows;
        norm_stats_kernel<<<static_cast<unsigned>((rows + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], means, inverses, options->epsilon, options->rms_only);
        norm_input_backward_kernel<<<static_cast<unsigned>((rows + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], weight, upstream, means, inverses, outputs[0], options->rms_only, has_weight);
        if (has_weight || has_bias)
            norm_parameter_backward_kernel<<<static_cast<unsigned>((width + threads - 1) / threads), threads, 0, stream->stream>>>(
                inputs[0], upstream, means, inverses, has_weight ? outputs[1] : empty,
                has_bias ? outputs[2] : empty, options->rms_only, has_weight, has_bias);
        cuda_status = cudaFreeAsync(statistics, stream->stream);
        if (cuda_status != cudaSuccess)
            return cuda_fail(error, "CudaNormBackwardWorkspaceFree", cuda_status);
    } else if (operation == OASP_AI_BACKEND_OP_FILL && input_count == 0 &&
               attributes_size == sizeof(oasp_ai_fill_attributes)) {
        float value = static_cast<float>(static_cast<const oasp_ai_fill_attributes *>(attributes)->value);
        fill_kernel<<<static_cast<unsigned>((work + threads - 1) / threads), threads, 0, stream->stream>>>(outputs[0], value);
    } else if (operation == OASP_AI_BACKEND_OP_SUM_TO_SHAPE && input_count == 1) {
        sum_to_shape_kernel<<<static_cast<unsigned>((work + threads - 1) / threads), threads, 0, stream->stream>>>(inputs[0], outputs[0]);
    } else if (operation == OASP_AI_BACKEND_OP_UNARY_BACKWARD && input_count == 3 &&
               attributes_size == sizeof(oasp_ai_backward_attributes)) {
        const auto *options = static_cast<const oasp_ai_backward_attributes *>(attributes);
        unary_backward_kernel<<<static_cast<unsigned>((work + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], inputs[1], inputs[2], outputs[0], options->forward_op, options->argument);
    } else if (operation == OASP_AI_BACKEND_OP_BINARY_BACKWARD && input_count == 3 && output_count == 2 &&
               attributes_size == sizeof(oasp_ai_backward_attributes)) {
        const auto *options = static_cast<const oasp_ai_backward_attributes *>(attributes);
        binary_backward_kernel<<<static_cast<unsigned>((outputs[0].element_count + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], inputs[1], inputs[2], inputs[2], outputs[0], options->forward_op, 1);
        binary_backward_kernel<<<static_cast<unsigned>((outputs[1].element_count + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], inputs[1], inputs[2], inputs[2], outputs[1], options->forward_op, 0);
    } else if (operation == OASP_AI_BACKEND_OP_REDUCE_BACKWARD && input_count == 1 &&
               attributes_size == sizeof(oasp_ai_reduce_backward_attributes)) {
        const auto *options = static_cast<const oasp_ai_reduce_backward_attributes *>(attributes);
        reduce_backward_kernel<<<static_cast<unsigned>((work + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], outputs[0], options->forward_op, options->axis);
    } else if (operation == OASP_AI_BACKEND_OP_SOFTMAX_BACKWARD && input_count == 2 &&
               attributes_size == sizeof(oasp_ai_axis_attributes)) {
        int32_t axis = static_cast<const oasp_ai_axis_attributes *>(attributes)->axis;
        uint64_t inner = 1;
        for (uint32_t current = static_cast<uint32_t>(axis) + 1; current < inputs[0].rank; current++)
            inner *= static_cast<uint64_t>(inputs[0].shape[current]);
        uint64_t width = static_cast<uint64_t>(inputs[0].shape[axis]);
        uint64_t groups = inputs[0].element_count / width;
        softmax_backward_kernel<<<static_cast<unsigned>((groups + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], inputs[1], outputs[0], inner, width);
    } else if (operation == OASP_AI_BACKEND_OP_CROSS_ENTROPY_BACKWARD && input_count >= 4 &&
               attributes_size == sizeof(oasp_ai_cross_entropy_attributes)) {
        const auto options = *static_cast<const oasp_ai_cross_entropy_attributes *>(attributes);
        oasp_ai_tensor_view empty{};
        cross_entropy_backward_kernel<<<static_cast<unsigned>((work + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], inputs[1], inputs[2], inputs[3],
            options.has_weight ? inputs[4] : empty, outputs[0], options);
    } else if (operation == OASP_AI_BACKEND_OP_CTC_LOSS_BACKWARD &&
               input_count == 7 && output_count == 2 &&
               attributes_size == sizeof(oasp_ai_ctc_attributes)) {
        const auto options = *static_cast<const oasp_ai_ctc_attributes *>(attributes);
        uint64_t batch = static_cast<uint64_t>(inputs[0].shape[1]);
        ctc_backward_kernel<<<static_cast<unsigned>((batch + threads - 1) / threads),
                              threads, 0, stream->stream>>>(
            inputs[0], inputs[1], inputs[2], inputs[3], inputs[4], inputs[5],
            inputs[6], outputs[0], outputs[1], options);
    } else if (operation == OASP_AI_BACKEND_OP_NLL_BACKWARD && input_count == 2 &&
               attributes_size == sizeof(oasp_ai_loss_attributes)) {
        uint64_t batch = static_cast<uint64_t>(inputs[0].shape[0]);
        nll_backward_kernel<<<static_cast<unsigned>((batch + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], inputs[1], outputs[0],
            static_cast<const oasp_ai_loss_attributes *>(attributes)->reduction);
    } else if (operation == OASP_AI_OP_ATTENTION && input_count >= 3) {
        oasp_ai_attention_attributes options = {0};
        if (attributes_size == sizeof(options))
            options = *static_cast<const oasp_ai_attention_attributes *>(attributes);
        oasp_ai_tensor_view empty{};
        uint64_t queries = static_cast<uint64_t>(inputs[0].shape[inputs[0].rank - 2]);
        uint64_t keys = static_cast<uint64_t>(inputs[1].shape[inputs[1].rank - 2]);
        uint64_t width = static_cast<uint64_t>(inputs[0].shape[inputs[0].rank - 1]);
        uint64_t batches = inputs[0].element_count / (queries * width);
        uint64_t score_count = batches * queries * keys;
        float *probabilities = nullptr;
        cuda_status = cudaMallocAsync(reinterpret_cast<void **>(&probabilities),
                                      static_cast<size_t>(score_count) * sizeof(*probabilities), stream->stream);
        if (cuda_status != cudaSuccess)
            return cuda_fail(error, "CudaAttentionWorkspace", cuda_status);
        bool cublas_layout = row_major_contiguous(inputs[0]) && row_major_contiguous(inputs[1]) &&
                             row_major_contiguous(inputs[2]) && row_major_contiguous(outputs[0]);
        if (cublas_layout) {
            float alpha = rsqrtf(static_cast<float>(width));
            float beta = 0;
            cublasStatus_t status = cublasSetStream(stream->cublas, stream->stream);
            if (status == CUBLAS_STATUS_SUCCESS)
                status = cublasSgemmStridedBatched(
                    stream->cublas, CUBLAS_OP_T, CUBLAS_OP_N, static_cast<int>(keys), static_cast<int>(queries),
                    static_cast<int>(width), &alpha, static_cast<const float *>(view_pointer(inputs[1])),
                    static_cast<int>(width), static_cast<long long>(keys * width),
                    static_cast<const float *>(view_pointer(inputs[0])), static_cast<int>(width),
                    static_cast<long long>(queries * width), &beta, probabilities, static_cast<int>(keys),
                    static_cast<long long>(queries * keys), static_cast<int>(batches));
            if (status != CUBLAS_STATUS_SUCCESS) {
                cudaFreeAsync(probabilities, stream->stream);
                return cublas_fail(error, "CublasAttentionScores", status);
            }
            if (options.causal || input_count > 3)
                attention_mask_kernel<<<static_cast<unsigned>((score_count + threads - 1) / threads), threads, 0, stream->stream>>>(
                    input_count > 3 ? inputs[3] : empty, probabilities, batches, queries, keys,
                    options.causal, input_count > 3);
        } else
            attention_scores_kernel<<<static_cast<unsigned>((score_count + threads - 1) / threads), threads, 0, stream->stream>>>(
                inputs[0], inputs[1], input_count > 3 ? inputs[3] : empty, probabilities, options.causal, input_count > 3);
        attention_softmax_kernel<<<static_cast<unsigned>((batches * queries + threads - 1) / threads), threads, 0, stream->stream>>>(
            probabilities, batches * queries, keys);
        if (cublas_layout) {
            float alpha = 1;
            float beta = 0;
            cublasStatus_t status = cublasSgemmStridedBatched(
                stream->cublas, CUBLAS_OP_N, CUBLAS_OP_N,
                static_cast<int>(inputs[2].shape[inputs[2].rank - 1]), static_cast<int>(queries), static_cast<int>(keys),
                &alpha, static_cast<const float *>(view_pointer(inputs[2])),
                static_cast<int>(inputs[2].shape[inputs[2].rank - 1]),
                static_cast<long long>(keys * inputs[2].shape[inputs[2].rank - 1]), probabilities,
                static_cast<int>(keys), static_cast<long long>(queries * keys), &beta,
                static_cast<float *>(view_pointer(outputs[0])),
                static_cast<int>(inputs[2].shape[inputs[2].rank - 1]),
                static_cast<long long>(queries * inputs[2].shape[inputs[2].rank - 1]), static_cast<int>(batches));
            if (status != CUBLAS_STATUS_SUCCESS) {
                cudaFreeAsync(probabilities, stream->stream);
                return cublas_fail(error, "CublasAttentionOutput", status);
            }
        } else
            attention_output_kernel<<<static_cast<unsigned>((work + threads - 1) / threads), threads, 0, stream->stream>>>(
                probabilities, inputs[2], outputs[0]);
        cuda_status = cudaFreeAsync(probabilities, stream->stream);
        if (cuda_status != cudaSuccess)
            return cuda_fail(error, "CudaAttentionWorkspaceFree", cuda_status);
    } else if (operation == OASP_AI_BACKEND_OP_ATTENTION_BACKWARD && input_count >= 4 && output_count == 3) {
        oasp_ai_attention_attributes options = {0};
        if (attributes_size == sizeof(options))
            options = *static_cast<const oasp_ai_attention_attributes *>(attributes);
        oasp_ai_tensor_view empty{};
        uint64_t queries = static_cast<uint64_t>(inputs[0].shape[inputs[0].rank - 2]);
        uint64_t keys = static_cast<uint64_t>(inputs[1].shape[inputs[1].rank - 2]);
        uint64_t width = static_cast<uint64_t>(inputs[0].shape[inputs[0].rank - 1]);
        uint64_t batches = inputs[0].element_count / (queries * width);
        uint64_t score_count = batches * queries * keys;
        float *probabilities = nullptr;
        float *score_gradient = nullptr;
        size_t workspace_bytes = static_cast<size_t>(score_count) * sizeof(*probabilities);
        cuda_status = cudaMallocAsync(reinterpret_cast<void **>(&probabilities), workspace_bytes, stream->stream);
        if (cuda_status == cudaSuccess)
            cuda_status = cudaMallocAsync(reinterpret_cast<void **>(&score_gradient), workspace_bytes, stream->stream);
        if (cuda_status != cudaSuccess) {
            if (probabilities)
                cudaFreeAsync(probabilities, stream->stream);
            return cuda_fail(error, "CudaAttentionBackwardWorkspace", cuda_status);
        }
        oasp_ai_tensor_view upstream = inputs[input_count - 1];
        bool cublas_layout = row_major_contiguous(inputs[0]) && row_major_contiguous(inputs[1]) &&
                             row_major_contiguous(inputs[2]) && row_major_contiguous(upstream) &&
                             row_major_contiguous(outputs[0]) && row_major_contiguous(outputs[1]) &&
                             row_major_contiguous(outputs[2]);
        cublasStatus_t cublas_status = CUBLAS_STATUS_SUCCESS;
        float alpha = 1;
        float beta = 0;
        if (cublas_layout) {
            float score_scale = rsqrtf(static_cast<float>(width));
            cublas_status = cublasSetStream(stream->cublas, stream->stream);
            if (cublas_status == CUBLAS_STATUS_SUCCESS)
                cublas_status = cublasSgemmStridedBatched(
                    stream->cublas, CUBLAS_OP_T, CUBLAS_OP_N, static_cast<int>(keys), static_cast<int>(queries),
                    static_cast<int>(width), &score_scale, static_cast<const float *>(view_pointer(inputs[1])),
                    static_cast<int>(width), static_cast<long long>(keys * width),
                    static_cast<const float *>(view_pointer(inputs[0])), static_cast<int>(width),
                    static_cast<long long>(queries * width), &beta, probabilities, static_cast<int>(keys),
                    static_cast<long long>(queries * keys), static_cast<int>(batches));
            if (cublas_status == CUBLAS_STATUS_SUCCESS && (options.causal || input_count > 4))
                attention_mask_kernel<<<static_cast<unsigned>((score_count + threads - 1) / threads), threads, 0, stream->stream>>>(
                    input_count > 4 ? inputs[3] : empty, probabilities, batches, queries, keys,
                    options.causal, input_count > 4);
        } else
            attention_scores_kernel<<<static_cast<unsigned>((score_count + threads - 1) / threads), threads, 0, stream->stream>>>(
                inputs[0], inputs[1], input_count > 4 ? inputs[3] : empty, probabilities, options.causal, input_count > 4);
        if (cublas_status != CUBLAS_STATUS_SUCCESS) {
            cudaFreeAsync(score_gradient, stream->stream);
            cudaFreeAsync(probabilities, stream->stream);
            return cublas_fail(error, "CublasAttentionBackwardScores", cublas_status);
        }
        attention_softmax_kernel<<<static_cast<unsigned>((batches * queries + threads - 1) / threads), threads, 0, stream->stream>>>(
            probabilities, batches * queries, keys);
        uint64_t value_width = static_cast<uint64_t>(inputs[2].shape[inputs[2].rank - 1]);
        if (cublas_layout)
            cublas_status = cublasSgemmStridedBatched(
                stream->cublas, CUBLAS_OP_T, CUBLAS_OP_N, static_cast<int>(keys), static_cast<int>(queries),
                static_cast<int>(value_width), &alpha, static_cast<const float *>(view_pointer(inputs[2])),
                static_cast<int>(value_width), static_cast<long long>(keys * value_width),
                static_cast<const float *>(view_pointer(upstream)), static_cast<int>(value_width),
                static_cast<long long>(queries * value_width), &beta, score_gradient, static_cast<int>(keys),
                static_cast<long long>(queries * keys), static_cast<int>(batches));
        else
            attention_probability_gradient_kernel<<<static_cast<unsigned>((score_count + threads - 1) / threads), threads, 0, stream->stream>>>(
                inputs[2], upstream, score_gradient, queries);
        if (cublas_status != CUBLAS_STATUS_SUCCESS) {
            cudaFreeAsync(score_gradient, stream->stream);
            cudaFreeAsync(probabilities, stream->stream);
            return cublas_fail(error, "CublasAttentionProbabilityGradient", cublas_status);
        }
        attention_score_gradient_kernel<<<static_cast<unsigned>((batches * queries + threads - 1) / threads), threads, 0, stream->stream>>>(
            probabilities, score_gradient, batches * queries, keys, rsqrtf(static_cast<float>(width)));
        if (cublas_layout && cublas_status == CUBLAS_STATUS_SUCCESS)
            cublas_status = cublasSgemmStridedBatched(
                stream->cublas, CUBLAS_OP_N, CUBLAS_OP_T, static_cast<int>(value_width), static_cast<int>(keys),
                static_cast<int>(queries), &alpha, static_cast<const float *>(view_pointer(upstream)),
                static_cast<int>(value_width), static_cast<long long>(queries * value_width), probabilities,
                static_cast<int>(keys), static_cast<long long>(queries * keys), &beta,
                static_cast<float *>(view_pointer(outputs[2])), static_cast<int>(value_width),
                static_cast<long long>(keys * value_width), static_cast<int>(batches));
        if (cublas_layout && cublas_status == CUBLAS_STATUS_SUCCESS)
            cublas_status = cublasSgemmStridedBatched(
                stream->cublas, CUBLAS_OP_N, CUBLAS_OP_N, static_cast<int>(width), static_cast<int>(queries),
                static_cast<int>(keys), &alpha, static_cast<const float *>(view_pointer(inputs[1])),
                static_cast<int>(width), static_cast<long long>(keys * width), score_gradient,
                static_cast<int>(keys), static_cast<long long>(queries * keys), &beta,
                static_cast<float *>(view_pointer(outputs[0])), static_cast<int>(width),
                static_cast<long long>(queries * width), static_cast<int>(batches));
        if (cublas_layout && cublas_status == CUBLAS_STATUS_SUCCESS)
            cublas_status = cublasSgemmStridedBatched(
                stream->cublas, CUBLAS_OP_N, CUBLAS_OP_T, static_cast<int>(width), static_cast<int>(keys),
                static_cast<int>(queries), &alpha, static_cast<const float *>(view_pointer(inputs[0])),
                static_cast<int>(width), static_cast<long long>(queries * width), score_gradient,
                static_cast<int>(keys), static_cast<long long>(queries * keys), &beta,
                static_cast<float *>(view_pointer(outputs[1])), static_cast<int>(width),
                static_cast<long long>(keys * width), static_cast<int>(batches));
        if (!cublas_layout) {
            attention_value_gradient_kernel<<<static_cast<unsigned>((outputs[2].element_count + threads - 1) / threads), threads, 0, stream->stream>>>(
                probabilities, upstream, outputs[2], queries);
            attention_query_gradient_kernel<<<static_cast<unsigned>((outputs[0].element_count + threads - 1) / threads), threads, 0, stream->stream>>>(
                score_gradient, inputs[1], outputs[0]);
            attention_key_gradient_kernel<<<static_cast<unsigned>((outputs[1].element_count + threads - 1) / threads), threads, 0, stream->stream>>>(
                score_gradient, inputs[0], outputs[1]);
        }
        if (cublas_status != CUBLAS_STATUS_SUCCESS) {
            cudaFreeAsync(score_gradient, stream->stream);
            cudaFreeAsync(probabilities, stream->stream);
            return cublas_fail(error, "CublasAttentionBackward", cublas_status);
        }
        cudaError_t gradient_free_status = cudaFreeAsync(score_gradient, stream->stream);
        cuda_status = cudaFreeAsync(probabilities, stream->stream);
        if (gradient_free_status != cudaSuccess || cuda_status != cudaSuccess)
            return cuda_fail(error, "CudaAttentionBackwardWorkspaceFree",
                             gradient_free_status != cudaSuccess ? gradient_free_status : cuda_status);
    } else if (operation == OASP_AI_BACKEND_OP_OPTIMIZER_STEP && input_count == 4 && output_count == 1 &&
               attributes_size == sizeof(oasp_ai_optimizer_attributes)) {
        optimizer_kernel<<<static_cast<unsigned>((work + threads - 1) / threads), threads, 0, stream->stream>>>(
            inputs[0], inputs[1], inputs[2], inputs[3], *static_cast<const oasp_ai_optimizer_attributes *>(attributes));
    } else if (operation == OASP_AI_BACKEND_OP_UNSCALE_AND_CHECK && input_count == 1 && output_count == 2 &&
               attributes_size == sizeof(oasp_ai_loss_scale_attributes) &&
               outputs[0].storage == inputs[0].storage && outputs[1].element_count == 1) {
        const auto *options = static_cast<const oasp_ai_loss_scale_attributes *>(attributes);
        unscale_and_check_kernel<<<static_cast<unsigned>((inputs[0].element_count + threads - 1) / threads),
                                   threads, 0, stream->stream>>>(
            inputs[0], static_cast<float *>(view_pointer(outputs[1])),
            static_cast<float>(options->inverse_scale));
    } else
        return fail(error, 5, "CudaExecuteOp", "operator is not implemented by the CUDA backend",
                    "Use a supported CUDA operator or disable StrictDevice for an explicit fallback.");
    cuda_status = cudaPeekAtLastError();
    if (cuda_status != cudaSuccess)
        return cuda_fail(error, "CudaKernelLaunch", cuda_status);
    if (!stream->capturing) {
        for (cuda_storage *storage : input_storages)
            if (mark_storage_used(storage, stream->stream, error))
                return -1;
        for (cuda_storage *storage : output_storages)
            if (mark_storage_used(storage, stream->stream, error))
                return -1;
    }
    return 0;
}

static int enumerate_devices(void *, oasp_ai_device_info *devices, size_t *count, oasp_ai_error *error) {
    if (!count)
        return -1;
    int available = 0;
    cudaError_t status = cudaGetDeviceCount(&available);
    if (status != cudaSuccess)
        return cuda_fail(error, "CudaEnumerate", status);
    if (!devices || *count < static_cast<size_t>(available)) {
        *count = static_cast<size_t>(available);
        return devices ? -1 : 0;
    }
    for (int ordinal = 0; ordinal < available; ordinal++) {
        cudaDeviceProp properties{};
        status = cudaGetDeviceProperties(&properties, ordinal);
        if (status != cudaSuccess)
            return cuda_fail(error, "CudaDeviceProperties", status);
        size_t free_bytes = 0;
        size_t total_bytes = 0;
        cudaSetDevice(ordinal);
        cudaMemGetInfo(&free_bytes, &total_bytes);
        devices[ordinal] = {sizeof(devices[ordinal]), static_cast<uint32_t>(ordinal),
                            {properties.name, std::strlen(properties.name)}, total_bytes, free_bytes,
                            static_cast<uint64_t>(properties.major * 10 + properties.minor)};
    }
    *count = static_cast<size_t>(available);
    return 0;
}

static int open_device(void *, uint32_t ordinal, const oasp_ai_device_options *options, oasp_ai_device *output, oasp_ai_error *error) {
    int available = 0;
    if (cudaGetDeviceCount(&available) != cudaSuccess || ordinal >= static_cast<uint32_t>(available))
        return fail(error, 5, "CudaOpenDevice", "CUDA device ordinal is unavailable", "Select an enumerated CUDA device.");
    cudaError_t status = cudaSetDevice(static_cast<int>(ordinal));
    if (status != cudaSuccess)
        return cuda_fail(error, "CudaSetDevice", status);
    status = cudaFree(nullptr);
    if (status != cudaSuccess)
        return cuda_fail(error, "CudaContext", status);
    cuda_device_state *device = new (std::nothrow) cuda_device_state{};
    if (!device)
        return fail(error, 7, "CudaOpenDevice", "unable to allocate CUDA device state", "Reduce host memory pressure.");
    device->ordinal = static_cast<int>(ordinal);
    device->memory_limit = options ? options->memory_limit : 0;
    status = cudaStreamCreateWithFlags(&device->allocation_stream, cudaStreamNonBlocking);
    if (status != cudaSuccess) {
        delete device;
        return cuda_fail(error, "CudaAllocationStream", status);
    }
    *output = reinterpret_cast<oasp_ai_device>(device);
    return 0;
}

static void close_device(oasp_ai_device handle) {
    cuda_device_state *device = reinterpret_cast<cuda_device_state *>(handle);
    if (!device)
        return;
    cudaSetDevice(device->ordinal);
    cudaDeviceSynchronize();
    cudaStreamDestroy(device->allocation_stream);
    for (cuda_storage *storage : device->cached) {
        if (storage->ready)
            cudaEventDestroy(storage->ready);
        cudaFree(storage->pointer);
        delete storage;
    }
    delete device;
}

static int query_capabilities(oasp_ai_device, oasp_ai_capability_query *query, oasp_ai_error *) {
    if (!query)
        return -1;
    bool operation = (query->op_id >= OASP_AI_OP_ADD && query->op_id <= OASP_AI_OP_MAXIMUM) ||
                     (query->op_id >= OASP_AI_OP_EXP && query->op_id <= OASP_AI_OP_SILU) ||
                     (query->op_id >= OASP_AI_OP_SUM && query->op_id <= OASP_AI_OP_NORM) ||
                     query->op_id == OASP_AI_OP_MATMUL || query->op_id == OASP_AI_OP_BATCHED_MATMUL ||
                     query->op_id == OASP_AI_OP_LAYER_NORM || query->op_id == OASP_AI_OP_RMS_NORM ||
                     query->op_id == OASP_AI_OP_CROSS_ENTROPY || query->op_id == OASP_AI_OP_NLL ||
                     query->op_id == OASP_AI_OP_CTC_LOSS ||
                     query->op_id == OASP_AI_OP_SOFTMAX ||
                     query->op_id == OASP_AI_OP_ATTENTION ||
                     query->op_id == OASP_AI_BACKEND_OP_UNSCALE_AND_CHECK ||
                     query->op_id == OASP_AI_BACKEND_OP_NLL_BACKWARD ||
                     query->op_id == OASP_AI_BACKEND_OP_CTC_LOSS_BACKWARD;
    query->supported = operation && query->op_version == 1 && query->input_dtype == OASP_AI_DTYPE_F32 &&
                       query->output_dtype == OASP_AI_DTYPE_F32;
    query->minimum_rank = 1;
    query->maximum_rank = OASP_AI_MAX_RANK;
    query->dynamic_shape = OASP_AI_DYNAMIC_RECOMPILE;
    query->deterministic = 1;
    query->training = 1;
    return 0;
}

static int allocate_storage(oasp_ai_device handle, const oasp_ai_alloc_desc *desc, oasp_ai_backend_storage *storage, oasp_ai_error *error) {
    cuda_device_state *device = reinterpret_cast<cuda_device_state *>(handle);
    if (!device || !desc || !storage || desc->memory_kind != OASP_AI_MEMORY_DEVICE)
        return fail(error, 5, "CudaAlloc", "invalid CUDA allocation request", "Request device memory from a valid CUDA device.");
    cudaError_t status = cudaSetDevice(device->ordinal);
    return status == cudaSuccess ? device_alloc(device, desc->bytes, storage, error) : cuda_fail(error, "CudaSetDevice", status);
}

static void free_storage(oasp_ai_device handle, oasp_ai_backend_storage storage) {
    device_release(reinterpret_cast<cuda_device_state *>(handle), storage);
}

static int copy_storage(oasp_ai_stream handle, const oasp_ai_copy_desc *desc, oasp_ai_event *event, oasp_ai_error *error) {
    cuda_stream_state *stream = reinterpret_cast<cuda_stream_state *>(handle);
    if (!stream || !desc)
        return -1;
    cudaSetDevice(stream->device->ordinal);
    const void *source = desc->source_host;
    void *destination = desc->destination_host;
    if (!source) {
        cuda_storage *storage = storage_from(desc->source);
        source = storage ? static_cast<unsigned char *>(storage->pointer) + desc->source_offset : nullptr;
    }
    if (!destination) {
        cuda_storage *storage = storage_from(desc->destination);
        destination = storage ? static_cast<unsigned char *>(storage->pointer) + desc->destination_offset : nullptr;
    }
    if (!source || !destination)
        return fail(error, 5, "CudaCopy", "copy source or destination is missing", "Pass valid host pointers or CUDA storage handles.");
    if (desc->source && wait_storage_ready(storage_from(desc->source), stream->stream, error))
        return -1;
    if (desc->destination && wait_storage_ready(storage_from(desc->destination), stream->stream, error))
        return -1;
    cudaMemcpyKind kind = desc->source_kind == OASP_AI_MEMORY_HOST ? cudaMemcpyHostToDevice
                          : desc->destination_kind == OASP_AI_MEMORY_HOST ? cudaMemcpyDeviceToHost
                                                                         : cudaMemcpyDeviceToDevice;
    cudaError_t status = cudaMemcpyAsync(destination, source, static_cast<size_t>(desc->bytes), kind, stream->stream);
    if (status != cudaSuccess)
        return cuda_fail(error, "CudaCopy", status);
    if (desc->source && mark_storage_used(storage_from(desc->source), stream->stream, error))
        return -1;
    if (desc->destination && mark_storage_used(storage_from(desc->destination), stream->stream, error))
        return -1;
    if (event) {
        cudaEvent_t completed;
        status = cudaEventCreateWithFlags(&completed, cudaEventDisableTiming);
        if (status == cudaSuccess)
            status = cudaEventRecord(completed, stream->stream);
        if (status != cudaSuccess)
            return cuda_fail(error, "CudaRecordEvent", status);
        *event = reinterpret_cast<oasp_ai_event>(completed);
    }
    return 0;
}

static int create_stream(oasp_ai_device handle, const oasp_ai_stream_desc *, oasp_ai_stream *output, oasp_ai_error *error) {
    cuda_device_state *device = reinterpret_cast<cuda_device_state *>(handle);
    cuda_stream_state *stream = new (std::nothrow) cuda_stream_state{};
    if (!device || !stream)
        return fail(error, 7, "CudaCreateStream", "unable to allocate CUDA stream state", "Reduce host resource pressure.");
    stream->device = device;
    stream->cublas_workspace_bytes = 8 * 1024 * 1024;
    cudaSetDevice(device->ordinal);
    cudaError_t status = cudaStreamCreateWithFlags(&stream->stream, cudaStreamNonBlocking);
    cublasStatus_t cublas_status = status == cudaSuccess ? cublasCreate(&stream->cublas) : CUBLAS_STATUS_NOT_INITIALIZED;
    if (cublas_status == CUBLAS_STATUS_SUCCESS)
        cublas_status = cublasLtCreate(&stream->cublas_lt);
    if (status == cudaSuccess && cublas_status == CUBLAS_STATUS_SUCCESS)
        status = cudaMalloc(&stream->cublas_workspace, stream->cublas_workspace_bytes);
    if (status != cudaSuccess || cublas_status != CUBLAS_STATUS_SUCCESS) {
        if (stream->cublas_lt)
            cublasLtDestroy(stream->cublas_lt);
        if (stream->cublas)
            cublasDestroy(stream->cublas);
        if (status == cudaSuccess)
            cudaStreamDestroy(stream->stream);
        delete stream;
        return status != cudaSuccess ? cuda_fail(error, "CudaCreateStream", status)
                                     : cublas_fail(error, "CublasCreate", cublas_status);
    }
    cublasSetStream(stream->cublas, stream->stream);
    cublasSetMathMode(stream->cublas, CUBLAS_TENSOR_OP_MATH);
#if defined(OASP_AI_WITH_CUDNN)
    if (!environment_disabled("OPENASP_AI_CUDA_CUDNN"))
        stream->cudnn = cuda_cudnn_create(stream->stream);
#endif
    cudaDeviceProp properties{};
    cudaGetDeviceProperties(&properties, device->ordinal);
    stream->fp16_tensor_cores = environment_enabled("OPENASP_AI_CUDA_FP16") && properties.major >= 7;
    stream->fp16_workspace_bytes = stream->fp16_tensor_cores
                                       ? environment_megabytes("OPENASP_AI_CUDA_FP16_WORKSPACE_MB", 32)
                                       : 0;
    if (stream->fp16_workspace_bytes) {
        status = cudaMalloc(reinterpret_cast<void **>(&stream->fp16_workspace), stream->fp16_workspace_bytes);
        if (status != cudaSuccess) {
#if defined(OASP_AI_WITH_CUDNN)
            cuda_cudnn_destroy(stream->cudnn);
#endif
            cublasLtDestroy(stream->cublas_lt);
            cublasDestroy(stream->cublas);
            cudaFree(stream->cublas_workspace);
            cudaStreamDestroy(stream->stream);
            delete stream;
            return cuda_fail(error, "CudaFp16Workspace", status);
        }
    }
    {
        std::lock_guard<std::mutex> lock(device->mutex);
        device->workspace_bytes += stream->cublas_workspace_bytes + stream->fp16_workspace_bytes;
    }
    *output = reinterpret_cast<oasp_ai_stream>(stream);
    return 0;
}

static void destroy_stream(oasp_ai_stream handle) {
    cuda_stream_state *stream = reinterpret_cast<cuda_stream_state *>(handle);
    if (!stream)
        return;
    cudaSetDevice(stream->device->ordinal);
    cudaStreamSynchronize(stream->stream);
    {
        std::lock_guard<std::mutex> lock(stream->device->mutex);
        stream->device->workspace_bytes -= std::min<uint64_t>(
            stream->device->workspace_bytes, stream->cublas_workspace_bytes + stream->fp16_workspace_bytes);
    }
    for (cublas_lt_plan *plan : stream->cublas_lt_plans)
        destroy_cublas_lt_plan(plan);
#if defined(OASP_AI_WITH_CUDNN)
    cuda_cudnn_destroy(stream->cudnn);
#endif
    cudaFree(stream->fp16_workspace);
    cudaFree(stream->cublas_workspace);
    cublasLtDestroy(stream->cublas_lt);
    cublasDestroy(stream->cublas);
    cudaStreamDestroy(stream->stream);
    delete stream;
}

static int record_event(oasp_ai_stream handle, oasp_ai_event *output, oasp_ai_error *error) {
    cuda_stream_state *stream = reinterpret_cast<cuda_stream_state *>(handle);
    if (!stream || !output)
        return -1;
    cudaEvent_t event;
    cudaError_t status = cudaEventCreateWithFlags(&event, cudaEventDisableTiming);
    if (status == cudaSuccess)
        status = cudaEventRecord(event, stream->stream);
    if (status != cudaSuccess)
        return cuda_fail(error, "CudaRecordEvent", status);
    *output = reinterpret_cast<oasp_ai_event>(event);
    return 0;
}

static int wait_event(oasp_ai_stream handle, oasp_ai_event event_handle, oasp_ai_error *error) {
    cuda_stream_state *stream = reinterpret_cast<cuda_stream_state *>(handle);
    cudaEvent_t event = reinterpret_cast<cudaEvent_t>(event_handle);
    if (!stream || !event)
        return -1;
    cudaError_t status = cudaStreamWaitEvent(stream->stream, event, 0);
    if (status == cudaSuccess)
        status = cudaEventDestroy(event);
    return status == cudaSuccess ? 0 : cuda_fail(error, "CudaWaitEvent", status);
}

static int synchronize(oasp_ai_stream handle, oasp_ai_error *error) {
    cuda_stream_state *stream = reinterpret_cast<cuda_stream_state *>(handle);
    if (!stream)
        return -1;
    cudaError_t status = cudaStreamSynchronize(stream->stream);
    return status == cudaSuccess ? 0 : cuda_fail(error, "CudaSynchronize", status);
}

static int execute_op(oasp_ai_stream handle, const oasp_ai_op_desc *op, oasp_ai_event *event, oasp_ai_error *error) {
    cuda_stream_state *stream = reinterpret_cast<cuda_stream_state *>(handle);
    if (!op)
        return -1;
    int status = execute_views(stream, op->op_id, op->inputs, op->input_count, op->outputs, op->output_count,
                               op->attributes, op->attributes_size, error);
    return !status && event ? record_event(handle, event, error) : status;
}

struct ir_reader {
    const unsigned char *cursor;
    const unsigned char *end;
    bool failed;
    uint32_t u32() {
        if (static_cast<size_t>(end - cursor) < 4) {
            failed = true;
            return 0;
        }
        uint32_t value = static_cast<uint32_t>(cursor[0]) | static_cast<uint32_t>(cursor[1]) << 8 |
                         static_cast<uint32_t>(cursor[2]) << 16 | static_cast<uint32_t>(cursor[3]) << 24;
        cursor += 4;
        return value;
    }
    uint64_t u64() {
        if (static_cast<size_t>(end - cursor) < 8) {
            failed = true;
            return 0;
        }
        uint64_t value = 0;
        for (unsigned index = 0; index < 8; index++)
            value |= static_cast<uint64_t>(cursor[index]) << (index * 8);
        cursor += 8;
        return value;
    }
    std::vector<unsigned char> bytes(size_t count) {
        if (static_cast<size_t>(end - cursor) < count) {
            failed = true;
            return {};
        }
        std::vector<unsigned char> value(cursor, cursor + count);
        cursor += count;
        return value;
    }
};

static int compile_graph(oasp_ai_device handle, const oasp_ai_graph_desc *graph, oasp_ai_executable *output, oasp_ai_error *error) {
    cuda_device_state *device = reinterpret_cast<cuda_device_state *>(handle);
    if (!device || !graph || !output || graph->core_ir_size < 28 ||
        std::memcmp(graph->core_ir, "OAIRABI1", 8))
        return fail(error, 5, "CudaCompileGraph", "invalid OAIR graph", "Pass verified OAIR ABI 1 Core IR.");
    ir_reader reader{static_cast<const unsigned char *>(graph->core_ir) + 8,
                     static_cast<const unsigned char *>(graph->core_ir) + graph->core_ir_size, false};
    uint32_t version = reader.u32();
    uint32_t value_count = reader.u32();
    uint32_t operation_count = reader.u32();
    uint32_t input_count = reader.u32();
    uint32_t output_count = reader.u32();
    if (version != 1 || value_count > 1000000 || operation_count > 1000000)
        return fail(error, 5, "CudaCompileGraph", "unsupported or oversized OAIR graph", "Use OAIR ABI 1 within configured graph limits.");
    cuda_executable_state *executable = new (std::nothrow) cuda_executable_state{};
    if (!executable)
        return -1;
    executable->device = device;
    executable->values.resize(value_count);
    for (uint32_t index = 0; index < value_count && !reader.failed; index++) {
        uint32_t id = reader.u32();
        graph_value &value = executable->values[index];
        value.view.struct_size = sizeof(value.view);
        value.view.dtype = static_cast<oasp_ai_dtype>(reader.u32());
        value.view.rank = reader.u32();
        uint32_t constant = reader.u32();
        if (id != index + 1 || value.view.rank > OASP_AI_MAX_RANK) {
            reader.failed = true;
            break;
        }
        value.view.element_count = 1;
        for (uint32_t axis = 0; axis < value.view.rank; axis++) {
            value.view.shape[axis] = static_cast<int64_t>(reader.u64());
            value.view.strides[axis] = static_cast<int64_t>(reader.u64());
            value.view.element_count *= static_cast<uint64_t>(value.view.shape[axis]);
        }
        uint64_t bytes = reader.u64();
        if (constant) {
            value.constant = reader.bytes(static_cast<size_t>(bytes));
            oasp_ai_alloc_desc allocation{sizeof(allocation), bytes, 256, OASP_AI_MEMORY_DEVICE, 0};
            if (!reader.failed && allocate_storage(handle, &allocation, &value.constant_storage, error))
                reader.failed = true;
            if (!reader.failed) {
                cuda_storage *storage = storage_from(value.constant_storage);
                cudaError_t status = cudaMemcpy(storage->pointer, value.constant.data(), static_cast<size_t>(bytes), cudaMemcpyHostToDevice);
                if (status != cudaSuccess)
                    reader.failed = true;
                value.view.storage = value.constant_storage;
            }
        } else if (bytes)
            reader.failed = true;
    }
    executable->operations.resize(operation_count);
    for (uint32_t index = 0; index < operation_count && !reader.failed; index++) {
        graph_operation &operation = executable->operations[index];
        operation.op_id = reader.u32();
        operation.op_version = static_cast<uint16_t>(reader.u32());
        operation.flags = static_cast<uint16_t>(reader.u32());
        operation.output = reader.u32();
        uint32_t count = reader.u32();
        uint64_t attribute_size = reader.u64();
        operation.inputs.resize(count);
        for (uint32_t input = 0; input < count; input++)
            operation.inputs[input] = reader.u32();
        operation.attributes = reader.bytes(static_cast<size_t>(attribute_size));
    }
    executable->inputs.resize(input_count);
    executable->outputs.resize(output_count);
    for (uint32_t &input : executable->inputs)
        input = reader.u32();
    for (uint32_t &graph_output : executable->outputs)
        graph_output = reader.u32();
    if (reader.failed || reader.cursor != reader.end) {
        for (graph_value &value : executable->values)
            if (value.constant_storage)
                free_storage(handle, value.constant_storage);
        delete executable;
        return fail(error, 5, "CudaCompileGraph", "malformed OAIR graph payload", "Regenerate the graph with a compatible OpenASP.AI Core.");
    }
    executable->intermediates.resize(executable->values.size());
    for (size_t index = 0; index < executable->values.size(); index++) {
        graph_value &value = executable->values[index];
        if (value.constant_storage)
            continue;
        uint64_t bytes = value.view.element_count * dtype_bytes(value.view.dtype);
        oasp_ai_alloc_desc allocation{sizeof(allocation), bytes, 256, OASP_AI_MEMORY_DEVICE, 0};
        if (!bytes || allocate_storage(handle, &allocation, &executable->intermediates[index], error)) {
            for (oasp_ai_backend_storage storage : executable->intermediates)
                if (storage)
                    free_storage(handle, storage);
            for (graph_value &cleanup : executable->values)
                if (cleanup.constant_storage)
                    free_storage(handle, cleanup.constant_storage);
            delete executable;
            return fail(error, 7, "CudaCompileGraph", "unable to allocate persistent CUDA graph storage",
                        "Reduce graph size or increase the configured device memory limit.");
        }
        value.view.storage = executable->intermediates[index];
    }
    *output = reinterpret_cast<oasp_ai_executable>(executable);
    return 0;
}

static int execute_graph(oasp_ai_stream handle, oasp_ai_executable executable_handle, const oasp_ai_graph_bindings *bindings,
                         oasp_ai_event *event, oasp_ai_error *error) {
    cuda_stream_state *stream = reinterpret_cast<cuda_stream_state *>(handle);
    cuda_executable_state *executable = reinterpret_cast<cuda_executable_state *>(executable_handle);
    if (!stream || !executable || !bindings || bindings->input_count != executable->inputs.size() ||
        bindings->output_count != executable->outputs.size())
        return fail(error, 5, "CudaExecuteGraph", "graph bindings do not match the executable", "Bind every declared graph input and output.");
    cudaError_t cuda_status = cudaSetDevice(stream->device->ordinal);
    if (cuda_status != cudaSuccess)
        return cuda_fail(error, "CudaGraphSetDevice", cuda_status);
    for (size_t index = 0; index < executable->inputs.size(); index++) {
        cuda_storage *source = storage_from(bindings->inputs[index]);
        cuda_storage *destination = storage_from(
            executable->intermediates[executable->inputs[index] - 1]);
        graph_value &value = executable->values[executable->inputs[index] - 1];
        if (!source || !destination || wait_storage_ready(source, stream->stream, error) ||
            wait_storage_ready(destination, stream->stream, error))
            return fail(error, 5, "CudaExecuteGraph", "graph input storage is invalid",
                        "Bind CUDA tensors allocated by this backend.");
        size_t bytes = static_cast<size_t>(value.view.element_count * dtype_bytes(value.view.dtype));
        cuda_status = cudaMemcpyAsync(destination->pointer, source->pointer, bytes,
                                      cudaMemcpyDeviceToDevice, stream->stream);
        if (cuda_status != cudaSuccess)
            return cuda_fail(error, "CudaGraphInputCopy", cuda_status);
    }
    int status = 0;
    if (!executable->cuda_graph_exec) {
        for (oasp_ai_backend_storage storage : executable->intermediates)
            if (storage && wait_storage_ready(storage_from(storage), stream->stream, error))
                return -1;
        cuda_status = cudaStreamBeginCapture(stream->stream, cudaStreamCaptureModeThreadLocal);
        if (cuda_status != cudaSuccess)
            return cuda_fail(error, "CudaGraphBeginCapture", cuda_status);
        stream->capturing = true;
        for (const graph_operation &operation : executable->operations) {
            graph_value &output_value = executable->values[operation.output - 1];
            std::vector<oasp_ai_tensor_view> inputs;
            inputs.reserve(operation.inputs.size());
            for (uint32_t input : operation.inputs)
                inputs.push_back(executable->values[input - 1].view);
            status = execute_views(stream, operation.op_id, inputs.data(), inputs.size(), &output_value.view, 1,
                                   operation.attributes.data(), operation.attributes.size(), error);
            if (status)
                break;
        }
        stream->capturing = false;
        cuda_status = cudaStreamEndCapture(stream->stream, &executable->cuda_graph);
        if (status || cuda_status != cudaSuccess || !executable->cuda_graph) {
            if (executable->cuda_graph) {
                cudaGraphDestroy(executable->cuda_graph);
                executable->cuda_graph = nullptr;
            }
            return status ? status : cuda_fail(error, "CudaGraphEndCapture", cuda_status);
        }
        cuda_status = cudaGraphInstantiate(&executable->cuda_graph_exec, executable->cuda_graph, 0);
        if (cuda_status != cudaSuccess)
            return cuda_fail(error, "CudaGraphInstantiate", cuda_status);
    }
    cuda_status = cudaGraphLaunch(executable->cuda_graph_exec, stream->stream);
    if (cuda_status != cudaSuccess)
        return cuda_fail(error, "CudaGraphLaunch", cuda_status);
    for (size_t index = 0; index < executable->outputs.size(); index++) {
        cuda_storage *source = storage_from(
            executable->intermediates[executable->outputs[index] - 1]);
        cuda_storage *destination = storage_from(bindings->outputs[index]);
        graph_value &value = executable->values[executable->outputs[index] - 1];
        if (!source || !destination || wait_storage_ready(destination, stream->stream, error))
            return fail(error, 5, "CudaExecuteGraph", "graph output storage is invalid",
                        "Bind CUDA tensors allocated by this backend.");
        size_t bytes = static_cast<size_t>(value.view.element_count * dtype_bytes(value.view.dtype));
        cuda_status = cudaMemcpyAsync(destination->pointer, source->pointer, bytes,
                                      cudaMemcpyDeviceToDevice, stream->stream);
        if (cuda_status != cudaSuccess)
            return cuda_fail(error, "CudaGraphOutputCopy", cuda_status);
        if (mark_storage_used(destination, stream->stream, error))
            return -1;
    }
    for (size_t index = 0; index < executable->inputs.size(); index++)
        if (mark_storage_used(storage_from(bindings->inputs[index]), stream->stream, error))
            return -1;
    for (oasp_ai_backend_storage storage : executable->intermediates)
        if (storage && mark_storage_used(storage_from(storage), stream->stream, error))
            return -1;
    return event ? record_event(handle, event, error) : 0;
}

static void destroy_executable(oasp_ai_executable handle) {
    cuda_executable_state *executable = reinterpret_cast<cuda_executable_state *>(handle);
    if (!executable)
        return;
    cudaSetDevice(executable->device->ordinal);
    cudaDeviceSynchronize();
    if (executable->cuda_graph_exec)
        cudaGraphExecDestroy(executable->cuda_graph_exec);
    if (executable->cuda_graph)
        cudaGraphDestroy(executable->cuda_graph);
    for (oasp_ai_backend_storage storage : executable->intermediates)
        if (storage)
            device_release(executable->device, storage);
    for (graph_value &value : executable->values)
        if (value.constant_storage)
            device_release(executable->device, value.constant_storage);
    delete executable;
}

static int memory_stats(oasp_ai_device handle, oasp_ai_memory_stats *stats, oasp_ai_error *) {
    cuda_device_state *device = reinterpret_cast<cuda_device_state *>(handle);
    if (!device || !stats)
        return -1;
    std::lock_guard<std::mutex> lock(device->mutex);
    *stats = {sizeof(*stats), device->live_bytes, device->reserved_bytes, device->peak_bytes,
              device->workspace_bytes, device->allocations};
    return 0;
}

static int trim_memory(oasp_ai_device handle, uint64_t target, oasp_ai_error *error) {
    cuda_device_state *device = reinterpret_cast<cuda_device_state *>(handle);
    if (!device)
        return -1;
    cudaSetDevice(device->ordinal);
    std::lock_guard<std::mutex> lock(device->mutex);
    while (device->reserved_bytes > target && !device->cached.empty()) {
        cuda_storage *storage = device->cached.back();
        device->cached.pop_back();
        cudaError_t status = cudaFree(storage->pointer);
        if (status != cudaSuccess)
            return cuda_fail(error, "CudaTrimMemory", status);
        device->reserved_bytes -= storage->bytes;
        if (storage->ready)
            cudaEventDestroy(storage->ready);
        delete storage;
    }
    return 0;
}

#if defined(OASP_AI_WITH_NCCL)
static int all_reduce(const oasp_ai_stream *handles, oasp_ai_tensor_view *tensors, size_t count,
                      oasp_ai_collective_reduction reduction, oasp_ai_error *error) {
    if (!handles || !tensors || count < 2)
        return -1;
    std::vector<int> ordinals(count);
    std::vector<ncclComm_t> communicators(count);
    for (size_t index = 0; index < count; index++) {
        cuda_stream_state *stream = reinterpret_cast<cuda_stream_state *>(handles[index]);
        cuda_storage *storage = storage_from(tensors[index].storage);
        if (!stream || !storage || tensors[index].dtype != OASP_AI_DTYPE_F32 ||
            !(tensors[index].flags & 1u))
            return fail(error, 5, "NcclAllReduce", "NCCL requires contiguous float32 CUDA tensors",
                        "Use equal contiguous float32 tensors on CUDA devices.");
        ordinals[index] = stream->device->ordinal;
        tensors[index].storage = reinterpret_cast<oasp_ai_backend_storage>(storage->pointer);
    }
    ncclResult_t status = ncclCommInitAll(communicators.data(), static_cast<int>(count), ordinals.data());
    if (status != ncclSuccess)
        return fail(error, 5, "NcclInit", ncclGetErrorString(status), "Check NCCL installation and peer access between CUDA devices.");
    ncclRedOp_t operation = reduction == OASP_AI_COLLECTIVE_MIN ? ncclMin :
                            reduction == OASP_AI_COLLECTIVE_MAX ? ncclMax : ncclSum;
    ncclGroupStart();
    for (size_t index = 0; index < count; index++) {
        cuda_stream_state *stream = reinterpret_cast<cuda_stream_state *>(handles[index]);
        void *pointer = view_pointer(tensors[index]);
        status = ncclAllReduce(pointer, pointer, tensors[index].element_count, ncclFloat, operation,
                               communicators[index], stream->stream);
        if (status != ncclSuccess)
            break;
    }
    ncclResult_t group_status = ncclGroupEnd();
    if (status == ncclSuccess)
        status = group_status;
    for (size_t index = 0; index < count; index++) {
        cuda_stream_state *stream = reinterpret_cast<cuda_stream_state *>(handles[index]);
        cudaSetDevice(stream->device->ordinal);
        if (status == ncclSuccess && reduction == OASP_AI_COLLECTIVE_MEAN) {
            float scale = 1.0f / static_cast<float>(count);
            cublasSetStream(stream->cublas, stream->stream);
            if (cublasSscal(stream->cublas, static_cast<int>(tensors[index].element_count), &scale,
                            static_cast<float *>(view_pointer(tensors[index])), 1) != CUBLAS_STATUS_SUCCESS)
                status = ncclUnhandledCudaError;
        }
        cudaStreamSynchronize(stream->stream);
        ncclCommDestroy(communicators[index]);
    }
    return status == ncclSuccess ? 0 :
           fail(error, 5, "NcclAllReduce", ncclGetErrorString(status), "Check NCCL topology, device health, and matching collective participants.");
}
#else
#define all_reduce nullptr
#endif

static int module_init(const oasp_ai_host_api *host, void **module) {
    (void)host;
    *module = nullptr;
    return 0;
}

static int worker_init(void *, void **worker, oasp_ai_error *) {
    *worker = nullptr;
    return 0;
}

static const oasp_ai_backend_api api = {
    sizeof(api), enumerate_devices, open_device, close_device, query_capabilities, allocate_storage, free_storage,
    copy_storage, create_stream, destroy_stream, record_event, wait_event, synchronize, execute_op, compile_graph,
    execute_graph, destroy_executable, memory_stats, trim_memory, all_reduce
};

static const oasp_ai_backend_descriptor descriptor = {
    sizeof(descriptor), OASP_AI_BACKEND_ABI_MAJOR, OASP_AI_BACKEND_ABI_MINOR, "cuda", "1.0.0", 0,
    module_init, nullptr, worker_init, nullptr, &api
};

extern "C" OASP_AI_BACKEND_EXPORT const oasp_ai_backend_descriptor *openasp_ai_backend(void) {
    return &descriptor;
}
