// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include <openasp/ai.h>

#include <oneapi/dnnl/dnnl.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

using dnnl::engine;
using dnnl::matmul;
using dnnl::memory;
using dnnl::primitive;
using dnnl::stream;

struct cpu_storage {
    void *pointer;
    uint64_t bytes;
};

struct cpu_device_state {
    engine dnnl_engine;
    uint64_t memory_limit;
    uint64_t live_bytes;
    uint64_t peak_bytes;
    uint64_t allocations;
    std::mutex mutex;

    explicit cpu_device_state(uint64_t limit)
        : dnnl_engine(engine::kind::cpu, 0), memory_limit(limit), live_bytes(0), peak_bytes(0), allocations(0) {}
};

struct matmul_cache_entry {
    int64_t rows;
    int64_t shared;
    int64_t columns;
    int64_t left_row_stride;
    int64_t left_column_stride;
    int64_t right_row_stride;
    int64_t right_column_stride;
    memory::desc left_desc;
    memory::desc right_desc;
    memory::desc output_desc;
    primitive operation;

    matmul_cache_entry(cpu_device_state *device, int64_t row_count, int64_t shared_count, int64_t column_count,
                       int64_t left_rows, int64_t left_columns, int64_t right_rows, int64_t right_columns)
        : rows(row_count), shared(shared_count), columns(column_count), left_row_stride(left_rows),
          left_column_stride(left_columns), right_row_stride(right_rows), right_column_stride(right_columns),
          left_desc(memory::dims{rows, shared}, memory::data_type::f32,
                    memory::dims{left_row_stride, left_column_stride}),
          right_desc(memory::dims{shared, columns}, memory::data_type::f32,
                     memory::dims{right_row_stride, right_column_stride}),
          output_desc(memory::dims{rows, columns}, memory::data_type::f32, memory::dims{columns, 1}),
          operation(matmul::primitive_desc(device->dnnl_engine, left_desc, right_desc, output_desc)) {}

    bool matches(int64_t row_count, int64_t shared_count, int64_t column_count, int64_t left_rows,
                 int64_t left_columns, int64_t right_rows, int64_t right_columns) const {
        return rows == row_count && shared == shared_count && columns == column_count &&
               left_row_stride == left_rows && left_column_stride == left_columns &&
               right_row_stride == right_rows && right_column_stride == right_columns;
    }
};

struct cpu_stream_state {
    cpu_device_state *device;
    stream dnnl_stream;
    std::vector<matmul_cache_entry> matmul_cache;

    explicit cpu_stream_state(cpu_device_state *owner) : device(owner), dnnl_stream(owner->dnnl_engine) {}
};

static int fail(oasp_ai_error *error, int code, const char *operation, const char *message, const char *suggestion) {
    if (error)
        *error = {sizeof(*error), code, 0, operation, message, suggestion};
    return -1;
}

static int dnnl_fail(oasp_ai_error *error, const char *operation, const dnnl::error &exception) {
    if (error)
        *error = {sizeof(*error), 5, static_cast<int32_t>(exception.status), operation, exception.what(),
                  "Check tensor dimensions, strides, and the installed oneDNN runtime."};
    return -1;
}

static cpu_storage *storage_from(oasp_ai_backend_storage handle) {
    return reinterpret_cast<cpu_storage *>(handle);
}

static void *view_pointer(const oasp_ai_tensor_view &view) {
    cpu_storage *storage = storage_from(view.storage);
    return storage ? static_cast<unsigned char *>(storage->pointer) + view.byte_offset : nullptr;
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

static uint64_t physical_index(const oasp_ai_tensor_view &view, uint64_t logical) {
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

static uint64_t broadcast_index(const oasp_ai_tensor_view &input, const oasp_ai_tensor_view &output, uint64_t logical) {
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

static bool row_major_contiguous(const oasp_ai_tensor_view &view) {
    int64_t stride = 1;
    for (uint32_t axis = view.rank; axis; axis--) {
        if (view.strides[axis - 1] != stride)
            return false;
        stride *= view.shape[axis - 1];
    }
    return true;
}

static int dnnl_matmul_raw(cpu_stream_state *state, const float *left, const float *right, float *output,
                           int64_t rows, int64_t shared, int64_t columns, int64_t left_row_stride,
                           int64_t left_column_stride, int64_t right_row_stride, int64_t right_column_stride,
                           oasp_ai_error *error) {
    try {
        matmul_cache_entry *cached = nullptr;
        for (matmul_cache_entry &entry : state->matmul_cache) {
            if (entry.matches(rows, shared, columns, left_row_stride, left_column_stride,
                              right_row_stride, right_column_stride)) {
                cached = &entry;
                break;
            }
        }
        if (!cached) {
            static const size_t maximum_cached_matmuls = 64;
            if (state->matmul_cache.size() == maximum_cached_matmuls)
                state->matmul_cache.erase(state->matmul_cache.begin());
            state->matmul_cache.emplace_back(state->device, rows, shared, columns, left_row_stride,
                                             left_column_stride, right_row_stride, right_column_stride);
            cached = &state->matmul_cache.back();
        }
        memory left_memory(cached->left_desc, state->device->dnnl_engine, const_cast<float *>(left));
        memory right_memory(cached->right_desc, state->device->dnnl_engine, const_cast<float *>(right));
        memory output_memory(cached->output_desc, state->device->dnnl_engine, output);
        cached->operation.execute(state->dnnl_stream, {
            {DNNL_ARG_SRC, left_memory},
            {DNNL_ARG_WEIGHTS, right_memory},
            {DNNL_ARG_DST, output_memory},
        });
        state->dnnl_stream.wait();
        return 0;
    } catch (const dnnl::error &exception) {
        return dnnl_fail(error, "OneDnnMatMul", exception);
    }
}

static int execute_matmul(cpu_stream_state *state, const oasp_ai_tensor_view &left,
                          const oasp_ai_tensor_view &right, oasp_ai_tensor_view &output,
                          oasp_ai_error *error) {
    uint64_t rows = static_cast<uint64_t>(left.shape[left.rank - 2]);
    uint64_t shared = static_cast<uint64_t>(left.shape[left.rank - 1]);
    uint64_t columns = static_cast<uint64_t>(right.shape[right.rank - 1]);
    uint64_t batches = output.element_count / (rows * columns);
    uint64_t left_matrix = rows * shared;
    uint64_t right_matrix = shared * columns;
    uint64_t output_matrix = rows * columns;
    const float *left_data = static_cast<const float *>(view_pointer(left));
    const float *right_data = static_cast<const float *>(view_pointer(right));
    float *output_data = static_cast<float *>(view_pointer(output));
    for (uint64_t batch = 0; batch < batches; batch++) {
        uint64_t left_logical = left.element_count == left_matrix ? 0 : batch * left_matrix;
        uint64_t right_logical = right.element_count == right_matrix ? 0 : batch * right_matrix;
        uint64_t output_logical = batch * output_matrix;
        if (dnnl_matmul_raw(state, left_data + physical_index(left, left_logical),
                            right_data + physical_index(right, right_logical),
                            output_data + physical_index(output, output_logical),
                            static_cast<int64_t>(rows), static_cast<int64_t>(shared),
                            static_cast<int64_t>(columns), left.strides[left.rank - 2],
                            left.strides[left.rank - 1], right.strides[right.rank - 2],
                            right.strides[right.rank - 1], error))
            return -1;
    }
    return 0;
}

static float binary_derivative(uint32_t operation, bool left, float first, float second) {
    if (operation == OASP_AI_OP_ADD)
        return 1;
    if (operation == OASP_AI_OP_SUB)
        return left ? 1.0f : -1.0f;
    if (operation == OASP_AI_OP_MUL)
        return left ? second : first;
    if (operation == OASP_AI_OP_DIV)
        return left ? 1.0f / second : -first / (second * second);
    if (operation == OASP_AI_OP_POW)
        return left ? second * std::pow(first, second - 1.0f) : std::pow(first, second) * std::log(first);
    if (operation == OASP_AI_OP_MAXIMUM)
        return first == second ? 0.5f : static_cast<float>(left ? first > second : second > first);
    return 0;
}

static int execute_binary(uint32_t operation, const oasp_ai_tensor_view &left,
                          const oasp_ai_tensor_view &right, oasp_ai_tensor_view &output) {
    const float *left_data = static_cast<const float *>(view_pointer(left));
    const float *right_data = static_cast<const float *>(view_pointer(right));
    float *output_data = static_cast<float *>(view_pointer(output));
#pragma omp parallel for schedule(static)
    for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(output.element_count); signed_index++) {
        uint64_t index = static_cast<uint64_t>(signed_index);
        float first = left_data[physical_index(left, broadcast_index(left, output, index))];
        float second = right_data[physical_index(right, broadcast_index(right, output, index))];
        float value = 0;
        switch (operation) {
        case OASP_AI_OP_ADD: value = first + second; break;
        case OASP_AI_OP_SUB: value = first - second; break;
        case OASP_AI_OP_MUL: value = first * second; break;
        case OASP_AI_OP_DIV: value = first / second; break;
        case OASP_AI_OP_POW: value = std::pow(first, second); break;
        case OASP_AI_OP_MAXIMUM: value = std::max(first, second); break;
        }
        output_data[physical_index(output, index)] = value;
    }
    return 0;
}

static int execute_unary(uint32_t operation, const oasp_ai_tensor_view &input,
                         oasp_ai_tensor_view &output, double argument) {
    const float *input_data = static_cast<const float *>(view_pointer(input));
    float *output_data = static_cast<float *>(view_pointer(output));
#pragma omp parallel for schedule(static)
    for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(output.element_count); signed_index++) {
        uint64_t index = static_cast<uint64_t>(signed_index);
        float value = input_data[physical_index(input, index)];
        switch (operation) {
        case OASP_AI_OP_EXP: value = std::exp(value); break;
        case OASP_AI_OP_LOG: value = std::log(value); break;
        case OASP_AI_OP_SQRT: value = std::sqrt(value); break;
        case OASP_AI_OP_RELU: value = std::max(value, 0.0f); break;
        case OASP_AI_OP_LEAKY_RELU: value = value > 0 ? value : static_cast<float>(argument) * value; break;
        case OASP_AI_OP_SIGMOID: value = 1.0f / (1.0f + std::exp(-value)); break;
        case OASP_AI_OP_TANH: value = std::tanh(value); break;
        case OASP_AI_OP_GELU:
            value = 0.5f * value * (1.0f + std::tanh(0.7978845608f * (value + 0.044715f * value * value * value)));
            break;
        case OASP_AI_OP_SILU: value = value / (1.0f + std::exp(-value)); break;
        case OASP_AI_OP_TEMPERATURE: value /= static_cast<float>(argument); break;
        }
        output_data[physical_index(output, index)] = value;
    }
    return 0;
}

static int execute_reduce(uint32_t operation, const oasp_ai_tensor_view &input,
                          oasp_ai_tensor_view &output, int32_t axis) {
    const float *source = static_cast<const float *>(view_pointer(input));
    float *destination = static_cast<float *>(view_pointer(output));
    uint64_t reduction = axis < 0 ? input.element_count : static_cast<uint64_t>(input.shape[axis]);
    uint64_t inner = 1;
    if (axis >= 0)
        for (uint32_t current = static_cast<uint32_t>(axis) + 1; current < input.rank; current++)
            inner *= static_cast<uint64_t>(input.shape[current]);
#pragma omp parallel for schedule(static)
    for (int64_t signed_output = 0; signed_output < static_cast<int64_t>(output.element_count); signed_output++) {
        uint64_t output_index = static_cast<uint64_t>(signed_output);
        float aggregate = operation == OASP_AI_OP_MIN ? INFINITY : operation == OASP_AI_OP_MAX ? -INFINITY : 0.0f;
        for (uint64_t item = 0; item < reduction; item++) {
            uint64_t index = axis < 0 ? item : (output_index / inner) * reduction * inner + item * inner + output_index % inner;
            float value = source[physical_index(input, index)];
            if (operation == OASP_AI_OP_MAX)
                aggregate = std::max(aggregate, value);
            else if (operation == OASP_AI_OP_MIN)
                aggregate = std::min(aggregate, value);
            else if (operation == OASP_AI_OP_NORM)
                aggregate += value * value;
            else
                aggregate += value;
        }
        if (operation == OASP_AI_OP_MEAN)
            aggregate /= static_cast<float>(reduction);
        else if (operation == OASP_AI_OP_NORM)
            aggregate = std::sqrt(aggregate);
        destination[physical_index(output, output_index)] = aggregate;
    }
    return 0;
}

static int execute_softmax(const oasp_ai_tensor_view &input, oasp_ai_tensor_view &output, int32_t axis) {
    uint64_t inner = 1;
    for (uint32_t current = static_cast<uint32_t>(axis) + 1; current < input.rank; current++)
        inner *= static_cast<uint64_t>(input.shape[current]);
    uint64_t width = static_cast<uint64_t>(input.shape[axis]);
    uint64_t groups = input.element_count / width;
    const float *source = static_cast<const float *>(view_pointer(input));
    float *destination = static_cast<float *>(view_pointer(output));
#pragma omp parallel for schedule(static)
    for (int64_t signed_group_position = 0; signed_group_position < static_cast<int64_t>(groups); signed_group_position++) {
        uint64_t group_position = static_cast<uint64_t>(signed_group_position);
        uint64_t group = group_position / inner;
        uint64_t position = group_position % inner;
        float maximum = -INFINITY;
        for (uint64_t item = 0; item < width; item++)
            maximum = std::max(maximum, source[physical_index(input, group * width * inner + item * inner + position)]);
        float total = 0;
        for (uint64_t item = 0; item < width; item++)
            total += std::exp(source[physical_index(input, group * width * inner + item * inner + position)] - maximum);
        for (uint64_t item = 0; item < width; item++) {
            uint64_t index = group * width * inner + item * inner + position;
            destination[physical_index(output, index)] = std::exp(source[physical_index(input, index)] - maximum) / total;
        }
    }
    return 0;
}

static int execute_cross_entropy(const oasp_ai_tensor_view &logits, const oasp_ai_tensor_view &labels,
                                 const oasp_ai_tensor_view *weight, oasp_ai_tensor_view &output,
                                 oasp_ai_tensor_view &normalizer,
                                 const oasp_ai_cross_entropy_attributes &options) {
    const float *values = static_cast<const float *>(view_pointer(logits));
    const int64_t *targets = static_cast<const int64_t *>(view_pointer(labels));
    const float *weights = weight ? static_cast<const float *>(view_pointer(*weight)) : nullptr;
    float *destination = static_cast<float *>(view_pointer(output));
    uint64_t batch = static_cast<uint64_t>(logits.shape[0]);
    uint64_t classes = static_cast<uint64_t>(logits.shape[1]);
    double total_loss = 0;
    double denominator = 0;
#pragma omp parallel for reduction(+ : total_loss, denominator) schedule(static)
    for (int64_t signed_row = 0; signed_row < static_cast<int64_t>(batch); signed_row++) {
        uint64_t row = static_cast<uint64_t>(signed_row);
        int64_t label = targets[physical_index(labels, row)];
        if (label == options.ignore_index) {
            if (options.reduction == OASP_AI_REDUCTION_NONE)
                destination[physical_index(output, row)] = 0;
            continue;
        }
        float maximum = -INFINITY;
        for (uint64_t column = 0; column < classes; column++)
            maximum = std::max(maximum, values[physical_index(logits, row * classes + column)]);
        float total = 0;
        for (uint64_t column = 0; column < classes; column++)
            total += std::exp(values[physical_index(logits, row * classes + column)] - maximum);
        float log_sum_exp = std::log(total) + maximum;
        float target_weight = weights ? weights[static_cast<uint64_t>(label)] : 1.0f;
        float nll = (log_sum_exp -
                     values[physical_index(logits, row * classes + static_cast<uint64_t>(label))]) *
                    target_weight;
        float smooth = 0;
        for (uint64_t column = 0; column < classes; column++)
            smooth += (log_sum_exp - values[physical_index(logits, row * classes + column)]) *
                      (weights ? weights[column] : 1.0f);
        float row_loss = static_cast<float>(1 - options.label_smoothing) * nll +
                         static_cast<float>(options.label_smoothing) * smooth /
                             static_cast<float>(classes);
        if (options.reduction == OASP_AI_REDUCTION_NONE)
            destination[physical_index(output, row)] = row_loss;
        else
            total_loss += row_loss;
        denominator += target_weight;
    }
    if (options.reduction != OASP_AI_REDUCTION_NONE)
        destination[physical_index(output, 0)] = static_cast<float>(
            options.reduction == OASP_AI_REDUCTION_MEAN ? total_loss / denominator : total_loss);
    static_cast<float *>(view_pointer(normalizer))[physical_index(normalizer, 0)] =
        options.reduction == OASP_AI_REDUCTION_MEAN ? static_cast<float>(denominator) : 1.0f;
    return 0;
}

static float ctc_log_add(float left, float right) {
    if (left == -INFINITY)
        return right;
    if (right == -INFINITY)
        return left;
    float maximum = std::max(left, right);
    return maximum + std::log1p(std::exp(-std::fabs(left - right)));
}

static int execute_ctc_forward(const oasp_ai_tensor_view &log_probabilities,
                               const oasp_ai_tensor_view &targets,
                               const oasp_ai_tensor_view &input_lengths,
                               const oasp_ai_tensor_view &target_lengths,
                               oasp_ai_tensor_view &output, oasp_ai_tensor_view &alpha,
                               oasp_ai_tensor_view &per_sample,
                               const oasp_ai_ctc_attributes &options) {
    uint64_t maximum_time = static_cast<uint64_t>(log_probabilities.shape[0]);
    uint64_t batch_count = static_cast<uint64_t>(log_probabilities.shape[1]);
    uint64_t classes = static_cast<uint64_t>(log_probabilities.shape[2]);
    uint64_t maximum_target_length = static_cast<uint64_t>(targets.shape[1]);
    uint64_t maximum_states = static_cast<uint64_t>(alpha.shape[2]);
    const float *log_values = static_cast<const float *>(view_pointer(log_probabilities));
    const int64_t *target_values = static_cast<const int64_t *>(view_pointer(targets));
    const int64_t *input_length_values = static_cast<const int64_t *>(view_pointer(input_lengths));
    const int64_t *target_length_values = static_cast<const int64_t *>(view_pointer(target_lengths));
    float *alpha_values = static_cast<float *>(view_pointer(alpha));
    float *sample_values = static_cast<float *>(view_pointer(per_sample));
#pragma omp parallel for schedule(static)
    for (int64_t signed_batch = 0; signed_batch < static_cast<int64_t>(batch_count); signed_batch++) {
        uint64_t batch = static_cast<uint64_t>(signed_batch);
        for (uint64_t index = 0; index < maximum_time * maximum_states; index++)
            alpha_values[physical_index(
                alpha, batch * maximum_time * maximum_states + index)] = -INFINITY;
        int64_t input_length = input_length_values[physical_index(input_lengths, batch)];
        int64_t target_length = target_length_values[physical_index(target_lengths, batch)];
        uint64_t repeats = 0;
        bool valid = input_length > 0 && input_length <= static_cast<int64_t>(maximum_time) &&
                     target_length >= 0 &&
                     target_length <= static_cast<int64_t>(maximum_target_length);
        for (int64_t position = 0; valid && position < target_length; position++) {
            int64_t label = target_values[physical_index(
                targets, batch * maximum_target_length + static_cast<uint64_t>(position))];
            valid = label >= 0 && label < static_cast<int64_t>(classes) &&
                    label != options.blank;
            if (position) {
                int64_t previous = target_values[physical_index(
                    targets, batch * maximum_target_length +
                                 static_cast<uint64_t>(position - 1))];
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
                log_values[physical_index(log_probabilities,
                                          CTC_LOGP_INDEX(0, options.blank))];
            if (target_length) {
                int64_t label =
                    target_values[physical_index(targets, batch * maximum_target_length)];
                alpha_values[physical_index(alpha, CTC_ALPHA_INDEX(0, 1))] =
                    log_values[physical_index(log_probabilities,
                                              CTC_LOGP_INDEX(0, label))];
            }
            for (int64_t time = 1; time < input_length; time++)
                for (uint64_t state = 0; state < states; state++) {
                    int64_t label =
                        state % 2
                            ? target_values[physical_index(
                                  targets, batch * maximum_target_length + state / 2)]
                            : options.blank;
                    float value = alpha_values[physical_index(
                        alpha, CTC_ALPHA_INDEX(time - 1, state))];
                    if (state)
                        value = ctc_log_add(
                            value, alpha_values[physical_index(
                                       alpha, CTC_ALPHA_INDEX(time - 1, state - 1))]);
                    if (state > 1 && label != options.blank) {
                        int64_t previous =
                            state > 2
                                ? target_values[physical_index(
                                      targets, batch * maximum_target_length +
                                                   state / 2 - 1)]
                                : options.blank;
                        if (label != previous)
                            value = ctc_log_add(
                                value, alpha_values[physical_index(
                                           alpha,
                                           CTC_ALPHA_INDEX(time - 1, state - 2))]);
                    }
                    value += log_values[physical_index(
                        log_probabilities, CTC_LOGP_INDEX(time, label))];
                    alpha_values[physical_index(
                        alpha, CTC_ALPHA_INDEX(time, state))] = value;
                }
            float likelihood = alpha_values[physical_index(
                alpha, CTC_ALPHA_INDEX(input_length - 1, states - 1))];
            if (states > 1)
                likelihood = ctc_log_add(
                    likelihood, alpha_values[physical_index(
                                    alpha,
                                    CTC_ALPHA_INDEX(input_length - 1, states - 2))]);
            sample_loss = -likelihood;
#undef CTC_LOGP_INDEX
#undef CTC_ALPHA_INDEX
        }
        sample_values[physical_index(per_sample, batch)] = sample_loss;
    }
    float aggregate = 0;
    float *loss = static_cast<float *>(view_pointer(output));
    for (uint64_t batch = 0; batch < batch_count; batch++) {
        float sample_loss = sample_values[physical_index(per_sample, batch)];
        float reduced = options.zero_infinity && !std::isfinite(sample_loss)
                            ? 0
                            : sample_loss;
        if (options.reduction == OASP_AI_REDUCTION_NONE)
            loss[physical_index(output, batch)] = reduced;
        else if (options.reduction == OASP_AI_REDUCTION_MEAN) {
            int64_t target_length =
                target_length_values[physical_index(target_lengths, batch)];
            aggregate +=
                reduced / static_cast<float>(target_length > 0 ? target_length : 1);
        } else
            aggregate += reduced;
    }
    if (options.reduction != OASP_AI_REDUCTION_NONE)
        loss[physical_index(output, 0)] =
            options.reduction == OASP_AI_REDUCTION_MEAN
                ? aggregate / static_cast<float>(batch_count)
                : aggregate;
    return 0;
}

static int execute_ctc_backward(const oasp_ai_tensor_view *inputs,
                                oasp_ai_tensor_view *outputs,
                                const oasp_ai_ctc_attributes &options) {
    const oasp_ai_tensor_view &log_probabilities = inputs[0];
    const oasp_ai_tensor_view &targets = inputs[1];
    const oasp_ai_tensor_view &input_lengths = inputs[2];
    const oasp_ai_tensor_view &target_lengths = inputs[3];
    const oasp_ai_tensor_view &alpha = inputs[4];
    const oasp_ai_tensor_view &per_sample = inputs[5];
    const oasp_ai_tensor_view &upstream = inputs[6];
    oasp_ai_tensor_view &gradient = outputs[0];
    oasp_ai_tensor_view &beta = outputs[1];
    uint64_t maximum_time = static_cast<uint64_t>(log_probabilities.shape[0]);
    uint64_t batch_count = static_cast<uint64_t>(log_probabilities.shape[1]);
    uint64_t classes = static_cast<uint64_t>(log_probabilities.shape[2]);
    uint64_t maximum_target_length = static_cast<uint64_t>(targets.shape[1]);
    uint64_t maximum_states = static_cast<uint64_t>(alpha.shape[2]);
    const float *log_values = static_cast<const float *>(view_pointer(log_probabilities));
    const int64_t *target_values = static_cast<const int64_t *>(view_pointer(targets));
    const int64_t *input_length_values = static_cast<const int64_t *>(view_pointer(input_lengths));
    const int64_t *target_length_values =
        static_cast<const int64_t *>(view_pointer(target_lengths));
    const float *alpha_values = static_cast<const float *>(view_pointer(alpha));
    const float *sample_values = static_cast<const float *>(view_pointer(per_sample));
    const float *upstream_values = static_cast<const float *>(view_pointer(upstream));
    float *gradient_values = static_cast<float *>(view_pointer(gradient));
    float *beta_values = static_cast<float *>(view_pointer(beta));
#pragma omp parallel for schedule(static)
    for (int64_t signed_batch = 0;
         signed_batch < static_cast<int64_t>(batch_count); signed_batch++) {
        uint64_t batch = static_cast<uint64_t>(signed_batch);
        for (uint64_t time = 0; time < maximum_time; time++)
            for (uint64_t class_index = 0; class_index < classes; class_index++)
                gradient_values[physical_index(
                    gradient,
                    (time * batch_count + batch) * classes + class_index)] = 0;
        for (uint64_t index = 0; index < maximum_time * maximum_states; index++)
            beta_values[physical_index(
                beta, batch * maximum_time * maximum_states + index)] = -INFINITY;
        int64_t input_length =
            input_length_values[physical_index(input_lengths, batch)];
        int64_t target_length =
            target_length_values[physical_index(target_lengths, batch)];
        float sample_loss = sample_values[physical_index(per_sample, batch)];
        if (!std::isfinite(sample_loss) || input_length <= 0)
            continue;
        uint64_t states = static_cast<uint64_t>(target_length) * 2 + 1;
#define CTC_BETA_INDEX(t, s) ((batch * maximum_time + static_cast<uint64_t>(t)) * maximum_states + static_cast<uint64_t>(s))
#define CTC_ALPHA_INDEX(t, s) ((batch * maximum_time + static_cast<uint64_t>(t)) * maximum_states + static_cast<uint64_t>(s))
#define CTC_LOGP_INDEX(t, c) ((static_cast<uint64_t>(t) * batch_count + batch) * classes + static_cast<uint64_t>(c))
        beta_values[physical_index(
            beta, CTC_BETA_INDEX(input_length - 1, states - 1))] = 0;
        if (states > 1)
            beta_values[physical_index(
                beta, CTC_BETA_INDEX(input_length - 1, states - 2))] = 0;
        for (int64_t time = input_length - 2; time >= 0; time--)
            for (uint64_t state = 0; state < states; state++) {
                float value = -INFINITY;
                for (uint64_t advance = 0; advance < 3; advance++) {
                    uint64_t next = state + advance;
                    if (next >= states)
                        continue;
                    int64_t next_label =
                        next % 2
                            ? target_values[physical_index(
                                  targets, batch * maximum_target_length + next / 2)]
                            : options.blank;
                    int64_t current_label =
                        state % 2
                            ? target_values[physical_index(
                                  targets, batch * maximum_target_length + state / 2)]
                            : options.blank;
                    if (advance == 2 &&
                        (next_label == options.blank || next_label == current_label))
                        continue;
                    value = ctc_log_add(
                        value,
                        beta_values[physical_index(
                            beta, CTC_BETA_INDEX(time + 1, next))] +
                            log_values[physical_index(
                                log_probabilities,
                                CTC_LOGP_INDEX(time + 1, next_label))]);
                }
                beta_values[physical_index(
                    beta, CTC_BETA_INDEX(time, state))] = value;
            }
        float scale = upstream_values[physical_index(
            upstream,
            options.reduction == OASP_AI_REDUCTION_NONE ? batch : 0)];
        if (options.reduction == OASP_AI_REDUCTION_MEAN)
            scale /= static_cast<float>(batch_count) *
                     static_cast<float>(target_length > 0 ? target_length : 1);
        for (int64_t time = 0; time < input_length; time++) {
            for (uint64_t class_index = 0; class_index < classes; class_index++)
                gradient_values[physical_index(
                    gradient, CTC_LOGP_INDEX(time, class_index))] =
                    std::exp(log_values[physical_index(
                        log_probabilities,
                        CTC_LOGP_INDEX(time, class_index))]) *
                    scale;
            for (uint64_t state = 0; state < states; state++) {
                int64_t label =
                    state % 2
                        ? target_values[physical_index(
                              targets, batch * maximum_target_length + state / 2)]
                        : options.blank;
                uint64_t index = CTC_LOGP_INDEX(time, label);
                float posterior = std::exp(
                    alpha_values[physical_index(
                        alpha, CTC_ALPHA_INDEX(time, state))] +
                    beta_values[physical_index(
                        beta, CTC_BETA_INDEX(time, state))] +
                    sample_loss);
                gradient_values[physical_index(gradient, index)] -=
                    posterior * scale;
            }
        }
#undef CTC_LOGP_INDEX
#undef CTC_ALPHA_INDEX
#undef CTC_BETA_INDEX
    }
    return 0;
}

static int execute_nll(const oasp_ai_tensor_view &input, const oasp_ai_tensor_view &target,
                       oasp_ai_tensor_view &output, uint32_t reduction) {
    const float *values = static_cast<const float *>(view_pointer(input));
    const int64_t *labels = static_cast<const int64_t *>(view_pointer(target));
    float *destination = static_cast<float *>(view_pointer(output));
    uint64_t batch = static_cast<uint64_t>(input.shape[0]);
    uint64_t classes = static_cast<uint64_t>(input.shape[1]);
    if (reduction == OASP_AI_REDUCTION_NONE) {
#pragma omp parallel for schedule(static)
        for (int64_t signed_row = 0; signed_row < static_cast<int64_t>(batch); signed_row++) {
            uint64_t row = static_cast<uint64_t>(signed_row);
            uint64_t label = static_cast<uint64_t>(labels[physical_index(target, row)]);
            destination[physical_index(output, row)] =
                -values[physical_index(input, row * classes + label)];
        }
        return 0;
    }
    double total = 0;
#pragma omp parallel for reduction(+ : total) schedule(static)
    for (int64_t signed_row = 0; signed_row < static_cast<int64_t>(batch); signed_row++) {
        uint64_t row = static_cast<uint64_t>(signed_row);
        uint64_t label = static_cast<uint64_t>(labels[physical_index(target, row)]);
        total -= values[physical_index(input, row * classes + label)];
    }
    destination[physical_index(output, 0)] = static_cast<float>(
        reduction == OASP_AI_REDUCTION_MEAN ? total / static_cast<double>(batch) : total);
    return 0;
}

static int execute_norm(const oasp_ai_tensor_view &input, const oasp_ai_tensor_view *weight,
                        const oasp_ai_tensor_view *bias, oasp_ai_tensor_view &output,
                        const oasp_ai_norm_attributes &options) {
    uint64_t width = static_cast<uint64_t>(input.shape[input.rank - 1]);
    uint64_t rows = input.element_count / width;
    const float *source = static_cast<const float *>(view_pointer(input));
    const float *scale = weight ? static_cast<const float *>(view_pointer(*weight)) : nullptr;
    const float *shift = bias ? static_cast<const float *>(view_pointer(*bias)) : nullptr;
    float *destination = static_cast<float *>(view_pointer(output));
#pragma omp parallel for schedule(static)
    for (int64_t signed_row = 0; signed_row < static_cast<int64_t>(rows); signed_row++) {
        uint64_t row = static_cast<uint64_t>(signed_row);
        float mean = 0;
        if (!options.rms_only) {
            for (uint64_t column = 0; column < width; column++)
                mean += source[physical_index(input, row * width + column)];
            mean /= static_cast<float>(width);
        }
        float variance = 0;
        for (uint64_t column = 0; column < width; column++) {
            float centered = source[physical_index(input, row * width + column)] - (options.rms_only ? 0.0f : mean);
            variance += centered * centered;
        }
        float inverse = 1.0f / std::sqrt(variance / static_cast<float>(width) + static_cast<float>(options.epsilon));
        for (uint64_t column = 0; column < width; column++) {
            float value = (source[physical_index(input, row * width + column)] - (options.rms_only ? 0.0f : mean)) * inverse;
            if (scale)
                value *= scale[physical_index(*weight, column)];
            if (shift)
                value += shift[physical_index(*bias, column)];
            destination[physical_index(output, row * width + column)] = value;
        }
    }
    return 0;
}

static int execute_norm_backward(const oasp_ai_tensor_view *inputs, size_t input_count,
                                 oasp_ai_tensor_view *outputs, size_t output_count,
                                 const oasp_ai_norm_attributes &options) {
    const oasp_ai_tensor_view &input = inputs[0];
    const oasp_ai_tensor_view &upstream = inputs[input_count - 1];
    bool has_weight = output_count > 1;
    bool has_bias = output_count > 2;
    const oasp_ai_tensor_view *weight = has_weight ? &inputs[1] : nullptr;
    uint64_t width = static_cast<uint64_t>(input.shape[input.rank - 1]);
    uint64_t rows = input.element_count / width;
    const float *source = static_cast<const float *>(view_pointer(input));
    const float *gradient = static_cast<const float *>(view_pointer(upstream));
    const float *scale = weight ? static_cast<const float *>(view_pointer(*weight)) : nullptr;
    float *input_gradient = static_cast<float *>(view_pointer(outputs[0]));
    std::vector<float> means(rows);
    std::vector<float> inverses(rows);
#pragma omp parallel for schedule(static)
    for (int64_t signed_row = 0; signed_row < static_cast<int64_t>(rows); signed_row++) {
        uint64_t row = static_cast<uint64_t>(signed_row);
        float mean = 0;
        if (!options.rms_only) {
            for (uint64_t column = 0; column < width; column++)
                mean += source[physical_index(input, row * width + column)];
            mean /= static_cast<float>(width);
        }
        float variance = 0;
        for (uint64_t column = 0; column < width; column++) {
            float centered = source[physical_index(input, row * width + column)] - (options.rms_only ? 0.0f : mean);
            variance += centered * centered;
        }
        means[row] = mean;
        inverses[row] = 1.0f / std::sqrt(variance / static_cast<float>(width) + static_cast<float>(options.epsilon));
        float sum_gradient = 0;
        float sum_gradient_normalized = 0;
        for (uint64_t column = 0; column < width; column++) {
            uint64_t index = row * width + column;
            float normalized = (source[physical_index(input, index)] - (options.rms_only ? 0.0f : mean)) * inverses[row];
            float scaled = gradient[physical_index(upstream, index)] * (scale ? scale[physical_index(*weight, column)] : 1.0f);
            sum_gradient += scaled;
            sum_gradient_normalized += scaled * normalized;
        }
        for (uint64_t column = 0; column < width; column++) {
            uint64_t index = row * width + column;
            float normalized = (source[physical_index(input, index)] - (options.rms_only ? 0.0f : mean)) * inverses[row];
            float scaled = gradient[physical_index(upstream, index)] * (scale ? scale[physical_index(*weight, column)] : 1.0f);
            float value = options.rms_only
                              ? inverses[row] * (scaled - normalized * sum_gradient_normalized / static_cast<float>(width))
                              : inverses[row] * (scaled - sum_gradient / static_cast<float>(width) -
                                                normalized * sum_gradient_normalized / static_cast<float>(width));
            input_gradient[physical_index(outputs[0], index)] = value;
        }
    }
    if (has_weight || has_bias) {
        float *weight_gradient = has_weight ? static_cast<float *>(view_pointer(outputs[1])) : nullptr;
        float *bias_gradient = has_bias ? static_cast<float *>(view_pointer(outputs[2])) : nullptr;
#pragma omp parallel for schedule(static)
        for (int64_t signed_column = 0; signed_column < static_cast<int64_t>(width); signed_column++) {
            uint64_t column = static_cast<uint64_t>(signed_column);
            float weight_sum = 0;
            float bias_sum = 0;
            for (uint64_t row = 0; row < rows; row++) {
                uint64_t index = row * width + column;
                float upstream_value = gradient[physical_index(upstream, index)];
                float normalized = (source[physical_index(input, index)] -
                                    (options.rms_only ? 0.0f : means[row])) * inverses[row];
                weight_sum += upstream_value * normalized;
                bias_sum += upstream_value;
            }
            if (weight_gradient)
                weight_gradient[physical_index(outputs[1], column)] = weight_sum;
            if (bias_gradient)
                bias_gradient[physical_index(outputs[2], column)] = bias_sum;
        }
    }
    return 0;
}

static bool attention_masked(const oasp_ai_tensor_view *mask, uint64_t batch, uint64_t row, uint64_t column,
                             uint64_t queries, uint64_t keys, int causal) {
    if (causal && column > row)
        return true;
    if (!mask)
        return false;
    uint64_t index = mask->element_count == queries * keys ? row * keys + column :
                     batch * queries * keys + row * keys + column;
    return !static_cast<const uint8_t *>(view_pointer(*mask))[physical_index(*mask, index)];
}

static void attention_softmax(float *scores, uint64_t rows, uint64_t keys) {
#pragma omp parallel for schedule(static)
    for (int64_t signed_row = 0; signed_row < static_cast<int64_t>(rows); signed_row++) {
        float *values = scores + static_cast<uint64_t>(signed_row) * keys;
        float maximum = -INFINITY;
        for (uint64_t column = 0; column < keys; column++)
            maximum = std::max(maximum, values[column]);
        if (maximum == -INFINITY) {
            std::fill(values, values + keys, 0.0f);
            continue;
        }
        float total = 0;
        for (uint64_t column = 0; column < keys; column++) {
            values[column] = values[column] == -INFINITY ? 0.0f : std::exp(values[column] - maximum);
            total += values[column];
        }
        if (total)
            for (uint64_t column = 0; column < keys; column++)
                values[column] /= total;
    }
}

static int attention_probabilities(cpu_stream_state *state, const oasp_ai_tensor_view &query,
                                   const oasp_ai_tensor_view &key, const oasp_ai_tensor_view *mask,
                                   const oasp_ai_attention_attributes &options, std::vector<float> &probabilities,
                                   oasp_ai_error *error) {
    uint64_t queries = static_cast<uint64_t>(query.shape[query.rank - 2]);
    uint64_t keys = static_cast<uint64_t>(key.shape[key.rank - 2]);
    uint64_t width = static_cast<uint64_t>(query.shape[query.rank - 1]);
    uint64_t batches = query.element_count / (queries * width);
    probabilities.resize(batches * queries * keys);
    const float *query_data = static_cast<const float *>(view_pointer(query));
    const float *key_data = static_cast<const float *>(view_pointer(key));
    bool dnnl_layout = row_major_contiguous(query) && row_major_contiguous(key);
    if (dnnl_layout) {
        for (uint64_t batch = 0; batch < batches; batch++)
            if (dnnl_matmul_raw(state, query_data + batch * queries * width, key_data + batch * keys * width,
                                probabilities.data() + batch * queries * keys, queries, width, keys,
                                width, 1, 1, width, error))
                return -1;
    } else {
#pragma omp parallel for schedule(static)
        for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(probabilities.size()); signed_index++) {
            uint64_t index = static_cast<uint64_t>(signed_index);
            uint64_t batch = index / (queries * keys);
            uint64_t row = index / keys % queries;
            uint64_t column = index % keys;
            float score = 0;
            for (uint64_t inner = 0; inner < width; inner++)
                score += query_data[physical_index(query, batch * queries * width + row * width + inner)] *
                         key_data[physical_index(key, batch * keys * width + column * width + inner)];
            probabilities[index] = score;
        }
    }
    float scale = 1.0f / std::sqrt(static_cast<float>(width));
#pragma omp parallel for schedule(static)
    for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(probabilities.size()); signed_index++) {
        uint64_t index = static_cast<uint64_t>(signed_index);
        uint64_t batch = index / (queries * keys);
        uint64_t row = index / keys % queries;
        uint64_t column = index % keys;
        probabilities[index] = attention_masked(mask, batch, row, column, queries, keys, options.causal)
                                   ? -INFINITY : probabilities[index] * scale;
    }
    attention_softmax(probabilities.data(), batches * queries, keys);
    return 0;
}

static int execute_attention(cpu_stream_state *state, const oasp_ai_tensor_view *inputs, size_t input_count,
                             oasp_ai_tensor_view &output, const oasp_ai_attention_attributes &options,
                             oasp_ai_error *error) {
    const oasp_ai_tensor_view &query = inputs[0];
    const oasp_ai_tensor_view &key = inputs[1];
    const oasp_ai_tensor_view &value = inputs[2];
    const oasp_ai_tensor_view *mask = input_count > 3 ? &inputs[3] : nullptr;
    uint64_t queries = static_cast<uint64_t>(query.shape[query.rank - 2]);
    uint64_t keys = static_cast<uint64_t>(key.shape[key.rank - 2]);
    uint64_t value_width = static_cast<uint64_t>(value.shape[value.rank - 1]);
    uint64_t batches = output.element_count / (queries * value_width);
    std::vector<float> probabilities;
    if (attention_probabilities(state, query, key, mask, options, probabilities, error))
        return -1;
    const float *value_data = static_cast<const float *>(view_pointer(value));
    float *output_data = static_cast<float *>(view_pointer(output));
    if (row_major_contiguous(value) && row_major_contiguous(output)) {
        for (uint64_t batch = 0; batch < batches; batch++)
            if (dnnl_matmul_raw(state, probabilities.data() + batch * queries * keys,
                                value_data + batch * keys * value_width,
                                output_data + batch * queries * value_width, queries, keys, value_width,
                                keys, 1, value_width, 1, error))
                return -1;
        return 0;
    }
#pragma omp parallel for schedule(static)
    for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(output.element_count); signed_index++) {
        uint64_t index = static_cast<uint64_t>(signed_index);
        uint64_t batch = index / (queries * value_width);
        uint64_t row = index / value_width % queries;
        uint64_t channel = index % value_width;
        float result = 0;
        for (uint64_t column = 0; column < keys; column++)
            result += probabilities[batch * queries * keys + row * keys + column] *
                      value_data[physical_index(value, batch * keys * value_width + column * value_width + channel)];
        output_data[physical_index(output, index)] = result;
    }
    return 0;
}

static int execute_attention_backward(cpu_stream_state *state, const oasp_ai_tensor_view *inputs, size_t input_count,
                                      oasp_ai_tensor_view *outputs, const oasp_ai_attention_attributes &options,
                                      oasp_ai_error *error) {
    const oasp_ai_tensor_view &query = inputs[0];
    const oasp_ai_tensor_view &key = inputs[1];
    const oasp_ai_tensor_view &value = inputs[2];
    const oasp_ai_tensor_view *mask = input_count > 4 ? &inputs[3] : nullptr;
    const oasp_ai_tensor_view &upstream = inputs[input_count - 1];
    uint64_t queries = static_cast<uint64_t>(query.shape[query.rank - 2]);
    uint64_t keys = static_cast<uint64_t>(key.shape[key.rank - 2]);
    uint64_t width = static_cast<uint64_t>(query.shape[query.rank - 1]);
    uint64_t value_width = static_cast<uint64_t>(value.shape[value.rank - 1]);
    uint64_t batches = query.element_count / (queries * width);
    std::vector<float> probabilities;
    if (attention_probabilities(state, query, key, mask, options, probabilities, error))
        return -1;
    std::vector<float> score_gradient(batches * queries * keys);
    const float *value_data = static_cast<const float *>(view_pointer(value));
    const float *upstream_data = static_cast<const float *>(view_pointer(upstream));
#pragma omp parallel for schedule(static)
    for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(score_gradient.size()); signed_index++) {
        uint64_t index = static_cast<uint64_t>(signed_index);
        uint64_t batch = index / (queries * keys);
        uint64_t row = index / keys % queries;
        uint64_t column = index % keys;
        float result = 0;
        for (uint64_t channel = 0; channel < value_width; channel++)
            result += upstream_data[physical_index(upstream, batch * queries * value_width + row * value_width + channel)] *
                      value_data[physical_index(value, batch * keys * value_width + column * value_width + channel)];
        score_gradient[index] = result;
    }
    float scale = 1.0f / std::sqrt(static_cast<float>(width));
#pragma omp parallel for schedule(static)
    for (int64_t signed_row = 0; signed_row < static_cast<int64_t>(batches * queries); signed_row++) {
        uint64_t offset = static_cast<uint64_t>(signed_row) * keys;
        float dot = 0;
        for (uint64_t column = 0; column < keys; column++)
            dot += probabilities[offset + column] * score_gradient[offset + column];
        for (uint64_t column = 0; column < keys; column++)
            score_gradient[offset + column] = probabilities[offset + column] *
                                              (score_gradient[offset + column] - dot) * scale;
    }
    const float *query_data = static_cast<const float *>(view_pointer(query));
    const float *key_data = static_cast<const float *>(view_pointer(key));
    float *query_gradient = static_cast<float *>(view_pointer(outputs[0]));
    float *key_gradient = static_cast<float *>(view_pointer(outputs[1]));
    float *value_gradient = static_cast<float *>(view_pointer(outputs[2]));
#pragma omp parallel for schedule(static)
    for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(outputs[0].element_count); signed_index++) {
        uint64_t index = static_cast<uint64_t>(signed_index);
        uint64_t batch = index / (queries * width);
        uint64_t row = index / width % queries;
        uint64_t channel = index % width;
        float result = 0;
        for (uint64_t column = 0; column < keys; column++)
            result += score_gradient[batch * queries * keys + row * keys + column] *
                      key_data[physical_index(key, batch * keys * width + column * width + channel)];
        query_gradient[physical_index(outputs[0], index)] = result;
    }
#pragma omp parallel for schedule(static)
    for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(outputs[1].element_count); signed_index++) {
        uint64_t index = static_cast<uint64_t>(signed_index);
        uint64_t batch = index / (keys * width);
        uint64_t column = index / width % keys;
        uint64_t channel = index % width;
        float result = 0;
        for (uint64_t row = 0; row < queries; row++)
            result += score_gradient[batch * queries * keys + row * keys + column] *
                      query_data[physical_index(query, batch * queries * width + row * width + channel)];
        key_gradient[physical_index(outputs[1], index)] = result;
    }
#pragma omp parallel for schedule(static)
    for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(outputs[2].element_count); signed_index++) {
        uint64_t index = static_cast<uint64_t>(signed_index);
        uint64_t batch = index / (keys * value_width);
        uint64_t column = index / value_width % keys;
        uint64_t channel = index % value_width;
        float result = 0;
        for (uint64_t row = 0; row < queries; row++)
            result += probabilities[batch * queries * keys + row * keys + column] *
                      upstream_data[physical_index(upstream, batch * queries * value_width + row * value_width + channel)];
        value_gradient[physical_index(outputs[2], index)] = result;
    }
    return 0;
}

static int execute_views(cpu_stream_state *state, uint32_t operation, const oasp_ai_tensor_view *inputs,
                         size_t input_count, oasp_ai_tensor_view *outputs, size_t output_count,
                         const void *attributes, size_t attributes_size, oasp_ai_error *error) {
    if (!state || !outputs || !output_count)
        return fail(error, 5, "OneDnnExecuteOp", "invalid oneDNN operator bindings",
                    "Pass valid input and output tensor views.");
    for (size_t index = 0; index < input_count; index++) {
        if (!storage_from(inputs[index].storage))
            return fail(error, 5, "OneDnnExecuteOp", "input storage handle is invalid",
                        "Use tensors allocated by this CPU backend.");
        if (inputs[index].dtype != OASP_AI_DTYPE_F32 && operation != OASP_AI_OP_CONTIGUOUS &&
            !((operation == OASP_AI_OP_CROSS_ENTROPY || operation == OASP_AI_BACKEND_OP_CROSS_ENTROPY_BACKWARD) && index == 1) &&
            !((operation == OASP_AI_OP_CTC_LOSS ||
               operation == OASP_AI_BACKEND_OP_CTC_LOSS_BACKWARD) &&
              index >= 1 && index <= 3 && inputs[index].dtype == OASP_AI_DTYPE_I64) &&
            !(((operation == OASP_AI_OP_NLL && index == 1) ||
               (operation == OASP_AI_BACKEND_OP_NLL_BACKWARD && index == 0)) &&
              inputs[index].dtype == OASP_AI_DTYPE_I64) &&
            !((operation == OASP_AI_OP_ATTENTION || operation == OASP_AI_BACKEND_OP_ATTENTION_BACKWARD) &&
              index == 3 && inputs[index].dtype == OASP_AI_DTYPE_BOOL))
            return fail(error, 5, "OneDnnExecuteOp", "oneDNN backend currently requires float32 tensors",
                        "Cast tensors to float32.");
    }
    for (size_t index = 0; index < output_count; index++)
        if (!storage_from(outputs[index].storage) ||
            (outputs[index].dtype != OASP_AI_DTYPE_F32 && operation != OASP_AI_OP_CONTIGUOUS))
            return fail(error, 5, "OneDnnExecuteOp", "invalid or unsupported output tensor",
                        "Use float32 tensors allocated by this CPU backend.");
    if (operation == OASP_AI_OP_CONTIGUOUS && input_count == 1 && output_count == 1 &&
        inputs[0].dtype == outputs[0].dtype && inputs[0].element_count == outputs[0].element_count) {
        uint32_t bytes = dtype_bytes(inputs[0].dtype);
        const unsigned char *source = static_cast<const unsigned char *>(view_pointer(inputs[0]));
        unsigned char *destination = static_cast<unsigned char *>(view_pointer(outputs[0]));
#pragma omp parallel for schedule(static)
        for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(outputs[0].element_count); signed_index++) {
            uint64_t index = static_cast<uint64_t>(signed_index);
            std::memcpy(destination + physical_index(outputs[0], index) * bytes,
                        source + physical_index(inputs[0], index) * bytes, bytes);
        }
        return 0;
    }
    if (operation >= OASP_AI_OP_ADD && operation <= OASP_AI_OP_MAXIMUM && input_count == 2)
        return execute_binary(operation, inputs[0], inputs[1], outputs[0]);
    if (operation >= OASP_AI_OP_EXP && operation <= OASP_AI_OP_SILU && input_count == 1) {
        double argument = attributes_size == sizeof(oasp_ai_unary_attributes)
                              ? static_cast<const oasp_ai_unary_attributes *>(attributes)->argument : 0.01;
        return execute_unary(operation, inputs[0], outputs[0], argument);
    }
    if ((operation == OASP_AI_OP_SUM || operation == OASP_AI_OP_MEAN || operation == OASP_AI_OP_MAX ||
         operation == OASP_AI_OP_MIN || operation == OASP_AI_OP_NORM) && input_count == 1) {
        int32_t axis = attributes_size == sizeof(oasp_ai_reduce_attributes)
                           ? static_cast<const oasp_ai_reduce_attributes *>(attributes)->axis : -1;
        return execute_reduce(operation, inputs[0], outputs[0], axis);
    }
    if ((operation == OASP_AI_OP_MATMUL || operation == OASP_AI_OP_BATCHED_MATMUL) && input_count == 2)
        return execute_matmul(state, inputs[0], inputs[1], outputs[0], error);
    if (operation == OASP_AI_OP_SOFTMAX && input_count == 1) {
        int32_t axis = attributes_size == sizeof(oasp_ai_axis_attributes)
                           ? static_cast<const oasp_ai_axis_attributes *>(attributes)->axis
                           : static_cast<int32_t>(inputs[0].rank) - 1;
        return execute_softmax(inputs[0], outputs[0], axis);
    }
    if (operation == OASP_AI_OP_CROSS_ENTROPY && input_count >= 2 && output_count == 2) {
        oasp_ai_cross_entropy_attributes options = {OASP_AI_REDUCTION_MEAN, -100, 0, 0};
        if (attributes_size == sizeof(options))
            options = *static_cast<const oasp_ai_cross_entropy_attributes *>(attributes);
        return execute_cross_entropy(inputs[0], inputs[1],
                                     options.has_weight ? &inputs[2] : nullptr,
                                     outputs[0], outputs[1], options);
    }
    if (operation == OASP_AI_OP_CTC_LOSS && input_count == 4 && output_count == 3 &&
        attributes_size == sizeof(oasp_ai_ctc_attributes))
        return execute_ctc_forward(
            inputs[0], inputs[1], inputs[2], inputs[3], outputs[0], outputs[1],
            outputs[2], *static_cast<const oasp_ai_ctc_attributes *>(attributes));
    if (operation == OASP_AI_OP_NLL && input_count == 2 &&
        attributes_size == sizeof(oasp_ai_loss_attributes))
        return execute_nll(inputs[0], inputs[1], outputs[0],
                           static_cast<const oasp_ai_loss_attributes *>(attributes)->reduction);
    if ((operation == OASP_AI_OP_LAYER_NORM || operation == OASP_AI_OP_RMS_NORM) && input_count >= 1) {
        oasp_ai_norm_attributes options = {1e-5, operation == OASP_AI_OP_RMS_NORM};
        if (attributes_size == sizeof(options))
            options = *static_cast<const oasp_ai_norm_attributes *>(attributes);
        return execute_norm(inputs[0], input_count > 1 ? &inputs[1] : nullptr,
                            input_count > 2 ? &inputs[2] : nullptr, outputs[0], options);
    }
    if (operation == OASP_AI_BACKEND_OP_NORM_BACKWARD && input_count >= 2 &&
        attributes_size == sizeof(oasp_ai_norm_attributes))
        return execute_norm_backward(inputs, input_count, outputs, output_count,
                                     *static_cast<const oasp_ai_norm_attributes *>(attributes));
    if (operation == OASP_AI_BACKEND_OP_FILL && input_count == 0 &&
        attributes_size == sizeof(oasp_ai_fill_attributes)) {
        float value = static_cast<float>(static_cast<const oasp_ai_fill_attributes *>(attributes)->value);
        float *destination = static_cast<float *>(view_pointer(outputs[0]));
#pragma omp parallel for schedule(static)
        for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(outputs[0].element_count); signed_index++) {
            uint64_t index = static_cast<uint64_t>(signed_index);
            destination[physical_index(outputs[0], index)] = value;
        }
        return 0;
    }
    if (operation == OASP_AI_BACKEND_OP_SUM_TO_SHAPE && input_count == 1) {
        const float *source = static_cast<const float *>(view_pointer(inputs[0]));
        float *destination = static_cast<float *>(view_pointer(outputs[0]));
#pragma omp parallel for schedule(static)
        for (int64_t signed_output = 0; signed_output < static_cast<int64_t>(outputs[0].element_count); signed_output++) {
            uint64_t output_index = static_cast<uint64_t>(signed_output);
            float total = 0;
            for (uint64_t input_index = 0; input_index < inputs[0].element_count; input_index++)
                if (broadcast_index(outputs[0], inputs[0], input_index) == output_index)
                    total += source[physical_index(inputs[0], input_index)];
            destination[physical_index(outputs[0], output_index)] = total;
        }
        return 0;
    }
    if (operation == OASP_AI_BACKEND_OP_UNARY_BACKWARD && input_count == 3 &&
        attributes_size == sizeof(oasp_ai_backward_attributes)) {
        const auto &options = *static_cast<const oasp_ai_backward_attributes *>(attributes);
        const float *input = static_cast<const float *>(view_pointer(inputs[0]));
        const float *output = static_cast<const float *>(view_pointer(inputs[1]));
        const float *upstream = static_cast<const float *>(view_pointer(inputs[2]));
        float *gradient = static_cast<float *>(view_pointer(outputs[0]));
#pragma omp parallel for schedule(static)
        for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(outputs[0].element_count); signed_index++) {
            uint64_t index = static_cast<uint64_t>(signed_index);
            float x = input[physical_index(inputs[0], index)];
            float y = output[physical_index(inputs[1], index)];
            float derivative = 1;
            switch (options.forward_op) {
            case OASP_AI_OP_EXP: derivative = y; break;
            case OASP_AI_OP_LOG: derivative = 1.0f / x; break;
            case OASP_AI_OP_SQRT: derivative = 0.5f / y; break;
            case OASP_AI_OP_RELU: derivative = x > 0; break;
            case OASP_AI_OP_LEAKY_RELU: derivative = x > 0 ? 1.0f : static_cast<float>(options.argument); break;
            case OASP_AI_OP_SIGMOID: derivative = y * (1.0f - y); break;
            case OASP_AI_OP_TANH: derivative = 1.0f - y * y; break;
            case OASP_AI_OP_SILU: {
                float sigmoid = 1.0f / (1.0f + std::exp(-x));
                derivative = sigmoid + x * sigmoid * (1.0f - sigmoid);
                break;
            }
            case OASP_AI_OP_GELU: {
                float inner = 0.7978845608f * (x + 0.044715f * x * x * x);
                float hyperbolic = std::tanh(inner);
                derivative = 0.5f * (1.0f + hyperbolic) +
                             0.5f * x * (1.0f - hyperbolic * hyperbolic) * 0.7978845608f *
                             (1.0f + 0.134145f * x * x);
                break;
            }
            }
            gradient[physical_index(outputs[0], index)] =
                upstream[physical_index(inputs[2], index)] * derivative;
        }
        return 0;
    }
    if (operation == OASP_AI_BACKEND_OP_BINARY_BACKWARD && input_count == 3 && output_count == 2 &&
        attributes_size == sizeof(oasp_ai_backward_attributes)) {
        const auto &options = *static_cast<const oasp_ai_backward_attributes *>(attributes);
        const float *left = static_cast<const float *>(view_pointer(inputs[0]));
        const float *right = static_cast<const float *>(view_pointer(inputs[1]));
        const float *upstream = static_cast<const float *>(view_pointer(inputs[2]));
        for (size_t side = 0; side < 2; side++) {
            float *gradient = static_cast<float *>(view_pointer(outputs[side]));
#pragma omp parallel for schedule(static)
            for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(outputs[side].element_count); signed_index++) {
                uint64_t index = static_cast<uint64_t>(signed_index);
                float first = left[physical_index(inputs[0], broadcast_index(inputs[0], inputs[2], index))];
                float second = right[physical_index(inputs[1], broadcast_index(inputs[1], inputs[2], index))];
                gradient[physical_index(outputs[side], index)] =
                    upstream[physical_index(inputs[2], index)] *
                    binary_derivative(options.forward_op, side == 0, first, second);
            }
        }
        return 0;
    }
    if (operation == OASP_AI_BACKEND_OP_REDUCE_BACKWARD && input_count == 1 &&
        attributes_size == sizeof(oasp_ai_reduce_backward_attributes)) {
        const auto &options = *static_cast<const oasp_ai_reduce_backward_attributes *>(attributes);
        uint64_t reduction = options.axis < 0 ? outputs[0].element_count :
                             static_cast<uint64_t>(outputs[0].shape[options.axis]);
        uint64_t inner = 1;
        if (options.axis >= 0)
            for (uint32_t current = static_cast<uint32_t>(options.axis) + 1; current < outputs[0].rank; current++)
                inner *= static_cast<uint64_t>(outputs[0].shape[current]);
        const float *upstream = static_cast<const float *>(view_pointer(inputs[0]));
        float *gradient = static_cast<float *>(view_pointer(outputs[0]));
#pragma omp parallel for schedule(static)
        for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(outputs[0].element_count); signed_index++) {
            uint64_t index = static_cast<uint64_t>(signed_index);
            uint64_t upstream_index = options.axis < 0 ? 0 :
                                      (index / (reduction * inner)) * inner + index % inner;
            float value = upstream[physical_index(inputs[0], upstream_index)];
            if (options.forward_op == OASP_AI_OP_MEAN)
                value /= static_cast<float>(reduction);
            gradient[physical_index(outputs[0], index)] = value;
        }
        return 0;
    }
    if (operation == OASP_AI_BACKEND_OP_SOFTMAX_BACKWARD && input_count == 2 &&
        attributes_size == sizeof(oasp_ai_axis_attributes)) {
        int32_t axis = static_cast<const oasp_ai_axis_attributes *>(attributes)->axis;
        uint64_t inner = 1;
        for (uint32_t current = static_cast<uint32_t>(axis) + 1; current < inputs[0].rank; current++)
            inner *= static_cast<uint64_t>(inputs[0].shape[current]);
        uint64_t width = static_cast<uint64_t>(inputs[0].shape[axis]);
        uint64_t groups = inputs[0].element_count / width;
        const float *values = static_cast<const float *>(view_pointer(inputs[0]));
        const float *upstream = static_cast<const float *>(view_pointer(inputs[1]));
        float *gradient = static_cast<float *>(view_pointer(outputs[0]));
#pragma omp parallel for schedule(static)
        for (int64_t signed_group = 0; signed_group < static_cast<int64_t>(groups); signed_group++) {
            uint64_t group_position = static_cast<uint64_t>(signed_group);
            uint64_t group = group_position / inner;
            uint64_t position = group_position % inner;
            float dot = 0;
            for (uint64_t item = 0; item < width; item++) {
                uint64_t index = group * width * inner + item * inner + position;
                dot += upstream[physical_index(inputs[1], index)] * values[physical_index(inputs[0], index)];
            }
            for (uint64_t item = 0; item < width; item++) {
                uint64_t index = group * width * inner + item * inner + position;
                gradient[physical_index(outputs[0], index)] =
                    values[physical_index(inputs[0], index)] * (upstream[physical_index(inputs[1], index)] - dot);
            }
        }
        return 0;
    }
    if (operation == OASP_AI_BACKEND_OP_CROSS_ENTROPY_BACKWARD && input_count >= 4 &&
        attributes_size == sizeof(oasp_ai_cross_entropy_attributes)) {
        const auto &options = *static_cast<const oasp_ai_cross_entropy_attributes *>(attributes);
        const float *logits = static_cast<const float *>(view_pointer(inputs[0]));
        const int64_t *labels = static_cast<const int64_t *>(view_pointer(inputs[1]));
        const float *upstream = static_cast<const float *>(view_pointer(inputs[2]));
        const float *normalizer = static_cast<const float *>(view_pointer(inputs[3]));
        const float *weights = options.has_weight
                                   ? static_cast<const float *>(view_pointer(inputs[4]))
                                   : nullptr;
        float *gradient = static_cast<float *>(view_pointer(outputs[0]));
        uint64_t classes = static_cast<uint64_t>(inputs[0].shape[1]);
#pragma omp parallel for schedule(static)
        for (int64_t signed_row = 0; signed_row < inputs[0].shape[0]; signed_row++) {
            uint64_t row = static_cast<uint64_t>(signed_row);
            int64_t label = labels[physical_index(inputs[1], row)];
            if (label == options.ignore_index) {
                for (uint64_t column = 0; column < classes; column++)
                    gradient[physical_index(outputs[0], row * classes + column)] = 0;
                continue;
            }
            float maximum = -INFINITY;
            for (uint64_t item = 0; item < classes; item++)
                maximum = std::max(maximum, logits[physical_index(inputs[0], row * classes + item)]);
            float total = 0;
            for (uint64_t item = 0; item < classes; item++)
                total += std::exp(logits[physical_index(inputs[0], row * classes + item)] - maximum);
            float target_weight = weights ? weights[static_cast<uint64_t>(label)] : 1.0f;
            float weight_sum = 0;
            for (uint64_t column = 0; column < classes; column++)
                weight_sum += weights ? weights[column] : 1.0f;
            float scale = upstream[physical_index(
                inputs[2], options.reduction == OASP_AI_REDUCTION_NONE ? row : 0)];
            if (options.reduction == OASP_AI_REDUCTION_MEAN)
                scale /= normalizer[physical_index(inputs[3], 0)];
            for (uint64_t column = 0; column < classes; column++) {
                uint64_t index = row * classes + column;
                float probability = std::exp(logits[physical_index(inputs[0], index)] - maximum) / total;
                float class_weight = weights ? weights[column] : 1.0f;
                float value =
                    static_cast<float>(1 - options.label_smoothing) * target_weight *
                        (probability - (column == static_cast<uint64_t>(label))) +
                    static_cast<float>(options.label_smoothing) / static_cast<float>(classes) *
                        (probability * weight_sum - class_weight);
                gradient[physical_index(outputs[0], index)] =
                    value * scale;
            }
        }
        return 0;
    }
    if (operation == OASP_AI_BACKEND_OP_CTC_LOSS_BACKWARD &&
        input_count == 7 && output_count == 2 &&
        attributes_size == sizeof(oasp_ai_ctc_attributes))
        return execute_ctc_backward(
            inputs, outputs,
            *static_cast<const oasp_ai_ctc_attributes *>(attributes));
    if (operation == OASP_AI_BACKEND_OP_NLL_BACKWARD && input_count == 2 &&
        attributes_size == sizeof(oasp_ai_loss_attributes)) {
        const int64_t *labels = static_cast<const int64_t *>(view_pointer(inputs[0]));
        const float *upstream = static_cast<const float *>(view_pointer(inputs[1]));
        float *gradient = static_cast<float *>(view_pointer(outputs[0]));
        uint32_t reduction = static_cast<const oasp_ai_loss_attributes *>(attributes)->reduction;
        uint64_t batch = static_cast<uint64_t>(inputs[0].shape[0]);
        uint64_t classes = static_cast<uint64_t>(outputs[0].shape[1]);
#pragma omp parallel for schedule(static)
        for (int64_t signed_row = 0; signed_row < static_cast<int64_t>(batch); signed_row++) {
            uint64_t row = static_cast<uint64_t>(signed_row);
            float scale = upstream[physical_index(inputs[1],
                reduction == OASP_AI_REDUCTION_NONE ? row : 0)];
            if (reduction == OASP_AI_REDUCTION_MEAN)
                scale /= static_cast<float>(batch);
            uint64_t label = static_cast<uint64_t>(labels[physical_index(inputs[0], row)]);
            gradient[physical_index(outputs[0], row * classes + label)] = -scale;
        }
        return 0;
    }
    if (operation == OASP_AI_OP_ATTENTION && input_count >= 3) {
        oasp_ai_attention_attributes options = {0};
        if (attributes_size == sizeof(options))
            options = *static_cast<const oasp_ai_attention_attributes *>(attributes);
        return execute_attention(state, inputs, input_count, outputs[0], options, error);
    }
    if (operation == OASP_AI_BACKEND_OP_ATTENTION_BACKWARD && input_count >= 4 && output_count == 3) {
        oasp_ai_attention_attributes options = {0};
        if (attributes_size == sizeof(options))
            options = *static_cast<const oasp_ai_attention_attributes *>(attributes);
        return execute_attention_backward(state, inputs, input_count, outputs, options, error);
    }
    if (operation == OASP_AI_BACKEND_OP_OPTIMIZER_STEP && input_count == 4 && output_count == 1 &&
        attributes_size == sizeof(oasp_ai_optimizer_attributes)) {
        const auto &options = *static_cast<const oasp_ai_optimizer_attributes *>(attributes);
        float *parameter = static_cast<float *>(view_pointer(inputs[0]));
        const float *gradient = static_cast<const float *>(view_pointer(inputs[1]));
        float *first = static_cast<float *>(view_pointer(inputs[2]));
        float *second = static_cast<float *>(view_pointer(inputs[3]));
#pragma omp parallel for schedule(static)
        for (int64_t signed_index = 0; signed_index < static_cast<int64_t>(inputs[0].element_count); signed_index++) {
            uint64_t index = static_cast<uint64_t>(signed_index);
            uint64_t parameter_index = physical_index(inputs[0], index);
            uint64_t first_index = physical_index(inputs[2], index);
            uint64_t second_index = physical_index(inputs[3], index);
            float value = parameter[parameter_index];
            float derivative = gradient[physical_index(inputs[1], index)];
            float update;
            if (options.optimizer == OASP_AI_OPTIMIZER_SGD) {
                float velocity = static_cast<float>(options.momentum) * first[first_index] + derivative;
                first[first_index] = velocity;
                update = velocity + static_cast<float>(options.weight_decay) * value;
            } else {
                float first_value = static_cast<float>(options.beta1) * first[first_index] +
                                    static_cast<float>(1.0 - options.beta1) * derivative;
                float second_value = static_cast<float>(options.beta2) * second[second_index] +
                                     static_cast<float>(1.0 - options.beta2) * derivative * derivative;
                first[first_index] = first_value;
                second[second_index] = second_value;
                float corrected_first = first_value /
                                        static_cast<float>(1.0 - std::pow(options.beta1, static_cast<double>(options.step)));
                float corrected_second = second_value /
                                         static_cast<float>(1.0 - std::pow(options.beta2, static_cast<double>(options.step)));
                update = corrected_first / (std::sqrt(corrected_second) + static_cast<float>(options.epsilon));
                if (options.optimizer == OASP_AI_OPTIMIZER_ADAMW)
                    update += static_cast<float>(options.weight_decay) * value;
            }
            parameter[parameter_index] = value - static_cast<float>(options.learning_rate) * update;
        }
        return 0;
    }
    return fail(error, 5, "OneDnnExecuteOp", "operator is not implemented by the oneDNN CPU backend",
                "Use a supported operator or select the reference CPU backend.");
}

static int enumerate_devices(void *, oasp_ai_device_info *devices, size_t *count, oasp_ai_error *) {
    static const char name[] = "oneDNN CPU";
    if (!count)
        return -1;
    if (!devices || !*count) {
        *count = 1;
        return 0;
    }
    devices[0] = {
        sizeof(devices[0]), 0, {name, sizeof(name) - 1}, UINT64_MAX, UINT64_MAX, 1,
    };
    *count = 1;
    return 0;
}

static int open_device(void *, uint32_t ordinal, const oasp_ai_device_options *options,
                       oasp_ai_device *output, oasp_ai_error *error) {
    if (ordinal || !output)
        return fail(error, 5, "OneDnnOpenDevice", "oneDNN CPU only provides device zero",
                    "Use cpu-onednn:0.");
    try {
        cpu_device_state *device = new (std::nothrow) cpu_device_state(options ? options->memory_limit : 0);
        if (!device)
            return fail(error, 7, "OneDnnOpenDevice", "unable to allocate device state",
                        "Reduce host memory pressure.");
        *output = reinterpret_cast<oasp_ai_device>(device);
        return 0;
    } catch (const dnnl::error &exception) {
        return dnnl_fail(error, "OneDnnOpenDevice", exception);
    }
}

static void close_device(oasp_ai_device handle) {
    delete reinterpret_cast<cpu_device_state *>(handle);
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
                     query->op_id == OASP_AI_OP_SOFTMAX || query->op_id == OASP_AI_OP_ATTENTION ||
                     query->op_id == OASP_AI_OP_CONTIGUOUS ||
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

static int allocate_storage(oasp_ai_device handle, const oasp_ai_alloc_desc *desc,
                            oasp_ai_backend_storage *output, oasp_ai_error *error) {
    cpu_device_state *device = reinterpret_cast<cpu_device_state *>(handle);
    if (!device || !desc || !output || desc->memory_kind != OASP_AI_MEMORY_DEVICE || desc->bytes > SIZE_MAX)
        return fail(error, 5, "OneDnnAlloc", "invalid oneDNN CPU allocation request",
                    "Request device memory from a valid cpu-onednn device.");
    std::lock_guard<std::mutex> lock(device->mutex);
    if (device->memory_limit && desc->bytes > device->memory_limit - std::min(device->live_bytes, device->memory_limit))
        return fail(error, 7, "OneDnnAlloc", "oneDNN CPU memory budget exceeded",
                    "Release tensors or increase the configured device memory limit.");
    cpu_storage *storage = new (std::nothrow) cpu_storage{};
    if (!storage)
        return fail(error, 7, "OneDnnAlloc", "unable to allocate storage metadata",
                    "Reduce host memory pressure.");
    size_t alignment = std::max<size_t>(desc->alignment, 64);
    if (posix_memalign(&storage->pointer, alignment, static_cast<size_t>(desc->bytes))) {
        delete storage;
        return fail(error, 7, "OneDnnAlloc", "unable to allocate aligned tensor memory",
                    "Reduce host memory pressure.");
    }
    std::memset(storage->pointer, 0, static_cast<size_t>(desc->bytes));
    storage->bytes = desc->bytes;
    device->live_bytes += desc->bytes;
    device->peak_bytes = std::max(device->peak_bytes, device->live_bytes);
    device->allocations++;
    *output = reinterpret_cast<oasp_ai_backend_storage>(storage);
    return 0;
}

static void free_storage(oasp_ai_device handle, oasp_ai_backend_storage storage_handle) {
    cpu_device_state *device = reinterpret_cast<cpu_device_state *>(handle);
    cpu_storage *storage = storage_from(storage_handle);
    if (!device || !storage)
        return;
    {
        std::lock_guard<std::mutex> lock(device->mutex);
        device->live_bytes -= std::min(device->live_bytes, storage->bytes);
    }
    std::free(storage->pointer);
    delete storage;
}

static int copy_storage(oasp_ai_stream handle, const oasp_ai_copy_desc *desc,
                        oasp_ai_event *event, oasp_ai_error *error) {
    if (!handle || !desc)
        return -1;
    const unsigned char *source = static_cast<const unsigned char *>(desc->source_host);
    unsigned char *destination = static_cast<unsigned char *>(desc->destination_host);
    if (!source) {
        cpu_storage *storage = storage_from(desc->source);
        source = storage ? static_cast<const unsigned char *>(storage->pointer) + desc->source_offset : nullptr;
    } else
        source += desc->source_offset;
    if (!destination) {
        cpu_storage *storage = storage_from(desc->destination);
        destination = storage ? static_cast<unsigned char *>(storage->pointer) + desc->destination_offset : nullptr;
    } else
        destination += desc->destination_offset;
    if (!source || !destination)
        return fail(error, 5, "OneDnnCopy", "copy source or destination is missing",
                    "Pass valid host pointers or oneDNN CPU storage handles.");
    std::memmove(destination, source, static_cast<size_t>(desc->bytes));
    if (event)
        *event = 1;
    return 0;
}

static int create_stream(oasp_ai_device handle, const oasp_ai_stream_desc *,
                         oasp_ai_stream *output, oasp_ai_error *error) {
    cpu_device_state *device = reinterpret_cast<cpu_device_state *>(handle);
    if (!device || !output)
        return -1;
    try {
        cpu_stream_state *state = new (std::nothrow) cpu_stream_state(device);
        if (!state)
            return fail(error, 7, "OneDnnCreateStream", "unable to allocate stream state",
                        "Reduce host memory pressure.");
        *output = reinterpret_cast<oasp_ai_stream>(state);
        return 0;
    } catch (const dnnl::error &exception) {
        return dnnl_fail(error, "OneDnnCreateStream", exception);
    }
}

static void destroy_stream(oasp_ai_stream handle) {
    delete reinterpret_cast<cpu_stream_state *>(handle);
}

static int record_event(oasp_ai_stream handle, oasp_ai_event *event, oasp_ai_error *) {
    if (!handle || !event)
        return -1;
    *event = 1;
    return 0;
}

static int wait_event(oasp_ai_stream handle, oasp_ai_event event, oasp_ai_error *) {
    return handle && event ? 0 : -1;
}

static int synchronize(oasp_ai_stream handle, oasp_ai_error *error) {
    cpu_stream_state *state = reinterpret_cast<cpu_stream_state *>(handle);
    if (!state)
        return -1;
    try {
        state->dnnl_stream.wait();
        return 0;
    } catch (const dnnl::error &exception) {
        return dnnl_fail(error, "OneDnnSynchronize", exception);
    }
}

static int execute_op(oasp_ai_stream handle, const oasp_ai_op_desc *op,
                      oasp_ai_event *event, oasp_ai_error *error) {
    cpu_stream_state *state = reinterpret_cast<cpu_stream_state *>(handle);
    if (!state || !op)
        return -1;
    int status = execute_views(state, op->op_id, op->inputs, op->input_count, op->outputs,
                               op->output_count, op->attributes, op->attributes_size, error);
    if (!status && event)
        *event = 1;
    return status;
}

static int memory_stats(oasp_ai_device handle, oasp_ai_memory_stats *stats, oasp_ai_error *) {
    cpu_device_state *device = reinterpret_cast<cpu_device_state *>(handle);
    if (!device || !stats)
        return -1;
    std::lock_guard<std::mutex> lock(device->mutex);
    *stats = {sizeof(*stats), device->live_bytes, device->live_bytes, device->peak_bytes, 0, device->allocations};
    return 0;
}

static int trim_memory(oasp_ai_device handle, uint64_t, oasp_ai_error *) {
    return handle ? 0 : -1;
}

static int module_init(const oasp_ai_host_api *, void **module) {
    *module = nullptr;
    return 0;
}

static int worker_init(void *, void **worker, oasp_ai_error *) {
    *worker = nullptr;
    return 0;
}

static const oasp_ai_backend_api api = {
    sizeof(api),
    enumerate_devices,
    open_device,
    close_device,
    query_capabilities,
    allocate_storage,
    free_storage,
    copy_storage,
    create_stream,
    destroy_stream,
    record_event,
    wait_event,
    synchronize,
    execute_op,
    nullptr,
    nullptr,
    nullptr,
    memory_stats,
    trim_memory,
    nullptr,
};

static const oasp_ai_backend_descriptor descriptor = {
    sizeof(descriptor),
    OASP_AI_BACKEND_ABI_MAJOR,
    OASP_AI_BACKEND_ABI_MINOR,
    "cpu-onednn",
    "1.0.0",
    0,
    module_init,
    nullptr,
    worker_init,
    nullptr,
    &api,
};

} // namespace

extern "C" OASP_AI_BACKEND_EXPORT const oasp_ai_backend_descriptor *openasp_ai_backend(void) {
    return &descriptor;
}
